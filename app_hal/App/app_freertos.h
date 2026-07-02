#ifndef APP_FREERTOS_H
#define APP_FREERTOS_H

/*
 * 启动 App 侧 FreeRTOS 框架：
 * 1. 初始化 OTA 状态机；
 * 2. 创建日志任务、打印任务、三种升级任务；
 * 3. 启动调度器。
 */

#include "gpio.h"

void app_freertos_start(void);

#endif /* APP_FREERTOS_H */
