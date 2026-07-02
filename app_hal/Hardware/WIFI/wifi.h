#ifndef __WIFI_H__
#define __WIFI_H__

#include "esp32.h"

#include <stdint.h>

/*
 * Wi-Fi 模块说明：
 * 1. ESP32 在本项目中只作为 STM32 的外设模块使用。
 * 2. STM32 通过 USART2 发送 AT 指令控制 ESP32 建立 Wi-Fi/TCP 连接。
 * 3. 升级协议解析、CRC 校验、Flash 写入、pending 参数保存都在 STM32 侧完成。
 */

/* 初始化 ESP32 外设模块，底层会确认 USART2 和 ESP32 AT 通道可用。 */
HAL_StatusTypeDef WIFI_INIT(void);

/* STA 模式：ESP32 连接到已有路由器，STM32 仍然通过 AT 指令控制它。 */
HAL_StatusTypeDef WIFI_SET_Sta(void);

/* AP 模式：ESP32 开热点，PC/手机连接热点后访问 STM32 提供的 TCP 升级服务。 */
HAL_StatusTypeDef WIFI_SET_Ap(void);

/* 启动 ESP32 TCP Server，port 由 APP_WIFI_UPGRADE_PORT 传入，避免端口硬编码。 */
HAL_StatusTypeDef WIFI_TCP_ServerStart(uint16_t port);
void WIFI_TCP_ClearRxCache(void);

/* 通过指定 ESP32 link id 发送 TCP 数据，通常用于把 OTA ACK 原路返回上位机。 */
HAL_StatusTypeDef WIFI_TCP_SendData(uint8_t id, const uint8_t *pData, uint16_t len);

/*
 * 从 ESP32 USART2 数据流中解析一包 +IPD 数据。
 * 返回 0 表示 buffer 中已经得到真正的 TCP payload；返回 -1 表示未收到或格式不完整。
 */
int WIFI_TCP_ReceiveData(uint8_t *id,
                         uint8_t *buffer,
                         uint16_t max_len,
                         uint16_t *rx_len,
                         char *ip,
                         uint16_t ip_size,
                         uint16_t *port);

#endif /* __WIFI_H__ */
