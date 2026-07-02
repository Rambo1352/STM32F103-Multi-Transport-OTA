#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdint.h>

/*
 * 初始化 App 日志模块。
 * 该模块会创建一个 FreeRTOS 日志队列和专用日志任务，
 * 让多个任务的打印统一从 USART1 串行输出。
 */
int app_log_init(void);

/*
 * 按 printf 风格发送一条日志。
 * 当调度器尚未启动时会直接阻塞输出；
 * 当调度器已经运行时会先进入日志队列，再由日志任务输出。
 */
int app_log_printf(const char *format, ...);

/* 直接发送一段已经组织好的字符串。 */
void app_log_write_string(const char *text);
void app_log_set_enabled(uint8_t enabled);

#endif /* APP_LOG_H */
