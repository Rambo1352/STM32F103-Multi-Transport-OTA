#ifndef __INT_W24C02_H
#define __INT_W24C02_H

#include "i2c.h"
#include "usart.h"

/* 24C02 器件 7bit 地址左移后的写地址。 */
#define W24C02_ADDR 0xA0
/* 24C02 读地址。 */
#define W24C02_ADDR_R (W24C02_ADDR | 0x01)

/* EEPROM 内部地址长度为 8bit。 */
#define W24C02_ADDR_SIZE 8
/* EEPROM 页写大小为 16 字节。 */
#define W24C02_PAGE_SIZE 16

/* 读取 1 个字节。 */
uint8_t Int_w24c02_read_byte(uint8_t byte_addr);

/* 写入 1 个字节。 */
void Int_w24c02_write_byte(uint8_t byte_addr, uint8_t data);

/* 连续读取多个字节。 */
void Int_w24c02_read_bytes(uint8_t byte_addr, uint8_t *data, uint16_t len);

/* 连续写入多个字节。 */
void Int_w24c02_write_bytes(uint8_t byte_addr, const uint8_t *data, uint16_t len);

#endif /* __INT_W24C02_H */
