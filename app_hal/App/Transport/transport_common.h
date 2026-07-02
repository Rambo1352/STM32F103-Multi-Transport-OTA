#ifndef TRANSPORT_COMMON_H
#define TRANSPORT_COMMON_H

#include <stddef.h>
#include <stdint.h>

/* 通讯通道类型，用于调试时区分升级数据来自 UART、TCP 还是 Wi-Fi。 */
typedef enum {
    TRANSPORT_CHANNEL_UART = 0, /* 串口升级通道。 */
    TRANSPORT_CHANNEL_TCP  = 1, /* W5500 硬件 TCP 升级通道。 */
    TRANSPORT_CHANNEL_WIFI = 2  /* ESP32 USART2/AT Wi-Fi 外设升级通道。 */
} transport_channel_t;

/* 处理一整帧协议数据，生成 ACK 应答；三种通讯方式共用这一个入口。 */
int transport_common_handle_frame(transport_channel_t channel,
                                  const uint8_t *rx,
                                  size_t rx_size,
                                  uint8_t *tx,
                                  size_t tx_capacity,
                                  size_t *tx_size);

#endif
