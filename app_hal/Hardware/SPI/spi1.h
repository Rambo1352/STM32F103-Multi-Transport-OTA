#ifndef __SPI1_H
#define __SPI1_H

#include "main.h"
#include "spi.h"

/* 拉低 W25Q32 片选引脚，开始 SPI 通信。 */
void Driver_SPI_Start(void);

/* 拉高 W25Q32 片选引脚，结束 SPI 通信。 */
void Driver_SPI_Stop(void);

/* 通过 SPI1 交换 1 字节数据。 */
uint8_t Driver_SPI_SwapByte(uint8_t byte);

#endif /* __SPI1_H */
