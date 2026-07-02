#include "transport_wifi.h"

#include "app_log.h"
#include "ota_manager.h"
#include "transport_common.h"
#include "transport_wifi_port.h"
#include "upgrade_protocol.h"

#include "main.h"

#define WIFI_INIT_RETRY_MS 5000UL

static uint16_t g_wifi_port;
static uint32_t g_wifi_next_init_tick;

void transport_wifi_init(uint16_t port)
{
    g_wifi_port = port;
    g_wifi_next_init_tick = HAL_GetTick();
    transport_wifi_port_init(port);
    g_wifi_next_init_tick = HAL_GetTick() + WIFI_INIT_RETRY_MS;
}

void transport_wifi_poll(void)
{
    uint8_t rx[UPGRADE_FRAME_MAX_SIZE];
    uint8_t tx[UPGRADE_FRAME_OVERHEAD + 8U];
    size_t rx_size = 0U;
    size_t tx_size = 0U;
    uint16_t cmd = 0U;

    if (transport_wifi_port_is_ready() == 0) {
        if ((int32_t)(HAL_GetTick() - g_wifi_next_init_tick) >= 0) {
            (void)app_log_printf("[WIFI OTA] retry init\r\n");
            transport_wifi_port_init(g_wifi_port);
            g_wifi_next_init_tick = HAL_GetTick() + WIFI_INIT_RETRY_MS;
        }
        return;
    }

    /* 底层端口负责从 ESP32 USART2/AT Wi-Fi 外设中拼出一整帧协议数据。 */
    if (transport_wifi_port_recv_frame(rx, sizeof(rx), &rx_size) != 0) {
        return;
    }

    if (rx_size >= UPGRADE_FRAME_HEADER_SIZE) {
        cmd = (uint16_t)rx[0] | ((uint16_t)rx[1] << 8U);
        if (cmd != UPGRADE_CMD_DATA) {
            (void)app_log_printf("[WIFI OTA] rx cmd=0x%04X len=%u\r\n",
                                 (unsigned int)cmd,
                                 (unsigned int)((uint16_t)rx[2] | ((uint16_t)rx[3] << 8U)));
        }
    }

    if (transport_common_handle_frame(TRANSPORT_CHANNEL_WIFI,
                                      rx,
                                      rx_size,
                                      tx,
                                      sizeof(tx),
                                      &tx_size) == 0) {
        if (transport_wifi_port_send(tx, tx_size) != 0) {
            (void)app_log_printf("[WIFI OTA] ack send failed\r\n");
            return;
        }
        ota_manager_poll_reset();
    } else {
        (void)app_log_printf("[WIFI OTA] ack build failed\r\n");
    }

    (void)g_wifi_port;
}
