#include "esp32.h"

#include "app_config.h"
#include "app_log.h"

/* ESP32 AT 鎸囦护鏈€澶氱瓑寰呮鏁帮紝姣忔绛夊緟 1 绉掋€?*/
#define MAX_RECEIVE_TIMES 10

/* AT 鎸囦护鍝嶅簲缂撳瓨锛屽彧鍦?ESP32 鍛戒护闃舵浣跨敤锛屼笉淇濆瓨鍗囩骇鏁版嵁銆?*/
static uint8_t cmd_receive[1024] = {0};

#define ESP32_CMD_TX_BUFFER_SIZE 160U
#define ESP32_RX_RING_SIZE 2048U

typedef enum {
    ESP32_EOL_CRLF = 0,
    ESP32_EOL_LF,
    ESP32_EOL_CR
} esp32_eol_t;

static esp32_eol_t g_esp32_eol = ESP32_EOL_CRLF;
static volatile uint16_t g_esp32_rx_head = 0U;
static volatile uint16_t g_esp32_rx_tail = 0U;
static volatile uint8_t g_esp32_rx_started = 0U;
static volatile uint8_t g_esp32_rx_overflow = 0U;
static uint8_t g_esp32_rx_ring[ESP32_RX_RING_SIZE];

static void ESP32_RxRingReset(void)
{
    __disable_irq();
    g_esp32_rx_head = 0U;
    g_esp32_rx_tail = 0U;
    g_esp32_rx_overflow = 0U;
    __enable_irq();
}

static void ESP32_RxRingPush(uint8_t byte)
{
    uint16_t next = (uint16_t)(g_esp32_rx_head + 1U);

    if (next >= ESP32_RX_RING_SIZE) {
        next = 0U;
    }

    if (next == g_esp32_rx_tail) {
        g_esp32_rx_overflow = 1U;
        return;
    }

    g_esp32_rx_ring[g_esp32_rx_head] = byte;
    g_esp32_rx_head = next;
}

static int ESP32_RxRingPop(uint8_t *byte)
{
    uint16_t tail;

    if (byte == 0) {
        return -1;
    }

    __disable_irq();
    if (g_esp32_rx_tail == g_esp32_rx_head) {
        __enable_irq();
        return -1;
    }

    tail = g_esp32_rx_tail;
    *byte = g_esp32_rx_ring[tail];
    tail = (uint16_t)(tail + 1U);
    if (tail >= ESP32_RX_RING_SIZE) {
        tail = 0U;
    }
    g_esp32_rx_tail = tail;
    __enable_irq();

    return 0;
}

static void ESP32_RxRingPrepend(const uint8_t *data, uint16_t len)
{
    uint16_t i;

    if ((data == 0) || (len == 0U)) {
        return;
    }

    __disable_irq();
    for (i = len; i > 0U; --i) {
        uint16_t prev;

        prev = (g_esp32_rx_tail == 0U) ? (ESP32_RX_RING_SIZE - 1U) : (uint16_t)(g_esp32_rx_tail - 1U);
        if (prev == g_esp32_rx_head) {
            g_esp32_rx_overflow = 1U;
            break;
        }

        g_esp32_rx_tail = prev;
        g_esp32_rx_ring[g_esp32_rx_tail] = data[i - 1U];
    }
    __enable_irq();
}

static const char *ESP32_EolString(esp32_eol_t eol)
{
    if (eol == ESP32_EOL_LF) {
        return "\n";
    }
    if (eol == ESP32_EOL_CR) {
        return "\r";
    }
    return "\r\n";
}

static const char *ESP32_EolName(esp32_eol_t eol)
{
    if (eol == ESP32_EOL_LF) {
        return "lf";
    }
    if (eol == ESP32_EOL_CR) {
        return "cr";
    }
    return "crlf";
}

static uint16_t ESP32_BuildTxCommand(const char *cmdstr, char *out, uint16_t out_size)
{
    const char *eol;
    size_t cmd_len;
    size_t eol_len;

    if ((cmdstr == 0) || (out == 0) || (out_size == 0U)) {
        return 0U;
    }

    cmd_len = strlen(cmdstr);
    while ((cmd_len > 0U) &&
           ((cmdstr[cmd_len - 1U] == '\r') || (cmdstr[cmd_len - 1U] == '\n'))) {
        --cmd_len;
    }

    eol = ESP32_EolString(g_esp32_eol);
    eol_len = strlen(eol);
    if ((cmd_len + eol_len) >= (size_t)out_size) {
        return 0U;
    }

    (void)memcpy(out, cmdstr, cmd_len);
    (void)memcpy(&out[cmd_len], eol, eol_len);
    out[cmd_len + eol_len] = '\0';

    return (uint16_t)(cmd_len + eol_len);
}

