#ifndef TRANSPORT_WIFI_PORT_H
#define TRANSPORT_WIFI_PORT_H

#include <stddef.h>
#include <stdint.h>

/* 初始化 ESP32 Wi-Fi 底层并启动 TCP Server。 */
void transport_wifi_port_init(uint16_t port);
int transport_wifi_port_is_ready(void);

/* 从 ESP32 的 +IPD 数据流中拼出一整帧升级协议数据。 */
int transport_wifi_port_recv_frame(uint8_t *buffer, size_t capacity, size_t *received);

/* 通过 ESP32 当前连接原路返回 ACK。 */
int transport_wifi_port_send(const uint8_t *buffer, size_t size);

#endif /* TRANSPORT_WIFI_PORT_H */
