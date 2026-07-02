#include "Int_w24c02.h"

#include "app_log.h"

uint8_t Int_w24c02_read_byte(uint8_t byte_addr)
{
    uint8_t data = 0U;

    /* 读取单字节配置或升级标志。 */
    (void)HAL_I2C_Mem_Read(&hi2c2,
                           W24C02_ADDR_R,
                           byte_addr,
                           I2C_MEMADD_SIZE_8BIT,
                           &data,
                           1U,
                           100U);
    return data;
}

void Int_w24c02_write_byte(uint8_t byte_addr, uint8_t data)
{
    /* 写单字节标志位。 */
    (void)HAL_I2C_Mem_Write(&hi2c2,
                            W24C02_ADDR,
                            byte_addr,
                            I2C_MEMADD_SIZE_8BIT,
                            &data,
                            1U,
                            100U);
}

void Int_w24c02_read_bytes(uint8_t byte_addr, uint8_t *data, uint16_t len)
{
    if ((data == 0) || (len == 0U)) {
        return;
    }

    /* 连续读取 EEPROM 参数区。 */
    (void)HAL_I2C_Mem_Read(&hi2c2,
                           W24C02_ADDR_R,
                           byte_addr,
                           I2C_MEMADD_SIZE_8BIT,
                           data,
                           len,
                           1000U);
}

void Int_w24c02_write_bytes(uint8_t byte_addr, const uint8_t *data, uint16_t len)
{
    uint8_t page_remain_len;
    uint8_t start_page_addr;
    uint8_t page_count;

    if ((data == 0) || (len == 0U)) {
        return;
    }

    /* 24C02 总容量 256 字节，越界直接拒绝。 */
    if ((uint16_t)byte_addr + len > 255U) {
        (void)app_log_printf("w24c02 write overflow\r\n");
        return;
    }

    page_remain_len = (uint8_t)(W24C02_PAGE_SIZE - (byte_addr % W24C02_PAGE_SIZE));
    if (len <= page_remain_len) {
        (void)HAL_I2C_Mem_Write(&hi2c2,
                                W24C02_ADDR,
                                byte_addr,
                                I2C_MEMADD_SIZE_8BIT,
                                (uint8_t *)data,
                                len,
                                1000U);
        HAL_Delay(10U);
        return;
    }

    start_page_addr = byte_addr;
    page_count = 0U;

    /* 分页写入，避免跨页导致数据回卷覆盖。 */
    while (len > page_remain_len) {
        (void)HAL_I2C_Mem_Write(&hi2c2,
                                W24C02_ADDR,
                                start_page_addr,
                                I2C_MEMADD_SIZE_8BIT,
                                (uint8_t *)(data + (page_count * W24C02_PAGE_SIZE)),
                                page_remain_len,
                                1000U);

        page_count++;
        start_page_addr = (uint8_t)(start_page_addr + page_remain_len);
        len = (uint16_t)(len - page_remain_len);
        page_remain_len = W24C02_PAGE_SIZE;
        HAL_Delay(10U);
    }

    if (len > 0U) {
        (void)HAL_I2C_Mem_Write(&hi2c2,
                                W24C02_ADDR,
                                start_page_addr,
                                I2C_MEMADD_SIZE_8BIT,
                                (uint8_t *)(data + (page_count * W24C02_PAGE_SIZE)),
                                len,
                                1000U);
        HAL_Delay(10U);
    }
}
