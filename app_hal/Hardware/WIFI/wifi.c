#include "wifi.h"

#include "app_config.h"
#include "app_log.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * 本文件只封装 ESP32 AT 指令和 +IPD 数据拆包。
 * 注意：ESP32 不运行升级主程序，它只负责把 Wi-Fi/TCP 字节流搬到 STM32 的 USART2。
 */

/* AT 命令字符串缓存，长度覆盖 CWSAP/CWJAP/CIPSERVER/CIPSEND 等命令。 */
#define WIFI_AT_CMD_BUFFER_SIZE       128U

/* ESP32 +IPD 文本头 + 一整帧 OTA payload 的接收缓存，留余量给 IP/端口字段。 */
#define WIFI_RX_RAW_BUFFER_SIZE       1536U

/* AT 命令复用缓存，只在 Wi-Fi 任务内顺序使用。 */
static char g_wifi_cmd_buffer[WIFI_AT_CMD_BUFFER_SIZE];

/* ESP32 串口原始接收缓存：内容格式通常为 \r\n+IPD,id,len,"ip",port:payload。 */
static uint8_t g_wifi_raw_buffer[WIFI_RX_RAW_BUFFER_SIZE];
static uint8_t g_wifi_stream_buffer[WIFI_RX_RAW_BUFFER_SIZE];
static uint16_t g_wifi_stream_len = 0U;

/* 在二进制缓冲区中查找一个 ASCII token，避免 payload 中出现 0x00 时字符串函数提前结束。 */
static const uint8_t *wifi_find_token(const uint8_t *buffer, uint16_t size, const char *token)
{
    size_t token_len;
    uint16_t i;

    if ((buffer == 0) || (token == 0)) {
        return 0;
    }

    token_len = strlen(token);
    if ((token_len == 0U) || ((size_t)size < token_len)) {
        return 0;
    }

    for (i = 0U; (size_t)i <= ((size_t)size - token_len); ++i) {
        if (memcmp(&buffer[i], token, token_len) == 0) {
            return &buffer[i];
        }
    }

    return 0;
}

/* 解析十进制无符号数字，ESP32 的 id、payload 长度、远端端口都用 ASCII 数字表示。 */
static int wifi_parse_decimal(const uint8_t **cursor, const uint8_t *end, uint32_t *value)
{
    uint32_t result = 0U;
    uint8_t has_digit = 0U;

    if ((cursor == 0) || (*cursor == 0) || (value == 0)) {
        return -1;
    }

    while ((*cursor < end) && (**cursor >= (uint8_t)'0') && (**cursor <= (uint8_t)'9')) {
        result = (result * 10U) + (uint32_t)(**cursor - (uint8_t)'0');
        *cursor = *cursor + 1;
        has_digit = 1U;
    }

    if (has_digit == 0U) {
        return -1;
    }

    *value = result;
    return 0;
}

/* 检查并跳过一个固定分隔符，例如逗号或冒号。 */
static int wifi_expect_char(const uint8_t **cursor, const uint8_t *end, uint8_t ch)
{
    if ((cursor == 0) || (*cursor == 0) || (*cursor >= end) || (**cursor != ch)) {
        return -1;
    }

    *cursor = *cursor + 1;
    return 0;
}

/* 复制远端 IP 字符串，给调试打印保留信息，同时保证以 '\0' 结尾。 */
static void wifi_copy_ip(char *ip, uint16_t ip_size, const uint8_t *begin, const uint8_t *end)
{
    size_t ip_len;

    if ((ip == 0) || (ip_size == 0U) || (begin == 0) || (end < begin)) {
        return;
    }

    ip_len = (size_t)(end - begin);
    if (ip_len >= (size_t)ip_size) {
        ip_len = (size_t)ip_size - 1U;
    }

    (void)memcpy(ip, begin, ip_len);
    ip[ip_len] = '\0';
}

/*
 * 解析 ESP32 的 +IPD 数据头。
 * 支持两种常见格式：
 *   +IPD,id,len:payload
 *   +IPD,id,len,"remote_ip",remote_port:payload
 */
