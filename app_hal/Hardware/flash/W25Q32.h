#ifndef _W25Q32_H
#define _W25Q32_H

#include "main.h"
#include "spi1.h"

/* W25Q32 初始化入口，当前工程中保持空实现。 */
void W25Q32_Init(void);

/* 读取 W25Q32 的厂商 ID 和设备 ID。 */
void W25Q32_ReadId(uint8_t *mid, uint16_t *did);

/* 发送写使能命令。 */
void W25Q32_WriteEnable(void);

/* 发送写禁止命令。 */
void W25Q32_WriteDisable(void);

/* 擦除指定块内的 4KB 扇区。 */
void W25Q32_EraseSector(uint8_t block, uint8_t sector);

/* 向指定页写入数据，len 不应跨越页边界。 */
void W25Q32_WritePage(uint8_t block, uint8_t sector, uint8_t page, uint8_t *data, uint16_t len);

/* 从指定页读取数据到缓冲区。 */
void W25Q32_Read(uint8_t block, uint8_t sector, uint8_t page, uint8_t *data, uint16_t len);
#endif /* _W25Q32_H */
