#include "transport_common.h"

#include "ota_manager.h"
#include "upgrade_protocol.h"

int transport_common_handle_frame(transport_channel_t channel,
                                  const uint8_t *rx,
                                  size_t rx_size,
                                  uint8_t *tx,
                                  size_t tx_capacity,
                                  size_t *tx_size)
{
    upgrade_frame_t frame;
    upgrade_ack_t ack;
    uint32_t detail = 0U;
    upgrade_status_t status;

    /* 当前通道只用于调试扩展，协议内容保持三种通讯方式完全一致。 */
    if (upgrade_protocol_parse(rx, rx_size, &frame) != UPGRADE_PARSE_OK) {
        ack.request_cmd = 0U;
        ack.status = UPGRADE_STATUS_CRC_ERROR;
        ack.detail = 0U;
    } else {
        status = ota_manager_handle_frame(&frame, &detail, (uint8_t)channel);
        ack.request_cmd = frame.cmd;
        ack.status = status;
        ack.detail = detail;
    }

    /* ACK 也走统一协议封包，上位机不用区分 UART/TCP/Wi-Fi。 */
    return upgrade_protocol_build_ack(&ack, tx, tx_capacity, tx_size);
}
