#ifndef __ESP32_H__
#define __ESP32_H__

#include "main.h"
#include "usart.h"
#include "string.h"

/* 初始化 ESP32 外设模块：STM32 通过 USART2 发送 AT 指令控制 ESP32。 */
HAL_StatusTypeDef ESP32_Init(void);

/* 读取 ESP32 串口返回数据，适合调试或接收 AT 响应。 */
void ESP32_ReadResponse(uint8_t * pbuffer,uint16_t max_len, uint16_t * len);

/* 发送 AT 指令并等待期望响应；ESP32 只作为外设，所有 OTA 主流程仍在 STM32。 */
HAL_StatusTypeDef ESP32_SendCmd(const char *cmdstr, const char *exp_res);
void ESP32_StartRxIrq(void);
void ESP32_StopRxIrq(void);
uint8_t ESP32_IsRxIrqStarted(void);
uint16_t ESP32_ReadRx(uint8_t *buffer, uint16_t max_len, uint32_t timeout_ms);
void ESP32_RxIrqHandler(void);

#endif /* __ESP32_H__ */
