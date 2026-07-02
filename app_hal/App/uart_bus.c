#include "uart_bus.h"

#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t g_uart_bus_mutex = 0;

void uart_bus_init(void)
{
    if (g_uart_bus_mutex == 0) {
        g_uart_bus_mutex = xSemaphoreCreateMutex();
    }
}

void uart_bus_lock(void)
{
    if (g_uart_bus_mutex != 0) {
        (void)xSemaphoreTake(g_uart_bus_mutex, portMAX_DELAY);
    }
}

void uart_bus_unlock(void)
{
    if (g_uart_bus_mutex != 0) {
        (void)xSemaphoreGive(g_uart_bus_mutex);
    }
}