static const uint8_t *ESP32_FindToken(const uint8_t *buffer, uint16_t size, const char *token)
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

static uint16_t ESP32_FindIpdPrefixTail(const uint8_t *buffer, uint16_t size)
{
    static const uint8_t prefix[] = "+IPD,";
    uint16_t keep = 0U;
    uint16_t i;

    if ((buffer == 0) || (size == 0U)) {
        return 0U;
    }

    for (i = 1U; (i <= (uint16_t)(sizeof(prefix) - 1U)) && (i <= size); ++i) {
        if (memcmp(&buffer[size - i], prefix, i) == 0) {
            keep = i;
        }
    }

    return keep;
}

static void ESP32_RestorePossibleIpd(const uint8_t *buffer, uint16_t size)
{
    const uint8_t *ipd;
    uint16_t keep;

    if ((buffer == 0) || (size == 0U)) {
        return;
    }

    ipd = ESP32_FindToken(buffer, size, "+IPD,");
    if (ipd != 0) {
        ESP32_RxRingPrepend(ipd, (uint16_t)(size - (uint16_t)(ipd - buffer)));
        return;
    }

    keep = ESP32_FindIpdPrefixTail(buffer, size);
    if (keep > 0U) {
        ESP32_RxRingPrepend(&buffer[size - keep], keep);
    }
}

static const uint8_t *ESP32_FindLastToken(const uint8_t *buffer, uint16_t size, const char *token)
{
    const uint8_t *last = 0;
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
            last = &buffer[i];
        }
    }

    return last;
}

static uint8_t ESP32_HasOkAfterLastIpd(const uint8_t *buffer, uint16_t size)
{
    const uint8_t *last_ipd;
    const uint8_t *begin;
    uint16_t search_len;

    if ((buffer == 0) || (size == 0U)) {
        return 0U;
    }

    last_ipd = ESP32_FindLastToken(buffer, size, "+IPD,");
    begin = (last_ipd == 0) ? buffer : (last_ipd + 5U);
    if (begin >= &buffer[size]) {
        return 0U;
    }

    search_len = (uint16_t)(&buffer[size] - begin);
    if ((ESP32_FindToken(begin, search_len, "\r\nOK") != 0) ||
        (ESP32_FindToken(begin, search_len, "\nOK") != 0) ||
        (ESP32_FindToken(begin, search_len, "OK\r") != 0) ||
        (ESP32_FindToken(begin, search_len, "OK\n") != 0)) {
        return 1U;
    }

    return 0U;
}

static const uint8_t *ESP32_FindCmdSuccess(const uint8_t *buffer,
                                           uint16_t size,
                                           const char *token,
                                           uint8_t require_ok_before_token)
{
    const uint8_t *end;
    const uint8_t *match;
    const uint8_t *search;
    uint16_t remain;
    uint16_t token_len;

    if ((buffer == 0) || (token == 0)) {
        return 0;
    }

    end = &buffer[size];
    search = buffer;
    remain = size;
    token_len = (uint16_t)strlen(token);
    if (token_len == 0U) {
        return 0;
    }

    while (search < end) {
        match = ESP32_FindToken(search, remain, token);
        if (match == 0) {
            return 0;
        }

        if ((require_ok_before_token == 0U) ||
            (ESP32_HasOkAfterLastIpd(buffer, (uint16_t)(match - buffer)) != 0U)) {
            return match;
        }

        search = match + token_len;
        if (search >= end) {
            return 0;
        }
        remain = (uint16_t)(end - search);
    }

    return 0;
}

