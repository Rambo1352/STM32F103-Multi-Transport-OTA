/*
 * FreeRTOS V202212.01
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*-----------------------------------------------------------
 * Application specific definitions.
 *
 * These definitions should be adjusted for your particular hardware and
 * application requirements.
 *
 * THESE PARAMETERS ARE DESCRIBED WITHIN THE 'CONFIGURATION' SECTION OF THE
 * FreeRTOS API DOCUMENTATION AVAILABLE ON THE FreeRTOS.org WEB SITE.
 *
 * See http://www.freertos.org/a00110.html
 *----------------------------------------------------------*/

/* 使用抢占式调度，升级任务可以按优先级及时运行。 */
#define configUSE_PREEMPTION		1
/* 当前 demo 不使用空闲钩子，避免引入额外回调。 */
#define configUSE_IDLE_HOOK			0
/* 当前 demo 不使用 tick 钩子，任务延时直接用 vTaskDelay。 */
#define configUSE_TICK_HOOK			0
/* STM32F103 系统主频配置为 72MHz，需要和 SystemClock_Config 保持一致。 */
#define configCPU_CLOCK_HZ			( ( unsigned long ) 72000000 )
/* FreeRTOS tick 频率为 1kHz，因此 pdMS_TO_TICKS(1) 对应约 1ms。 */
#define configTICK_RATE_HZ			( ( TickType_t ) 1000 )
/* 任务优先级数量，当前打印任务和升级任务只用到低几个优先级。 */
#define configMAX_PRIORITIES		( 5 )
/* 最小任务栈大小，打印/升级任务可在 app_config.h 中单独设置。 */
#define configMINIMAL_STACK_SIZE	( ( unsigned short ) 128 )
/* FreeRTOS 堆大小，heap_4.c 会从这里分配任务栈和控制块。 */
#define configTOTAL_HEAP_SIZE		( ( size_t ) ( 28 * 1024 ) )
/* 任务名最大长度，便于 Keil 调试器观察任务。 */
#define configMAX_TASK_NAME_LEN		( 16 )
/* 当前不启用运行轨迹统计，节省 RAM 和代码空间。 */
#define configUSE_TRACE_FACILITY	0
/* STM32F103 使用 32 位 tick，避免长时间运行时太快溢出。 */
#define configUSE_16_BIT_TICKS		0
/* 同优先级任务允许空闲任务让出 CPU。 */
#define configIDLE_SHOULD_YIELD		1
/* 允许使用 mutex 保护共享 SPI 总线。 */
#define configUSE_MUTEXES           1
#define configUSE_TASK_NOTIFICATIONS 1
/* Cortex-M3 异常入口直接映射到 FreeRTOS port，避免再包一层 C 函数。 */
#define vPortSVCHandler             SVC_Handler
#define xPortPendSVHandler          PendSV_Handler


/* Set the following definitions to 1 to include the API function, or zero
to exclude the API function. */

/* 下面这些宏控制是否编译对应 FreeRTOS API。 */
#define INCLUDE_vTaskPrioritySet		1
#define INCLUDE_uxTaskPriorityGet		1
#define INCLUDE_vTaskDelete				1
#define INCLUDE_vTaskCleanUpResources	0
#define INCLUDE_vTaskSuspend			1
#define INCLUDE_vTaskDelayUntil			1
#define INCLUDE_vTaskDelay				1
/* App 的 SysTick_Handler 需要判断/调用调度器相关接口时保留该 API。 */
#define INCLUDE_xTaskGetSchedulerState  1
#define INCLUDE_xTaskGetCurrentTaskHandle 1

/* This is the raw value as per the Cortex-M3 NVIC.  Values can be 255
(lowest) to 0 (1?) (highest). */
#define configKERNEL_INTERRUPT_PRIORITY 		255
/* !!!! configMAX_SYSCALL_INTERRUPT_PRIORITY must not be set to zero !!!!
See http://www.FreeRTOS.org/RTOS-Cortex-M3-M4.html. */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY 	191 /* equivalent to 0xb0, or priority 11. */


/* This is the value being used as per the ST library which permits 16
priority values, 0 to 15.  This must correspond to the
configKERNEL_INTERRUPT_PRIORITY setting.  Here 15 corresponds to the lowest
NVIC value of 255. */
#define configLIBRARY_KERNEL_INTERRUPT_PRIORITY	15

#endif /* FREERTOS_CONFIG_H */

