#include "app_freertos.h"

#include "app_config.h"
#include "app_log.h"
#include "app_status.h"
#include "spi_bus.h"
#include "uart_bus.h"
#include "ota_manager.h"
#include "transport_tcp.h"
#include "transport_uart.h"
#include "transport_wifi.h"

#include "FreeRTOS.h"
#include "task.h"

/* 心跳打印任务：用于证明 FreeRTOS 调度与串口输出都正常工作。 */
static void task_print_alive(void *argument)
{
    uint32_t count = 0U;
    (void)argument;

    while (1) {
        //(void)app_log_printf("[APP] heartbeat=%lu\r\n", (unsigned long)count++);
        vTaskDelay(pdMS_TO_TICKS(APP_HEARTBEAT_PERIOD_MS));
    }
}

/* 状态打印任务：周期输出版本号与 OTA 进度。 */
static void task_print_version(void *argument)
{
    app_status_t status;
    (void)argument;

    while (1) {
        app_status_get(&status);
        (void)app_log_printf("[APP] version=0x%08lX ota=%lu staging=0x%08lX recv=%lu\r\n",
                             (unsigned long)status.version,
                             (unsigned long)status.ota_active,
                             (unsigned long)status.staging_addr,
                             (unsigned long)status.received_size);
        vTaskDelay(pdMS_TO_TICKS(APP_VERSION_PERIOD_MS));
    }
}

/* 串口升级任务：通过 USART1 接收升级协议帧。 */
static void task_uart_upgrade(void *argument)
{
    (void)argument;

    transport_uart_init();

    while (1) {
        transport_uart_poll();
        vTaskDelay(pdMS_TO_TICKS(APP_TRANSPORT_POLL_MS));
    }
}

/* 网口升级任务：通过 W5500 提供 TCP Server 接收升级协议帧。 */
static void task_tcp_upgrade(void *argument)
{
    (void)argument;

    transport_tcp_init(APP_TCP_UPGRADE_PORT);

    while (1) {
        transport_tcp_poll();
        vTaskDelay(pdMS_TO_TICKS(APP_TRANSPORT_POLL_MS));
    }
}

/* Wi-Fi 升级任务：STM32 通过 USART2 控制 ESP32，并接收其转发的 TCP 数据。 */
static void task_wifi_upgrade(void *argument)
{
    (void)argument;

    transport_wifi_init(APP_WIFI_UPGRADE_PORT);

    while (1) {
        transport_wifi_poll();
        vTaskDelay(pdMS_TO_TICKS(APP_WIFI_POLL_DELAY_MS));
    }
}

static void task_led_flow(void *argument)
{
    (void)argument;

    while (1) {
        HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET);
        HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_SET);
        vTaskDelay(pdMS_TO_TICKS(1000));

        HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
        HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_SET);
        vTaskDelay(pdMS_TO_TICKS(1000));

        HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
        HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET);
        HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_RESET);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void app_create_task_or_log(TaskFunction_t task,
                                   const char *name,
                                   uint16_t stack,
                                   UBaseType_t priority)
{
    BaseType_t result;

    result = xTaskCreate(task, name, stack, 0, priority, 0);
    if (result != pdPASS) {
        (void)app_log_printf("[APP] create task failed: %s\r\n", name);
    }
}

void app_freertos_start(void)
{
    spi_bus_init();
    uart_bus_init();
    /*
     * 先初始化 OTA 状态机，
     * 这样三种升级任务一上来就能使用统一的升级上下文。
     */
    ota_manager_init();

    /*
     * 日志模块使用 FreeRTOS 队列 + 日志任务进行封装，
     * 符合你前面提出的“App 打印要按 FreeRTOS 封装”的要求。
     */
    (void)app_log_init();
    (void)app_log_printf("[APP] FreeRTOS start...\r\n");

    app_create_task_or_log(task_print_alive,
                           "alive",
                           APP_TASK_STACK_PRINT,
                           APP_TASK_PRIO_PRINT);

    app_create_task_or_log(task_print_version,
                           "version",
                           APP_TASK_STACK_PRINT,
                           APP_TASK_PRIO_PRINT);

    app_create_task_or_log(task_led_flow,
                           "led_flow",
                           APP_TASK_STACK_LED,
                           APP_TASK_PRIO_LED);

    /*
     * 三种升级方式分别独立成任务：
     * 1. UART 升级；
     * 2. W5500 TCP 升级；
     * 3. ESP32 串口 Wi-Fi 升级。
     */
    app_create_task_or_log(task_uart_upgrade,
                           "uart_ota",
                           APP_TASK_STACK_UPGRADE,
                           APP_TASK_PRIO_UPGRADE);

    app_create_task_or_log(task_tcp_upgrade,
                           "tcp_ota",
                           APP_TASK_STACK_UPGRADE,
                           APP_TASK_PRIO_UPGRADE);

    app_create_task_or_log(task_wifi_upgrade,
                           "wifi_ota",
                           APP_TASK_STACK_UPGRADE,
                           APP_TASK_PRIO_UPGRADE);

    (void)app_log_printf("[APP] free heap before scheduler=%lu\r\n",
                         (unsigned long)xPortGetFreeHeapSize());

    /* 启动调度器后，系统就正式进入 App 任务运行状态。 */
    vTaskStartScheduler();

    /* 如果还能执行到这里，通常说明 FreeRTOS 堆空间不足。 */
    while (1) {
    }
}