static void ESP32_FlushRx(uint32_t timeout_ms)
{
    uint16_t rx_len = 0U;
    uint32_t start_tick;

    start_tick = HAL_GetTick();
    do {
        rx_len = 0U;
        (void)HAL_UARTEx_ReceiveToIdle(&huart2,
                                       cmd_receive,
                                       sizeof(cmd_receive) - 1U,
                                       &rx_len,
                                       20U);
    } while ((rx_len > 0U) && ((HAL_GetTick() - start_tick) < timeout_ms));

    (void)memset(cmd_receive, 0, sizeof(cmd_receive));
}

static void ESP32_LogRxHead(const char *prefix, const char *cmdstr, const char *exp_res, uint16_t rx_len)
{
    char preview[33];
    uint8_t i;
    size_t cmd_preview_len;

    if ((prefix == 0) || (cmdstr == 0) || (exp_res == 0)) {
        return;
    }

    cmd_preview_len = strcspn(cmdstr, "\r\n");
    if (cmd_preview_len > 80U) {
        cmd_preview_len = 80U;
    }

    for (i = 0U; (i < rx_len) && (i < (uint8_t)(sizeof(preview) - 1U)); ++i) {
        uint8_t ch = cmd_receive[i];
        preview[i] = ((ch >= 32U) && (ch <= 126U)) ? (char)ch : '.';
    }
    preview[i] = '\0';

    (void)app_log_printf("[ESP32] %s cmd=%.*s wait=%s rx=%u head=%02X %02X %02X %02X text=%s\r\n",
                         prefix,
                         (int)cmd_preview_len,
                         cmdstr,
                         exp_res,
                         (unsigned int)rx_len,
                         (unsigned int)((rx_len > 0U) ? cmd_receive[0] : 0U),
                         (unsigned int)((rx_len > 1U) ? cmd_receive[1] : 0U),
                         (unsigned int)((rx_len > 2U) ? cmd_receive[2] : 0U),
                         (unsigned int)((rx_len > 3U) ? cmd_receive[3] : 0U),
                         preview);
}

static void ESP32_HardwareReset(void)
{
    (void)app_log_printf("[ESP32] EN reset\r\n");
    HAL_GPIO_WritePin(ESP32_EN_GPIO_Port, ESP32_EN_Pin, GPIO_PIN_RESET);
    HAL_Delay(200U);
    HAL_GPIO_WritePin(ESP32_EN_GPIO_Port, ESP32_EN_Pin, GPIO_PIN_SET);
    HAL_Delay(3000U);
}

static HAL_StatusTypeDef ESP32_SetBaud(uint32_t baud)
{
    if (g_esp32_rx_started != 0U) {
        __HAL_UART_DISABLE_IT(&huart2, UART_IT_RXNE);
        __HAL_UART_DISABLE_IT(&huart2, UART_IT_ERR);
        HAL_NVIC_DisableIRQ(USART2_IRQn);
        g_esp32_rx_started = 0U;
    }
    (void)HAL_UART_Abort(&huart2);
    (void)HAL_UART_DeInit(&huart2);
    huart2.Init.BaudRate = baud;
    return HAL_UART_Init(&huart2);
}

static HAL_StatusTypeDef ESP32_ProbeAt(uint32_t baud, esp32_eol_t eol)
{
    uint16_t rx_len = 0U;
    const char *cmd;
    uint8_t i;
    char preview[33];

    (void)memset(cmd_receive, 0, sizeof(cmd_receive));
    ESP32_FlushRx(80U);
    cmd = (eol == ESP32_EOL_LF) ? "AT\n" : ((eol == ESP32_EOL_CR) ? "AT\r" : "AT\r\n");
    HAL_Delay(30U);
    (void)HAL_UART_Transmit(&huart2, (uint8_t *)cmd, (uint16_t)strlen(cmd), 1000U);
    (void)HAL_UARTEx_ReceiveToIdle(&huart2, cmd_receive, sizeof(cmd_receive) - 1U, &rx_len, 800U);

    if (strstr((char *)cmd_receive, "OK") != 0) {
        g_esp32_eol = eol;
        (void)app_log_printf("[ESP32] baud=%lu %s ok\r\n",
                             (unsigned long)baud,
                             ESP32_EolName(eol));
        return HAL_OK;
    }

    if (rx_len > 0U) {
        for (i = 0U; (i < rx_len) && (i < (uint8_t)(sizeof(preview) - 1U)); ++i) {
            uint8_t ch = cmd_receive[i];
            preview[i] = ((ch >= 32U) && (ch <= 126U)) ? (char)ch : '.';
        }
        preview[i] = '\0';
        (void)app_log_printf("[ESP32] baud=%lu %s rx len=%u head=%02X %02X %02X %02X text=%s\r\n",
                             (unsigned long)baud,
                             ESP32_EolName(eol),
                             (unsigned int)rx_len,
                             (unsigned int)((rx_len > 0U) ? cmd_receive[0] : 0U),
                             (unsigned int)((rx_len > 1U) ? cmd_receive[1] : 0U),
                             (unsigned int)((rx_len > 2U) ? cmd_receive[2] : 0U),
                             (unsigned int)((rx_len > 3U) ? cmd_receive[3] : 0U),
                             preview);
        return HAL_TIMEOUT;
    }

    (void)app_log_printf("[ESP32] baud=%lu %s no response\r\n",
                         (unsigned long)baud,
                         ESP32_EolName(eol));
    return HAL_TIMEOUT;
}

