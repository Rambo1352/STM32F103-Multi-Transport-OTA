#ifndef TRANSPORT_TCP_PORT_H
#define TRANSPORT_TCP_PORT_H

#include <stddef.h>
#include <stdint.h>

/* 初始化 W5500 TCP 底层并监听升级端口。 */
void transport_tcp_port_init(uint16_t port);

/* 从 W5500 TCP 流中取出一整帧升级协议数据。 */
int transport_tcp_port_recv_frame(uint8_t *buffer, size_t capacity, size_t *received);

/* 通过 W5500 原路返回 ACK 或状态数据。 */
int transport_tcp_port_send(const uint8_t *buffer, size_t size);

#endif /* TRANSPORT_TCP_PORT_H */