static int wifi_parse_ipd_packet(uint16_t raw_len,
                                 uint8_t *id,
                                 uint8_t *buffer,
                                 uint16_t max_len,
                                 uint16_t *rx_len,
                                 char *ip,
                                 uint16_t ip_size,
                                 uint16_t *port)
{
    const uint8_t *end = &g_wifi_raw_buffer[raw_len];
    const uint8_t *cursor;
    const uint8_t *payload;
    uint32_t link_id = 0U;
    uint32_t payload_len = 0U;
    uint32_t remote_port = 0U;

    cursor = wifi_find_token(g_wifi_raw_buffer, raw_len, "+IPD,");
    if (cursor == 0) {
        return -1;
    }
    cursor += 5U;

    if ((wifi_parse_decimal(&cursor, end, &link_id) != 0) || (link_id > 0xFFU)) {
        return -1;
    }

    if (wifi_expect_char(&cursor, end, (uint8_t)',') != 0) {
        return -1;
    }

    if ((wifi_parse_decimal(&cursor, end, &payload_len) != 0) || (payload_len > (uint32_t)max_len)) {
        return -1;
    }

    if ((ip != 0) && (ip_size > 0U)) {
        ip[0] = '\0';
    }
    if (port != 0) {
        *port = 0U;
    }

    /*
     * AT+CIPDINFO=1 后会多出远端 IP 和端口。
     * 这些字段只用于记录当前客户端，真正的 OTA 数据仍然只取冒号后面的 payload。
     */
    if ((cursor < end) && (*cursor == (uint8_t)',')) {
        cursor++;
        if ((cursor < end) && (*cursor == (uint8_t)'"')) {
            const uint8_t *ip_begin;

            cursor++;
            ip_begin = cursor;
            while ((cursor < end) && (*cursor != (uint8_t)'"')) {
                cursor++;
            }
            if (cursor >= end) {
                return -1;
            }

            wifi_copy_ip(ip, ip_size, ip_begin, cursor);
            cursor++;

            if (wifi_expect_char(&cursor, end, (uint8_t)',') != 0) {
                return -1;
            }

            if ((wifi_parse_decimal(&cursor, end, &remote_port) != 0) || (remote_port > 0xFFFFU)) {
                return -1;
            }

            if (port != 0) {
                *port = (uint16_t)remote_port;
            }
        } else {
            /* 如果某些 AT 固件返回非标准附加字段，直接跳到冒号，保证 payload 不被误解析。 */
            while ((cursor < end) && (*cursor != (uint8_t)':')) {
                cursor++;
            }
        }
    }

    if (wifi_expect_char(&cursor, end, (uint8_t)':') != 0) {
        return -1;
    }

    payload = cursor;
    if ((uint32_t)(end - payload) < payload_len) {
        return -1;
    }

    (void)memcpy(buffer, payload, (size_t)payload_len);
    *id = (uint8_t)link_id;
    *rx_len = (uint16_t)payload_len;
    return 0;
}

static void wifi_stream_drop(uint16_t count)
{
    uint16_t remain;

    if (count >= g_wifi_stream_len) {
        g_wifi_stream_len = 0U;
        return;
    }

    remain = (uint16_t)(g_wifi_stream_len - count);
    (void)memmove(g_wifi_stream_buffer, &g_wifi_stream_buffer[count], remain);
    g_wifi_stream_len = remain;
}

static void wifi_stream_keep_ipd_prefix(void)
{
    static const uint8_t prefix[] = "+IPD,";
    uint16_t keep = 0U;
    uint16_t i;

    for (i = 1U; (i <= (uint16_t)(sizeof(prefix) - 1U)) && (i <= g_wifi_stream_len); ++i) {
        if (memcmp(&g_wifi_stream_buffer[g_wifi_stream_len - i], prefix, i) == 0) {
            keep = i;
        }
    }

    if (keep == 0U) {
        g_wifi_stream_len = 0U;
        return;
    }

    if (keep < g_wifi_stream_len) {
        (void)memmove(g_wifi_stream_buffer,
                      &g_wifi_stream_buffer[g_wifi_stream_len - keep],
                      keep);
        g_wifi_stream_len = keep;
    }
}

