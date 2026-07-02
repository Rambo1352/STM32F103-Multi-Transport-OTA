#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>

/*
 * App 工程运行在内部 Flash 的 0x08008000 之后，
 * 前面的 0x08000000 ~ 0x08007FFF 由 bootloader 使用。
 */
#define APP_FLASH_BASE_ADDR            0x08008000UL

/* App 在 STM32F103ZE 内部 Flash 中可使用的最大空间。 */
#define APP_FLASH_MAX_SIZE             0x00078000UL

/* OTA 数据包允许写入的最大镜像大小。 */
#define APP_SIZE_MAX                   APP_FLASH_MAX_SIZE

/* 当前正在运行的 App 固件版本号，可按项目版本规则自行递增。 */
#define APP_VERSION                    0x00010000UL

/* 心跳日志打印周期，单位毫秒。 */
#define APP_HEARTBEAT_PERIOD_MS        1000UL

/* 版本与升级状态打印周期，单位毫秒。 */
#define APP_VERSION_PERIOD_MS          5000UL

/* 三种升级任务统一的轮询周期，单位毫秒。 */
#define APP_TRANSPORT_POLL_MS          10UL

/* UART 升级通道单帧接收超时时间，单位毫秒。 */
#define APP_UART_FRAME_TIMEOUT_MS      5UL

/* ESP32 串口转 Wi-Fi 时，等待一段 +IPD 数据的最大时间，单位毫秒。 */
#define APP_WIFI_RX_TIMEOUT_MS         5UL
#define APP_WIFI_IPD_WAIT_MS           20UL
#define APP_WIFI_POLL_DELAY_MS         10UL

/* W5500 TCP 升级服务端口。 */
#define APP_TCP_UPGRADE_PORT           5000U

/* ESP32 Wi-Fi 升级服务端口。 */
#define APP_WIFI_UPGRADE_PORT          5001U

/*
 * 1 表示 ESP32 工作在 AP 热点模式；
 * 0 表示 ESP32 工作在 STA 入网模式。
 */
#define APP_WIFI_USE_AP_MODE           1U

/* ESP32 AP 模式参数。 */
#define APP_WIFI_AP_SSID               "dengziqi"
#define APP_WIFI_AP_PASSWORD           "1234567890"
#define APP_WIFI_AP_CHANNEL            5U
#define APP_WIFI_AP_ENCRYPTION         3U
#define APP_WIFI_AP_IP                 "192.168.36.1"
#define APP_WIFI_AP_GATEWAY            "192.168.36.1"
#define APP_WIFI_AP_NETMASK            "255.255.255.0"

/* ESP32 STA 模式参数。 */
#define APP_WIFI_STA_SSID              "1104YUEXINGUOW"
#define APP_WIFI_STA_PASSWORD          "88888888"

/* Set to 1 to pulse ESP32 EN during init. Keep 0 while checking PE4/EN reset. */
#define APP_ESP32_USE_EN_RESET         0U

/* FreeRTOS 日志队列深度。 */
#define APP_LOG_QUEUE_LENGTH           16U

/* 单条日志最大长度，末尾会自动补 '\0'。 */
#define APP_LOG_LINE_MAX               256U

/* FreeRTOS 日志任务栈大小，单位 word。 */
#define APP_TASK_STACK_LOG             512U

/* FreeRTOS 打印任务栈大小，单位 word。 */
#define APP_TASK_STACK_PRINT           256U

#define APP_TASK_STACK_LED             256U

/* FreeRTOS 升级任务栈大小，单位 word。 */
#define APP_TASK_STACK_UPGRADE         512U

/* FreeRTOS 日志任务优先级。 */
#define APP_TASK_PRIO_LOG              2U

/* FreeRTOS 打印任务优先级。 */
#define APP_TASK_PRIO_PRINT            2U

/* FreeRTOS 升级任务优先级。 */
#define APP_TASK_PRIO_UPGRADE          3U

#define APP_TASK_PRIO_LED              4U 

#endif /* APP_CONFIG_H */
