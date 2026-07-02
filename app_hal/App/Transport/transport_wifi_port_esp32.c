#include "transport_wifi_port.h"

#include "app_config.h"
#include "app_log.h"
#include "upgrade_protocol.h"
#include "wifi.h"

#include <string.h>

/*
 * ESP32 Wi-Fi 升级端口层说明：
 * 1. ESP32 只是 USART2 外设模块，负责 Wi-Fi/TCP 收发。
 * 2. STM32 App 任务从 ESP32 的 +IPD 中取出 TCP payload。
 * 3. OTA 协议帧解析、CRC 校验、写 Flash、保存 pending 参数仍然由 STM32 完成。
 */

/* ESP32 允许多个 TCP 客户端连接；这里记录最近一次发来 OTA 帧的 link id。 */
#define WIFI_INVALID_LINK_ID          0xFFU

/* 协议帧头固定 4 字节：cmd(2) + len(2)。 */
#define WIFI_FRAME_HEADER_SIZE        UPGRADE_FRAME_HEADER_SIZE

/* Wi-Fi TCP 也是流式数据，缓存用于处理半帧、多帧粘包等情况。 */
static uint8_t g_wifi_rx_cache[UPGRADE_FRAME_MAX_SIZE];

/* 当前 Wi-Fi 缓存中已经累计的字节数。 */
static size_t g_wifi_rx_cached = 0U;

/* App 配置传入的 Wi-Fi TCP 升级端口。 */
static uint16_t g_wifi_listen_port = 0U;

/* 最近一次收到升级数据的 ESP32 link id，ACK 会回到这个连接。 */
static uint8_t g_wifi_client_id = WIFI_INVALID_LINK_ID;
static uint8_t g_wifi_ready = 0U;

/* 按升级协议小端序读取 payload 长度。 */
static uint16_t wifi_load_payload_len(const uint8_t *buffer)
{
    return (uint16_t)buffer[2] | ((uint16_t)buffer[3] << 8U);
}

/* 清空本地帧缓存，通常在发现非法长度或重新初始化 Wi-Fi 时使用。 */
static void wifi_clear_rx_cache(void)
{
    g_wifi_rx_cached = 0U;
}

static void wifi_clear_all_rx_cache(void)
{
    g_wifi_rx_cached = 0U;
    g_wifi_client_id = WIFI_INVALID_LINK_ID;
}

/* 将 ESP32 收到的一段 TCP payload 追加到本地缓存。 */
static int wifi_fill_rx_cache(void)
{
    uint16_t payload_len = 0U;
    uint8_t client_id = WIFI_INVALID_LINK_ID;
    size_t free_space;

    free_space = sizeof(g_wifi_rx_cache) - g_wifi_rx_cached;
    if (free_space == 0U) {
        /* 缓存满还没拼出完整帧，说明流已经错位，丢弃后等待下一帧头。 */
        wifi_clear_rx_cache();
        return -1;
    }

    if (WIFI_TCP_ReceiveData(&client_id,
                             &g_wifi_rx_cache[g_wifi_rx_cached],
                             (uint16_t)free_space,
                             &payload_len,
                             0,
                             0U,
                             0) != 0) {
        return -1;
    }

    if (payload_len == 0U) {
        return -1;
    }

    g_wifi_rx_cached += (size_t)payload_len;
    g_wifi_client_id = client_id;
    (void)app_log_printf("[WIFI OTA] ipd id=%u len=%u cached=%lu\r\n",
                         (unsigned int)client_id,
                         (unsigned int)payload_len,
                         (unsigned long)g_wifi_rx_cached);

    return 0;
}

/* 如果缓存里已经拼出完整 OTA 协议帧，则拷贝给 transport_wifi.c。 */
static int wifi_pop_frame(uint8_t *buffer, size_t capacity, size_t *received)
{
    uint16_t payload_len;
    size_t frame_size;
    size_t remain_size;
    size_t i;

    if ((buffer == 0) || (received == 0) || (capacity < UPGRADE_FRAME_OVERHEAD)) {
        return -1;
    }

    *received = 0U;

    if (g_wifi_rx_cached < WIFI_FRAME_HEADER_SIZE) {
        return -1;
    }

    payload_len = wifi_load_payload_len(g_wifi_rx_cache);
    if (payload_len > UPGRADE_FRAME_MAX_DATA) {
        /* 长度超过协议上限，直接清缓存，防止一直卡在错误帧头。 */
        wifi_clear_rx_cache();
        return -1;
    }

    frame_size = UPGRADE_FRAME_OVERHEAD + (size_t)payload_len;
    if (frame_size > capacity) {
        wifi_clear_rx_cache();
        return -1;
    }

    if (g_wifi_rx_cached < frame_size) {
        return -1;
    }

    for (i = 0U; i < frame_size; ++i) {
        buffer[i] = g_wifi_rx_cache[i];
    }
    *received = frame_size;
    (void)app_log_printf("[WIFI OTA] frame len=%lu\r\n", (unsigned long)frame_size);

    /*
     * 如果一次 +IPD 中带了多帧，取走第一帧后把剩余字节前移，
     * 下一次 poll 会继续处理后续帧。
     */
    remain_size = g_wifi_rx_cached - frame_size;
    for (i = 0U; i < remain_size; ++i) {
        g_wifi_rx_cache[i] = g_wifi_rx_cache[frame_size + i];
    }
    g_wifi_rx_cached = remain_size;

    return 0;
}