static void wifi_stream_append(const uint8_t *data, uint16_t len)
{
    uint16_t free_space;
    uint16_t drop_len;

    if ((data == 0) || (len == 0U)) {
        return;
    }

    if (len >= (uint16_t)sizeof(g_wifi_stream_buffer)) {
        data = &data[len - (uint16_t)sizeof(g_wifi_stream_buffer)];
        len = (uint16_t)sizeof(g_wifi_stream_buffer);
        g_wifi_stream_len = 0U;
    }

    free_space = (uint16_t)(sizeof(g_wifi_stream_buffer) - g_wifi_stream_len);
    if (len > free_space) {
        drop_len = (uint16_t)(len - free_space);
        wifi_stream_drop(drop_len);
    }

    (void)memcpy(&g_wifi_stream_buffer[g_wifi_stream_len], data, len);
    g_wifi_stream_len = (uint16_t)(g_wifi_stream_len + len);
}

void WIFI_TCP_ClearRxCache(void)
{
    g_wifi_stream_len = 0U;
    (void)memset(g_wifi_stream_buffer, 0, sizeof(g_wifi_stream_buffer));
    (void)memset(g_wifi_raw_buffer, 0, sizeof(g_wifi_raw_buffer));
}

static int wifi_stream_pop_ipd(uint8_t *id,
                               uint8_t *buffer,
                               uint16_t max_len,
                               uint16_t *rx_len,
                               char *ip,
                               uint16_t ip_size,
                               uint16_t *port)
{
    const uint8_t *token;
    const uint8_t *cursor;
    const uint8_t *end;
    const uint8_t *payload;
    uint32_t link_id = 0U;
    uint32_t payload_len = 0U;
    uint32_t remote_port = 0U;
    uint16_t header_len;
    uint16_t packet_len;

    token = wifi_find_token(g_wifi_stream_buffer, g_wifi_stream_len, "+IPD,");
    if (token == 0) {
        wifi_stream_keep_ipd_prefix();
        return -1;
    }

    if (token > g_wifi_stream_buffer) {
        wifi_stream_drop((uint16_t)(token - g_wifi_stream_buffer));
    }

    end = &g_wifi_stream_buffer[g_wifi_stream_len];
    cursor = &g_wifi_stream_buffer[5U];

    if (cursor >= end) {
        return -1;
    }

    if (wifi_parse_decimal(&cursor, end, &link_id) != 0) {
        if (cursor >= end) {
            return -1;
        }
        wifi_stream_drop(1U);
        return -1;
    }
    if (link_id > 0xFFU) {
        wifi_stream_drop(1U);
        return -1;
    }

    if (cursor >= end) {
        return -1;
    }
    if (wifi_expect_char(&cursor, end, (uint8_t)',') != 0) {
        wifi_stream_drop(1U);
        return -1;
    }

    if (cursor >= end) {
        return -1;
    }
    if (wifi_parse_decimal(&cursor, end, &payload_len) != 0) {
        if (cursor >= end) {
            return -1;
        }
        wifi_stream_drop(1U);
        return -1;
    }

    if (payload_len > (uint32_t)max_len) {
        wifi_stream_drop(1U);
        return -1;
    }

    if ((ip != 0) && (ip_size > 0U)) {
        ip[0] = '\0';
    }
    if (port != 0) {
        *port = 0U;
    }

    if (cursor >= end) {
        return -1;
    }

    if (*cursor == (uint8_t)',') {
        cursor++;
        if (cursor >= end) {
            return -1;
        }

        if (*cursor == (uint8_t)'"') {
            const uint8_t *ip_begin;

            cursor++;
            ip_begin = cursor;
            while ((cursor < end) && (*cursor != (uint8_t)'"')) {
                cursor++;
            }
            if (cursor >= end) {
                return -1;
            }

            wifi_copy_ip(ip, ip_size, ip_begin, cursor);
            cursor++;

            if (cursor >= end) {
                return -1;
            }
            if (wifi_expect_char(&cursor, end, (uint8_t)',') != 0) {
                wifi_stream_drop(1U);
                return -1;
            }

            if (cursor >= end) {
                return -1;
            }
            if (wifi_parse_decimal(&cursor, end, &remote_port) != 0) {
                if (cursor >= end) {
                    return -1;
                }
                wifi_stream_drop(1U);
                return -1;
            }

            if (remote_port > 0xFFFFU) {
                wifi_stream_drop(1U);
                return -1;
            }

            if (port != 0) {
                *port = (uint16_t)remote_port;
            }
        } else {
            while ((cursor < end) && (*cursor != (uint8_t)':')) {
                cursor++;
            }
            if (cursor >= end) {
                return -1;
            }
        }
    }

    if (cursor >= end) {
        return -1;
    }
    if (wifi_expect_char(&cursor, end, (uint8_t)':') != 0) {
        wifi_stream_drop(1U);
        return -1;
    }

    payload = cursor;
    header_len = (uint16_t)(payload - g_wifi_stream_buffer);
    if (payload_len > ((uint32_t)sizeof(g_wifi_stream_buffer) - header_len)) {
        wifi_stream_drop(1U);
        return -1;
    }
    packet_len = (uint16_t)(header_len + (uint16_t)payload_len);

    if (g_wifi_stream_len < packet_len) {
        return -1;
    }

    (void)memcpy(buffer, payload, (size_t)payload_len);
    *id = (uint8_t)link_id;
    *rx_len = (uint16_t)payload_len;
    wifi_stream_drop(packet_len);

    return 0;
}

