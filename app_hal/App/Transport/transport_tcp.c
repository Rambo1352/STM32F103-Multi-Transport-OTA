#include "transport_tcp.h"

#include "app_log.h"
#include "ota_manager.h"
#include "transport_common.h"
#include "transport_tcp_port.h"
#include "upgrade_protocol.h"

static uint16_t g_tcp_port;

void transport_tcp_init(uint16_t port)
{
    g_tcp_port = port;
    transport_tcp_port_init(port);
}

void transport_tcp_poll(void)
{
    uint8_t rx[UPGRADE_FRAME_MAX_SIZE];
    uint8_t tx[UPGRADE_FRAME_OVERHEAD + 8U];
    size_t rx_size = 0U;
    size_t tx_size = 0U;

    /*
     * W5500 端口层负责监听 TCP 连接，并从 socket RX 缓冲中拼出一整帧升级协议数据。
     * 这里不关心底层是 UART、W5500 还是 Wi-Fi，只把完整帧交给统一 OTA 处理入口。
     */
    if (transport_tcp_port_recv_frame(rx, sizeof(rx), &rx_size) != 0) {
        return;
    }

    (void)app_log_printf("[TCP OTA] rx cmd=0x%02X%02X len=%u\r\n",
                         (unsigned int)rx[1],
                         (unsigned int)rx[0],
                         (unsigned int)((uint16_t)rx[2] | ((uint16_t)rx[3] << 8U)));

    /* 统一处理 BEGIN/DATA/END 等命令，并生成 ACK 应答帧。 */
    if (transport_common_handle_frame(TRANSPORT_CHANNEL_TCP,
                                      rx,
                                      rx_size,
                                      tx,
                                      sizeof(tx),
                                      &tx_size) == 0) {
        /* ACK 从收到该升级帧的 TCP 连接原路返回给上位机。 */
        if (transport_tcp_port_send(tx, tx_size) != 0) {
            (void)app_log_printf("[TCP OTA] ack send failed\r\n");
        }
        ota_manager_poll_reset();
    }

    (void)g_tcp_port;
}
