#ifndef __INT_W25Q32_H
#define __INT_W25Q32_H

#include "spi.h"
#include "main.h"

/* W25Q32 指令定义。 */
#define W25Q32_READ_ID            0x9F
#define W25Q32_READ_STATUS_REG    0x05
#define W25Q32_READ_DATA          0x03
#define W25Q32_WRITE_DATA         0x02
#define W25Q32_ERASE_SECTOR       0x20
#define W25Q32_WRITE_ENABLE       0x06

/* 拉低片选。 */
void Int_w25q32_start(void);

/* 拉高片选。 */
void Int_w25q32_stop(void);

/* 写 1 个字节。 */
void Int_w25q32_write_byte(uint8_t data);

/* 读 1 个字节。 */
uint8_t Int_w25q32_read_byte(void);

/* 读取器件 ID。 */
void Int_w25q32_read_id(uint8_t *mf_id, uint16_t *device_id);

/* 采用 block/sector/page/addr 四段地址格式读取。 */
void Int_w25q32_read_data(uint8_t block,
                          uint8_t sector,
                          uint8_t page,
                          uint8_t addr,
                          uint8_t *data,
                          uint16_t len);

/* 采用 24bit 线性地址读取。 */
void Int_w25q32_read_data_with_32addr(uint32_t addr, uint8_t *data, uint16_t len);

/* 页写接口，要求调用方自己保证不跨页。 */
void Int_w25q32_write_data(uint8_t block,
                           uint8_t sector,
                           uint8_t page,
                           uint8_t addr,
                           const uint8_t *data,
                           uint16_t len);

/* 擦除一个 4KB 扇区。 */
void Int_w25q32_erase_sector(uint8_t block, uint8_t sector);

#endif /* __INT_W25Q32_H */
