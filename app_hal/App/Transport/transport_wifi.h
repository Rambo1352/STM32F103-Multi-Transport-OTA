#ifndef TRANSPORT_WIFI_H
#define TRANSPORT_WIFI_H

#include <stdint.h>

/* 初始化 Wi-Fi 升级通道，ESP32 作为 USART2 外设启动 TCP Server。 */
void transport_wifi_init(uint16_t port);

/* 轮询 ESP32 Wi-Fi 通道，收到完整升级帧后进入统一 OTA 处理。 */
void transport_wifi_poll(void);

#endif /* TRANSPORT_WIFI_H */