static HAL_StatusTypeDef ESP32_ProbeBaud(uint32_t baud)
{
    if (ESP32_SetBaud(baud) != HAL_OK) {
        (void)app_log_printf("[ESP32] uart baud set failed=%lu\r\n", (unsigned long)baud);
        return HAL_ERROR;
    }

    if (ESP32_ProbeAt(baud, ESP32_EOL_CRLF) == HAL_OK) {
        return HAL_OK;
    }
    if (ESP32_ProbeAt(baud, ESP32_EOL_LF) == HAL_OK) {
        return HAL_OK;
    }
    return ESP32_ProbeAt(baud, ESP32_EOL_CR);
}

static HAL_StatusTypeDef ESP32_DetectBaud(void)
{
    static const uint32_t baud_list[] = {
        115200UL,
        9600UL,
        57600UL,
        38400UL,
        19200UL,
        74880UL,
        921600UL
    };
    size_t i;

    for (i = 0U; i < (sizeof(baud_list) / sizeof(baud_list[0])); ++i) {
        if (ESP32_ProbeBaud(baud_list[i]) == HAL_OK) {
            return HAL_OK;
        }
    }

    (void)ESP32_SetBaud(115200UL);
    return HAL_TIMEOUT;
}

static void ESP32_LogCmd(const char *prefix, const char *cmdstr)
{
    size_t len;

    if ((prefix == 0) || (cmdstr == 0)) {
        return;
    }

    len = strcspn(cmdstr, "\r\n");
    if (len > 80U) {
        len = 80U;
    }

    (void)app_log_printf("[ESP32] %s %.*s\r\n", prefix, (int)len, cmdstr);
}

