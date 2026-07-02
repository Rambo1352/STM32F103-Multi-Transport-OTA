#ifndef TRANSPORT_UART_H
#define TRANSPORT_UART_H

/* 初始化 UART 升级通道，当前复用 CubeMX 配置好的 USART1。 */
void transport_uart_init(void);
void transport_uart_irq_handler(void);

/* 轮询 UART 是否收到完整升级帧，收到后交给 OTA 管理层处理。 */
void transport_uart_poll(void);

#endif /* TRANSPORT_UART_H */