HAL_StatusTypeDef WIFI_INIT(void)
{
    /*
     * 这里只初始化 STM32 与 ESP32 之间的 USART2/AT 通道。
     * OTA 主状态机不在 ESP32 内运行，后续所有协议帧都交回 STM32 处理。
     */
    return ESP32_Init();
}

HAL_StatusTypeDef WIFI_SET_Sta(void)
{
    HAL_StatusTypeDef state;

    /* STA 模式用于让 ESP32 连接已有路由器，适合板子接入局域网后升级。 */
    state = ESP32_SendCmd("AT+CWMODE=1\r\n", "OK");
    if (state != HAL_OK) {
        return state;
    }

    /* SSID/密码集中放在 app_config.h，避免网络参数散落在驱动代码中。 */
    (void)sprintf(g_wifi_cmd_buffer,
                  "AT+CWJAP=\"%s\",\"%s\"\r\n",
                  APP_WIFI_STA_SSID,
                  APP_WIFI_STA_PASSWORD);

    state = ESP32_SendCmd(g_wifi_cmd_buffer, "OK");
    if (state == HAL_OK) {
        /* 查询 STA 地址，方便串口日志确认 PC 端应该连接哪个 IP。 */
        (void)ESP32_SendCmd("AT+CIPSTA?\r\n", "OK");
    } else {
        (void)app_log_printf("ESP32 STA connect failed\r\n");
    }

    return state;
}

HAL_StatusTypeDef WIFI_SET_Ap(void)
{
    HAL_StatusTypeDef state;

    /*
     * AP 模式下 ESP32 自己开热点，上位机连接该热点后访问 APP_WIFI_UPGRADE_PORT。
     * 这仍然只是网络外设模式，升级业务没有搬到 ESP32。
     */
    state = ESP32_SendCmd("AT\r\n", "OK");
    if (state != HAL_OK) {
        (void)app_log_printf("[WIFI] AT probe failed=%ld\r\n", (long)state);
        return state;
    }

    state = ESP32_SendCmd("AT+CWMODE=2\r\n", "OK");
    if (state != HAL_OK) {
        (void)app_log_printf("[WIFI] CWMODE AP failed=%ld\r\n", (long)state);
        return state;
    }

    /* 固定 AP 网段，避免 ESP32 热点地址和上位网络地址冲突。 */
    (void)sprintf(g_wifi_cmd_buffer,
                  "AT+CIPAP=\"%s\",\"%s\",\"%s\"\r\n",
                  APP_WIFI_AP_IP,
                  APP_WIFI_AP_GATEWAY,
                  APP_WIFI_AP_NETMASK);
    state = ESP32_SendCmd(g_wifi_cmd_buffer, "OK");
    if (state != HAL_OK) {
        (void)app_log_printf("[WIFI] CIPAP failed=%ld, try CIPAP_CUR\r\n", (long)state);
        (void)sprintf(g_wifi_cmd_buffer,
                      "AT+CIPAP_CUR=\"%s\",\"%s\",\"%s\"\r\n",
                      APP_WIFI_AP_IP,
                      APP_WIFI_AP_GATEWAY,
                      APP_WIFI_AP_NETMASK);
        state = ESP32_SendCmd(g_wifi_cmd_buffer, "OK");
        if (state != HAL_OK) {
            (void)app_log_printf("[WIFI] AP IP config skipped=%ld\r\n", (long)state);
        }
    }

    /* 查询 AP 地址，便于调试时确认热点 IP 是否按配置生效。 */
    (void)ESP32_SendCmd("AT+CIPAP?\r\n", "OK");

    /* 设置热点名称、密码、信道和加密方式。 */
    (void)sprintf(g_wifi_cmd_buffer,
                  "AT+CWSAP=\"%s\",\"%s\",%u,%u\r\n",
                  APP_WIFI_AP_SSID,
                  APP_WIFI_AP_PASSWORD,
                  (unsigned int)APP_WIFI_AP_CHANNEL,
                  (unsigned int)APP_WIFI_AP_ENCRYPTION);

    state = ESP32_SendCmd(g_wifi_cmd_buffer, "OK");
    if (state != HAL_OK) {
        (void)app_log_printf("[WIFI] CWSAP failed=%ld, query current AP\r\n", (long)state);
        state = ESP32_SendCmd("AT+CWSAP?\r\n", "OK");
        if (state != HAL_OK) {
            (void)app_log_printf("[WIFI] CWSAP query failed=%ld\r\n", (long)state);
            return state;
        }
    }

    (void)app_log_printf("[WIFI] CWSAP ok ssid=%s\r\n", APP_WIFI_AP_SSID);
    return HAL_OK;
}