void transport_wifi_port_init(uint16_t port)
{
    HAL_StatusTypeDef state;

    g_wifi_listen_port = port;
    g_wifi_client_id = WIFI_INVALID_LINK_ID;
    g_wifi_ready = 0U;
    wifi_clear_all_rx_cache();
    WIFI_TCP_ClearRxCache();
    ESP32_StopRxIrq();

    /*
     * 先初始化 ESP32，再按项目配置进入 AP 或 STA。
     * 默认使用 AP 模式，便于上位机直接连接 ESP32 热点进行 Wi-Fi OTA。
     */
    (void)app_log_printf("[WIFI OTA] init port=%u\r\n", (unsigned int)port);
    state = WIFI_INIT();
    if (state != HAL_OK) {
        (void)app_log_printf("[WIFI OTA] ESP32 init failed=%ld\r\n", (long)state);
        return;
    }
#if (APP_WIFI_USE_AP_MODE != 0)
    (void)app_log_printf("[WIFI OTA] mode=AP ssid=%s ip=%s\r\n",
                         APP_WIFI_AP_SSID,
                         APP_WIFI_AP_IP);
    state = WIFI_SET_Ap();
    if (state != HAL_OK) {
        (void)app_log_printf("[WIFI OTA] AP start failed=%ld\r\n", (long)state);
        return;
    }
    (void)app_log_printf("[WIFI OTA] AP ready ssid=%s ip=%s\r\n",
                         APP_WIFI_AP_SSID,
                         APP_WIFI_AP_IP);
#else
    (void)app_log_printf("[WIFI OTA] mode=STA ssid=%s\r\n", APP_WIFI_STA_SSID);
    state = WIFI_SET_Sta();
    if (state != HAL_OK) {
        (void)app_log_printf("[WIFI OTA] STA connect failed=%ld\r\n", (long)state);
        return;
    }
#endif

    state = WIFI_TCP_ServerStart(g_wifi_listen_port);
    if (state != HAL_OK) {
        (void)app_log_printf("[WIFI OTA] listen failed=%ld\r\n", (long)state);
        return;
    }

    (void)app_log_printf("[WIFI OTA] listen port=%u\r\n", (unsigned int)g_wifi_listen_port);
    ESP32_StartRxIrq();
    g_wifi_ready = 1U;
}

int transport_wifi_port_is_ready(void)
{
    return (g_wifi_ready != 0U) ? 1 : 0;
}

int transport_wifi_port_recv_frame(uint8_t *buffer, size_t capacity, size_t *received)
{
    if (g_wifi_ready == 0U) {
        return -1;
    }

    /*
     * 优先从历史缓存取完整帧，再尝试从 ESP32 USART2 搬运新的 +IPD 数据。
     * 这样既能处理 TCP 半包，也能处理一次收到多帧的情况。
     */
    if (wifi_pop_frame(buffer, capacity, received) == 0) {
        return 0;
    }

    (void)wifi_fill_rx_cache();

    return wifi_pop_frame(buffer, capacity, received);
}

int transport_wifi_port_send(const uint8_t *buffer, size_t size)
{
    if ((buffer == 0) || (size == 0U) || (size > 0xFFFFU)) {
        return -1;
    }

    if (g_wifi_client_id == WIFI_INVALID_LINK_ID) {
        (void)app_log_printf("[WIFI OTA] send no client\r\n");
        return -1;
    }

    /*
     * ACK 必须发回刚才发送升级帧的 ESP32 link id。
     * 上位机不需要知道底层是 UART、W5500 还是 ESP32 Wi-Fi，ACK 帧格式完全相同。
     */
    if (WIFI_TCP_SendData(g_wifi_client_id, buffer, (uint16_t)size) != HAL_OK) {
        (void)app_log_printf("[WIFI OTA] send failed id=%u len=%lu\r\n",
                             (unsigned int)g_wifi_client_id,
                             (unsigned long)size);
        return -1;
    }

    return 0;
}
