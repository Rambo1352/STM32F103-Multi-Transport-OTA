#include "app_log.h"

#include "app_config.h"
#include "uart_bus.h"
#include "usart.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * 日志队列中的每个元素都存放一整条完整字符串，
 * 避免多个任务直接抢占串口导致输出内容互相穿插。
 */
typedef struct
{
    char text[APP_LOG_LINE_MAX];
} app_log_item_t;

/* FreeRTOS 日志消息队列句柄。 */
static QueueHandle_t g_app_log_queue = 0;
static volatile uint8_t g_app_log_enabled = 1U;

/* 通过 USART1 直接输出字符串。 */
static void app_log_output_direct(const char *text)
{
    size_t len;

    if (g_app_log_enabled == 0U) {
        return;
    }

    if (text == 0) {
        return;
    }

    len = strlen(text);
    if (len == 0U) {
        return;
    }

    uart_bus_lock();
    (void)HAL_UART_Transmit(&huart1, (const uint8_t *)text, (uint16_t)len, HAL_MAX_DELAY);
    uart_bus_unlock();
}

/* FreeRTOS 专用日志任务，负责把队列中的日志真正发送到串口。 */
static void app_log_task(void *argument)
{
    app_log_item_t item;
    (void)argument;

    while (1) {
        if (xQueueReceive(g_app_log_queue, &item, portMAX_DELAY) == pdPASS) {
            app_log_output_direct(item.text);
        }
    }
}

int app_log_init(void)
{
    BaseType_t result;

    /* 已初始化过则直接返回，避免重复创建任务和队列。 */
    if (g_app_log_queue != 0) {
        return 0;
    }

    g_app_log_queue = xQueueCreate(APP_LOG_QUEUE_LENGTH, sizeof(app_log_item_t));
    if (g_app_log_queue == 0) {
        return -1;
    }

    result = xTaskCreate(app_log_task,
                         "app_log",
                         APP_TASK_STACK_LOG,
                         0,
                         APP_TASK_PRIO_LOG,
                         0);
    if (result != pdPASS) {
        return -1;
    }

    return 0;
}

void app_log_write_string(const char *text)
{
    /* 直接输出接口主要给已经整理好的固定字符串使用。 */
    app_log_output_direct(text);
}

void app_log_set_enabled(uint8_t enabled)
{
    g_app_log_enabled = (enabled != 0U) ? 1U : 0U;
}

int app_log_printf(const char *format, ...)
{
    app_log_item_t item;
    va_list args;
    int length;

    if ((format == 0) || (g_app_log_enabled == 0U)) {
        return -1;
    }

    (void)memset(&item, 0, sizeof(item));

    va_start(args, format);
    length = vsnprintf(item.text, sizeof(item.text), format, args);
    va_end(args);

    if (length < 0) {
        return -1;
    }

    /*
     * 在调度器还没启动之前，或者日志队列尚未创建时，
     * 直接阻塞输出，确保启动日志依然可见。
     */
    if ((g_app_log_queue == 0) || (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)) {
        app_log_output_direct(item.text);
        return 0;
    }

    /*
     * 正常运行阶段优先走队列。
     * 如果队列暂时满了，退回到直接输出，避免关键日志完全丢失。
     */
    if (xQueueSend(g_app_log_queue, &item, 0U) != pdPASS) {
        return -1;
    }

    return 0;
}
