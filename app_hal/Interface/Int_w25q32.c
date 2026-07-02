#include "Int_w25q32.h"
#include "spi_bus.h"

void Int_w25q32_start(void)
{
    spi_bus_lock();
    /* 选中外部 Flash。 */
    HAL_GPIO_WritePin(W25Q32_CS_GPIO_Port, W25Q32_CS_Pin, GPIO_PIN_RESET);
}

void Int_w25q32_stop(void)
{
    /* 释放外部 Flash。 */
    HAL_GPIO_WritePin(W25Q32_CS_GPIO_Port, W25Q32_CS_Pin, GPIO_PIN_SET);
    spi_bus_unlock();
}

void Int_w25q32_write_byte(uint8_t data)
{
    (void)HAL_SPI_Transmit(&hspi1, &data, 1U, 100U);
}

uint8_t Int_w25q32_read_byte(void)
{
    uint8_t tx = 0xFFU;
    uint8_t data = 0U;

    /* W25Q32 读数据时需要发送 dummy 字节产生时钟。 */
    (void)HAL_SPI_TransmitReceive(&hspi1, &tx, &data, 1U, 100U);
    return data;
}

void Int_w25q32_read_id(uint8_t *mf_id, uint16_t *device_id)
{
    uint8_t high;
    uint8_t low;

    if ((mf_id == 0) || (device_id == 0)) {
        return;
    }

    Int_w25q32_start();
    Int_w25q32_write_byte(W25Q32_READ_ID);
    *mf_id = Int_w25q32_read_byte();
    high = Int_w25q32_read_byte();
    low = Int_w25q32_read_byte();
    *device_id = (uint16_t)((uint16_t)high << 8U) | low;
    Int_w25q32_stop();
}

static void Int_w25q32_wait_busy(void)
{
    uint8_t status;

    /* 轮询 BUSY 位，直到擦除/编程结束。 */
    while (1) {
        Int_w25q32_start();
        Int_w25q32_write_byte(W25Q32_READ_STATUS_REG);
        status = Int_w25q32_read_byte();
        Int_w25q32_stop();
        if ((status & 0x01U) == 0U) {
            break;
        }
    }
}

static void Int_w25q32_write_enable_internal(void)
{
    Int_w25q32_wait_busy();
    Int_w25q32_start();
    Int_w25q32_write_byte(W25Q32_WRITE_ENABLE);
    Int_w25q32_stop();
}

void Int_w25q32_read_data(uint8_t block,
                          uint8_t sector,
                          uint8_t page,
                          uint8_t addr,
                          uint8_t *data,
                          uint16_t len)
{
    uint32_t addr_24;
    uint16_t i;

    if ((data == 0) || (len == 0U)) {
        return;
    }

    Int_w25q32_wait_busy();
    Int_w25q32_start();
    Int_w25q32_write_byte(W25Q32_READ_DATA);

    addr_24 = ((uint32_t)block << 16U) |
              ((uint32_t)sector << 12U) |
              ((uint32_t)page << 8U) |
              (uint32_t)addr;
    Int_w25q32_write_byte((uint8_t)(addr_24 >> 16U));
    Int_w25q32_write_byte((uint8_t)(addr_24 >> 8U));
    Int_w25q32_write_byte((uint8_t)addr_24);

    for (i = 0U; i < len; ++i) {
        data[i] = Int_w25q32_read_byte();
    }

    Int_w25q32_stop();
}

void Int_w25q32_read_data_with_32addr(uint32_t addr, uint8_t *data, uint16_t len)
{
    uint16_t i;

    if ((data == 0) || (len == 0U)) {
        return;
    }

    Int_w25q32_wait_busy();
    Int_w25q32_start();
    Int_w25q32_write_byte(W25Q32_READ_DATA);
    Int_w25q32_write_byte((uint8_t)((addr >> 16U) & 0xFFU));
    Int_w25q32_write_byte((uint8_t)((addr >> 8U) & 0xFFU));
    Int_w25q32_write_byte((uint8_t)(addr & 0xFFU));

    for (i = 0U; i < len; ++i) {
        data[i] = Int_w25q32_read_byte();
    }

    Int_w25q32_stop();
}

void Int_w25q32_write_data(uint8_t block,
                           uint8_t sector,
                           uint8_t page,
                           uint8_t addr,
                           const uint8_t *data,
                           uint16_t len)
{
    uint32_t addr_24;
    uint16_t i;

    if ((data == 0) || (len == 0U)) {
        return;
    }

    Int_w25q32_write_enable_internal();
    Int_w25q32_start();
    Int_w25q32_write_byte(W25Q32_WRITE_DATA);

    addr_24 = ((uint32_t)block << 16U) |
              ((uint32_t)sector << 12U) |
              ((uint32_t)page << 8U) |
              (uint32_t)addr;
    Int_w25q32_write_byte((uint8_t)(addr_24 >> 16U));
    Int_w25q32_write_byte((uint8_t)(addr_24 >> 8U));
    Int_w25q32_write_byte((uint8_t)addr_24);

    for (i = 0U; i < len; ++i) {
        Int_w25q32_write_byte(data[i]);
    }

    Int_w25q32_stop();
}

void Int_w25q32_erase_sector(uint8_t block, uint8_t sector)
{
    uint32_t addr_24;

    Int_w25q32_write_enable_internal();
    Int_w25q32_start();
    Int_w25q32_write_byte(W25Q32_ERASE_SECTOR);

    addr_24 = ((uint32_t)block << 16U) | ((uint32_t)sector << 12U);
    Int_w25q32_write_byte((uint8_t)(addr_24 >> 16U));
    Int_w25q32_write_byte((uint8_t)(addr_24 >> 8U));
    Int_w25q32_write_byte((uint8_t)addr_24);

    Int_w25q32_stop();
}