HAL_StatusTypeDef WIFI_TCP_ServerStart(uint16_t port)
{
    HAL_StatusTypeDef state;

    if (port == 0U) {
        return HAL_ERROR;
    }

    (void)ESP32_SendCmd("AT+CIPMODE=0\r\n", "OK");
    (void)ESP32_SendCmd("AT+CIPRECVMODE=0\r\n", "OK");

    /*
     * 多连接模式下 ESP32 会给每个 TCP 客户端分配 link id。
     * ACK 回传时必须带着这个 id，才能回到刚才发送升级帧的客户端。
     */
    state = ESP32_SendCmd("AT+CIPMUX=1\r\n", "OK");
    if (state != HAL_OK) {
        return state;
    }

    (void)ESP32_SendCmd("AT+CIPSERVER=0\r\n", "OK");
    (void)ESP32_SendCmd("AT+CIPCLOSE=0\r\n", "OK");
    (void)ESP32_SendCmd("AT+CIPCLOSE=1\r\n", "OK");
    (void)ESP32_SendCmd("AT+CIPCLOSE=2\r\n", "OK");
    (void)ESP32_SendCmd("AT+CIPCLOSE=3\r\n", "OK");
    (void)ESP32_SendCmd("AT+CIPCLOSE=4\r\n", "OK");

    /* 打开 +IPD 的远端 IP/端口显示，便于 transport 层记录当前客户端。 */
    state = ESP32_SendCmd("AT+CIPDINFO=1\r\n", "OK");
    if (state != HAL_OK) {
        return state;
    }

    (void)sprintf(g_wifi_cmd_buffer,
                  "AT+CIPSERVER=1,%u\r\n",
                  (unsigned int)port);

    state = ESP32_SendCmd(g_wifi_cmd_buffer, "OK");
    if (state == HAL_OK) {
        (void)ESP32_SendCmd("AT+CIPSERVER?\r\n", "+CIPSERVER:1");
    } else {
        (void)app_log_printf("[WIFI] CIPSERVER start failed=%ld\r\n", (long)state);
        if (ESP32_SendCmd("AT+CIPSERVER?\r\n", "+CIPSERVER:1") == HAL_OK) {
            (void)app_log_printf("[WIFI] CIPSERVER already running\r\n");
            return HAL_OK;
        }
    }

    return state;
}

HAL_StatusTypeDef WIFI_TCP_SendData(uint8_t id, const uint8_t *pData, uint16_t len)
{
    HAL_StatusTypeDef state;

    if ((pData == 0) || (len == 0U)) {
        return HAL_ERROR;
    }

    /* 先让 ESP32 进入指定 link id 的发送状态，收到 '>' 后再写入二进制 ACK 帧。 */
    (void)sprintf(g_wifi_cmd_buffer,
                  "AT+CIPSEND=%u,%u\r\n",
                  (unsigned int)id,
                  (unsigned int)len);
    state = ESP32_SendCmd(g_wifi_cmd_buffer, ">");
    if (state != HAL_OK) {
        (void)app_log_printf("[WIFI] cipsend prompt failed=%ld\r\n", (long)state);
        return state;
    }

    /* ACK 是升级协议二进制帧，不能用字符串发送函数，必须按长度原样发给 ESP32。 */
    state = HAL_UART_Transmit(&huart2, (uint8_t *)pData, len, 1000U);
    if (state != HAL_OK) {
        (void)app_log_printf("[WIFI] uart send failed=%ld\r\n", (long)state);
        return state;
    }

    /*
     * Do not wait for SEND OK here. ESP32 can report the next +IPD frame before
     * or together with SEND OK, and consuming it here would drop OTA data.
     */
    return HAL_OK;
}