HAL_StatusTypeDef ESP32_SendCmd(const char *cmdstr, const char *exp_res)
{
    uint16_t timeout = MAX_RECEIVE_TIMES;
    uint16_t cmd_len;
    uint16_t total_len = 0U;
    uint8_t preserve_rx = 0U;
    uint8_t require_ok_before_token = 0U;
    char tx_cmd[ESP32_CMD_TX_BUFFER_SIZE];

    if ((cmdstr == 0) || (exp_res == 0)) {
        return HAL_ERROR;
    }

    /* STM32 閫氳繃 USART2 鎶?AT 鎸囦护鍙戠粰 ESP32锛孍SP32 鍙礋璐ｇ綉缁滄帴鍏ャ€?*/
    cmd_len = ESP32_BuildTxCommand(cmdstr, tx_cmd, sizeof(tx_cmd));
    if (cmd_len == 0U) {
        return HAL_ERROR;
    }
    if (strstr(cmdstr, "CIPSERVER") != 0) {
        timeout = 5U;
    } else if ((strstr(cmdstr, "CWSAP") != 0) || (strstr(cmdstr, "CWJAP") != 0)) {
        timeout = 30U;
    }

    if ((g_esp32_rx_started != 0U) && (strstr(cmdstr, "CIPSEND") != 0)) {
        preserve_rx = 1U;
        if (strcmp(exp_res, ">") == 0) {
            require_ok_before_token = 1U;
        }
    }

    if (strstr(cmdstr, "CIPSEND") == 0) {
        if (g_esp32_rx_started != 0U) {
            ESP32_RxRingReset();
        } else {
            ESP32_FlushRx(80U);
        }
    }

    ESP32_LogCmd("cmd", tx_cmd);
    (void)HAL_UART_Transmit(&huart2, (uint8_t *)tx_cmd, cmd_len, UINT32_MAX);
    (void)memset(cmd_receive, 0, sizeof(cmd_receive));

    while (1) {
        uint16_t rx_len = 0;

        if (total_len >= (uint16_t)(sizeof(cmd_receive) - 1U)) {
            total_len = 0U;
            (void)memset(cmd_receive, 0, sizeof(cmd_receive));
        }

        if (g_esp32_rx_started != 0U) {
            uint8_t byte;
            uint32_t start_tick = HAL_GetTick();

            while ((HAL_GetTick() - start_tick) < 1000U) {
                if (ESP32_RxRingPop(&byte) == 0) {
                    cmd_receive[total_len] = byte;
                    rx_len = (uint16_t)(rx_len + 1U);
                    total_len = (uint16_t)(total_len + 1U);
                    if (total_len >= (uint16_t)(sizeof(cmd_receive) - 1U)) {
                        break;
                    }
                    cmd_receive[total_len] = '\0';
                    if ((ESP32_FindCmdSuccess(cmd_receive,
                                              total_len,
                                              exp_res,
                                              require_ok_before_token) != 0) ||
                        (ESP32_FindToken(cmd_receive, total_len, "ERROR") != 0) ||
                        (ESP32_FindToken(cmd_receive, total_len, "busy p...") != 0)) {
                        break;
                    }
                }
            }
        } else {
            (void)HAL_UARTEx_ReceiveToIdle(&huart2,
                                           &cmd_receive[total_len],
                                           (uint16_t)(sizeof(cmd_receive) - 1U - total_len),
                                           &rx_len,
                                           1000U);
            if (rx_len > 0U) {
                total_len = (uint16_t)(total_len + rx_len);
                cmd_receive[total_len] = '\0';
            }
        }

        /* 鏀跺埌鏈熸湜瀛楃涓茶鏄庢湰鏉?AT 鎸囦护鎵ц鎴愬姛銆?*/
        {
            const uint8_t *match = ESP32_FindCmdSuccess(cmd_receive,
                                                        total_len,
                                                        exp_res,
                                                        require_ok_before_token);

            if (match != 0) {
                if (preserve_rx != 0U) {
                    uint16_t prefix_len = (uint16_t)(match - cmd_receive);
                    uint16_t token_len = (uint16_t)strlen(exp_res);
                    uint16_t suffix_pos = (uint16_t)(prefix_len + token_len);

                    /*
                     * CIPSEND shares USART2 with incoming +IPD data. Put back
                     * bytes consumed while waiting for '>' so OTA data is not
                     * lost if ESP32 interleaves responses and TCP packets.
                     */
                    if (total_len > suffix_pos) {
                        ESP32_RxRingPrepend(&cmd_receive[suffix_pos],
                                            (uint16_t)(total_len - suffix_pos));
                    }
                    if (prefix_len > 0U) {
                        ESP32_RestorePossibleIpd(cmd_receive, prefix_len);
                    }
                }
                return HAL_OK;
            }
        }

        /* ESP32 鏄庣‘杩斿洖 ERROR 鏃剁洿鎺ュけ璐ワ紝閬垮厤涓婂眰缁х画绛夊緟銆?*/
        if (ESP32_FindToken(cmd_receive, total_len, "ERROR") != 0) {
            ESP32_LogRxHead("error", cmdstr, exp_res, total_len);
            return HAL_ERROR;
        }

        /* ESP32 蹇欐椂杩斿洖 HAL_BUSY锛屼笂灞傚彲閫夋嫨绋嶅悗閲嶈瘯銆?*/
        if (ESP32_FindToken(cmd_receive, total_len, "busy p...") != 0) {
            ESP32_LogRxHead("busy", cmdstr, exp_res, total_len);
            return HAL_BUSY;
        }

        --timeout;
        if (timeout == 0U) {
            ESP32_LogRxHead("timeout", cmdstr, exp_res, total_len);
            return HAL_TIMEOUT;
        }
    }
}

