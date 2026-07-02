#ifndef TRANSPORT_TCP_H
#define TRANSPORT_TCP_H

#include <stdint.h>

/* 初始化 TCP 升级通道，底层使用 W5500 监听指定端口。 */
void transport_tcp_init(uint16_t port);

/* 轮询 W5500 TCP 通道，收到完整升级帧后进入统一 OTA 处理。 */
void transport_tcp_poll(void);

#endif /* TRANSPORT_TCP_H */