int WIFI_TCP_ReceiveData(uint8_t *id,
                         uint8_t *buffer,
                         uint16_t max_len,
                         uint16_t *rx_len,
                         char *ip,
                         uint16_t ip_size,
                         uint16_t *port)
{
    uint16_t raw_len = 0U;
    uint32_t start_tick;
    uint32_t wait_ms;
    char preview[33];
    uint8_t i;

    if ((id == 0) || (buffer == 0) || (rx_len == 0) || (max_len == 0U)) {
        return -1;
    }

    *id = 0U;
    *rx_len = 0U;
    if ((ip != 0) && (ip_size > 0U)) {
        ip[0] = '\0';
    }
    if (port != 0) {
        *port = 0U;
    }

    wait_ms = APP_WIFI_IPD_WAIT_MS;
    if (wait_ms < APP_WIFI_RX_TIMEOUT_MS) {
        wait_ms = APP_WIFI_RX_TIMEOUT_MS;
    }

    start_tick = HAL_GetTick();
    while ((HAL_GetTick() - start_tick) < wait_ms) {
        if (wifi_stream_pop_ipd(id, buffer, max_len, rx_len, ip, ip_size, port) == 0) {
            return 0;
        }

        (void)memset(g_wifi_raw_buffer, 0, sizeof(g_wifi_raw_buffer));
        raw_len = 0U;

    /*
     * ReceiveToIdle 会在 USART2 空闲时返回，适合 ESP32 这种 “文本头 + 二进制 payload” 的格式。
     * 接收到的数据只解析 +IPD 包，普通 AT 日志或连接提示会被忽略。
     */
        if (ESP32_IsRxIrqStarted() != 0U) {
            raw_len = ESP32_ReadRx(g_wifi_raw_buffer,
                                   (uint16_t)(sizeof(g_wifi_raw_buffer) - 1U),
                                   APP_WIFI_RX_TIMEOUT_MS);
        } else {
            (void)HAL_UARTEx_ReceiveToIdle(&huart2,
                                           g_wifi_raw_buffer,
                                           (uint16_t)(sizeof(g_wifi_raw_buffer) - 1U),
                                           &raw_len,
                                           APP_WIFI_RX_TIMEOUT_MS);
        }
        if (raw_len == 0U) {
            continue;
        }

        wifi_stream_append(g_wifi_raw_buffer, raw_len);
        if (wifi_stream_pop_ipd(id, buffer, max_len, rx_len, ip, ip_size, port) == 0) {
            return 0;
        }

        for (i = 0U; (i < raw_len) && (i < (uint8_t)(sizeof(preview) - 1U)); ++i) {
            uint8_t ch = g_wifi_raw_buffer[i];
            preview[i] = ((ch >= 32U) && (ch <= 126U)) ? (char)ch : '.';
        }
        preview[i] = '\0';

        (void)app_log_printf("[WIFI] raw rx len=%u stream=%u head=%02X %02X %02X %02X %02X %02X %02X %02X text=%s\r\n",
                             (unsigned int)raw_len,
                             (unsigned int)g_wifi_stream_len,
                             (unsigned int)((raw_len > 0U) ? g_wifi_raw_buffer[0] : 0U),
                             (unsigned int)((raw_len > 1U) ? g_wifi_raw_buffer[1] : 0U),
                             (unsigned int)((raw_len > 2U) ? g_wifi_raw_buffer[2] : 0U),
                             (unsigned int)((raw_len > 3U) ? g_wifi_raw_buffer[3] : 0U),
                             (unsigned int)((raw_len > 4U) ? g_wifi_raw_buffer[4] : 0U),
                             (unsigned int)((raw_len > 5U) ? g_wifi_raw_buffer[5] : 0U),
                             (unsigned int)((raw_len > 6U) ? g_wifi_raw_buffer[6] : 0U),
                             (unsigned int)((raw_len > 7U) ? g_wifi_raw_buffer[7] : 0U),
                             preview);
    }

    return -1;
}