HAL_StatusTypeDef ESP32_Init(void)
{
    HAL_StatusTypeDef state;

    /* USART2 鏄?STM32 涓?ESP32 澶栬妯″潡涔嬮棿鐨勬帶鍒?鏁版嵁閫氶亾銆?*/

#if (APP_ESP32_USE_EN_RESET != 0U)
    ESP32_HardwareReset();
    (void)app_log_printf("[ESP32] probe baud after reset\r\n");
    state = ESP32_DetectBaud();
#else
    HAL_GPIO_WritePin(ESP32_EN_GPIO_Port, ESP32_EN_Pin, GPIO_PIN_SET);
    HAL_Delay(1000U);
    (void)app_log_printf("[ESP32] skip EN reset, keep EN high\r\n");

    (void)app_log_printf("[ESP32] probe baud\r\n");
    state = ESP32_DetectBaud();
    if (state != HAL_OK) {
        (void)app_log_printf("[ESP32] no AT before reset, retry EN reset\r\n");
        ESP32_HardwareReset();
        (void)app_log_printf("[ESP32] probe baud after reset\r\n");
        state = ESP32_DetectBaud();
    }
#endif

    if (state != HAL_OK) {
        (void)app_log_printf("[ESP32] no AT response on common baud rates\r\n");
        return state;
    }

    (void)app_log_printf("[ESP32] AT channel ready\r\n");
    return HAL_OK;
}

void ESP32_ReadResponse(uint8_t *pbuffer, uint16_t max_len, uint16_t *len)
{
    /* 璇诲彇 ESP32 涓€娈典覆鍙ｅ搷搴旓紝閫氬父鐢ㄤ簬璋冭瘯鎴?AT 鍝嶅簲閫忎紶銆?*/
    (void)HAL_UARTEx_ReceiveToIdle(&huart2, pbuffer, max_len, len, UINT32_MAX);
}

void ESP32_StartRxIrq(void)
{
    ESP32_RxRingReset();
    g_esp32_rx_started = 1U;
    __HAL_UART_CLEAR_OREFLAG(&huart2);
    __HAL_UART_CLEAR_FEFLAG(&huart2);
    __HAL_UART_CLEAR_NEFLAG(&huart2);
    HAL_NVIC_SetPriority(USART2_IRQn, 12, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
    __HAL_UART_ENABLE_IT(&huart2, UART_IT_RXNE);
    __HAL_UART_ENABLE_IT(&huart2, UART_IT_ERR);
}

void ESP32_StopRxIrq(void)
{
    __HAL_UART_DISABLE_IT(&huart2, UART_IT_RXNE);
    __HAL_UART_DISABLE_IT(&huart2, UART_IT_ERR);
    HAL_NVIC_DisableIRQ(USART2_IRQn);
    g_esp32_rx_started = 0U;
    ESP32_RxRingReset();
}

uint8_t ESP32_IsRxIrqStarted(void)
{
    return g_esp32_rx_started;
}

uint16_t ESP32_ReadRx(uint8_t *buffer, uint16_t max_len, uint32_t timeout_ms)
{
    uint16_t len = 0U;
    uint8_t byte;
    uint32_t start_tick;
    uint32_t last_rx_tick;

    if ((buffer == 0) || (max_len == 0U)) {
        return 0U;
    }

    start_tick = HAL_GetTick();
    last_rx_tick = start_tick;
    while (((HAL_GetTick() - start_tick) < timeout_ms) && (len < max_len)) {
        if (ESP32_RxRingPop(&byte) == 0) {
            buffer[len++] = byte;
            last_rx_tick = HAL_GetTick();
            continue;
        }

        if ((len > 0U) && ((HAL_GetTick() - last_rx_tick) >= 3U)) {
            break;
        }
    }

    return len;
}

void ESP32_RxIrqHandler(void)
{
    uint32_t sr = huart2.Instance->SR;
    uint8_t data;

    if ((sr & (USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)) != 0U) {
        (void)huart2.Instance->SR;
        data = (uint8_t)(huart2.Instance->DR & 0xFFU);
        if (g_esp32_rx_started != 0U) {
            ESP32_RxRingPush(data);
        }
        huart2.ErrorCode = HAL_UART_ERROR_NONE;
        return;
    }

    if ((sr & USART_SR_RXNE) != 0U) {
        data = (uint8_t)(huart2.Instance->DR & 0xFFU);
        if (g_esp32_rx_started != 0U) {
            ESP32_RxRingPush(data);
        }
    }
}

