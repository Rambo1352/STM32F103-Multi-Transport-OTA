#include "spi_bus.h"

#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t g_spi_bus_mutex = 0;

void spi_bus_init(void)
{
    if (g_spi_bus_mutex == 0) {
        g_spi_bus_mutex = xSemaphoreCreateMutex();
    }
}

void spi_bus_lock(void)
{
    if (g_spi_bus_mutex != 0) {
        (void)xSemaphoreTake(g_spi_bus_mutex, portMAX_DELAY);
    }
}

void spi_bus_unlock(void)
{
    if (g_spi_bus_mutex != 0) {
        (void)xSemaphoreGive(g_spi_bus_mutex);
    }
}
