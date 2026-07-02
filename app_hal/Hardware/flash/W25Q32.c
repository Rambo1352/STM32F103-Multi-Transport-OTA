#include "w25q32.h"

/* W25Q32 初始化入口，SPI 和 GPIO 已经由 CubeMX 生成代码完成初始化。 */
void W25Q32_Init(void)
{
    /* 保持空实现，避免重复初始化 SPI 外设。 */
}

/* 读取 JEDEC ID：厂商 ID 存入 mid，设备 ID 存入 did。 */
void W25Q32_ReadId(uint8_t *mid, uint16_t *did)
{
    /* 片选拉低后发送 0x9F 指令，随后连续读取 3 字节 ID。 */
    Driver_SPI_Start();

    Driver_SPI_SwapByte(0x9F);

    *mid = Driver_SPI_SwapByte(0x00);
    *did = Driver_SPI_SwapByte(0x00);
    *did <<= 8;
    *did |= Driver_SPI_SwapByte(0x00);

    Driver_SPI_Stop();
}

/* 发送写使能命令，后续擦除或写页之前必须先调用。 */
void W25Q32_WriteEnable(void)
{
    Driver_SPI_Start();
    Driver_SPI_SwapByte(0x06);
    Driver_SPI_Stop();
}

/* 发送写禁止命令，用于主动关闭芯片写入能力。 */
void W25Q32_WriteDisable(void)
{
    Driver_SPI_Start();
    Driver_SPI_SwapByte(0x04);
    Driver_SPI_Stop();
}

/* 轮询状态寄存器 BUSY 位，等待芯片完成擦除或写入。 */
void W25Q32_WAITNOTBUSY(void)
{
    Driver_SPI_Start();
    Driver_SPI_SwapByte(0x05);
    while (Driver_SPI_SwapByte(0x00) & 0x01);
    Driver_SPI_Stop();
}

/* 按块号和扇区号擦除 4KB 扇区。 */
void W25Q32_EraseSector(uint8_t block, uint8_t sector)
{
    W25Q32_WAITNOTBUSY();
    W25Q32_WriteEnable();

    /* W25Q32 每块 64KB，每扇区 4KB，这里换算成 24 位线性地址。 */
    uint32_t addr = (block * 65536) + (sector * 4096);

    Driver_SPI_Start();
    Driver_SPI_SwapByte(0x20);                /* 扇区擦除指令。 */
    Driver_SPI_SwapByte((addr >> 16) & 0xFF); /* 地址高 8 位。 */
    Driver_SPI_SwapByte((addr >> 8)  & 0xFF); /* 地址中 8 位。 */
    Driver_SPI_SwapByte((addr >> 0)  & 0xFF); /* 地址低 8 位。 */
    Driver_SPI_Stop();

    /* 擦除耗时较长，必须等待 BUSY 清零后才能继续访问。 */
    W25Q32_WAITNOTBUSY();
}

/* 按页写入数据，调用方需要保证 len 不超过页剩余空间。 */
void W25Q32_WritePage(uint8_t block, uint8_t sector, uint8_t page, uint8_t *data, uint16_t len)
{
    W25Q32_WAITNOTBUSY();
    W25Q32_WriteEnable();

    /* W25Q32 每页 256 字节，这里把块/扇区/页换算为线性地址。 */
    uint32_t addr = (block * 65536) + (sector * 4096) + (page * 256);

    Driver_SPI_Start();
    Driver_SPI_SwapByte(0x02);                /* 页编程指令。 */
    Driver_SPI_SwapByte((addr >> 16) & 0xFF); /* 地址高 8 位。 */
    Driver_SPI_SwapByte((addr >> 8)  & 0xFF); /* 地址中 8 位。 */
    Driver_SPI_SwapByte((addr >> 0)  & 0xFF); /* 地址低 8 位。 */

    for (uint16_t i = 0; i < len; i++)
    {
        /* 连续写入本页数据。 */
        Driver_SPI_SwapByte(data[i]);
    }
    Driver_SPI_Stop();

    /* 页写入结束后等待芯片内部编程完成。 */
    W25Q32_WAITNOTBUSY();
}

/* 从指定块/扇区/页读取数据到 data 缓冲区。 */
void W25Q32_Read(uint8_t block, uint8_t sector, uint8_t page, uint8_t *data, uint16_t len)
{
    W25Q32_WAITNOTBUSY();

    /* 把块/扇区/页换算为 24 位线性地址。 */
    uint32_t addr = (block * 65536) + (sector * 4096) + (page * 256);

    Driver_SPI_Start();
    Driver_SPI_SwapByte(0x03);                /* 标准读数据指令。 */
    Driver_SPI_SwapByte((addr >> 16) & 0xFF); /* 地址高 8 位。 */
    Driver_SPI_SwapByte((addr >> 8)  & 0xFF); /* 地址中 8 位。 */
    Driver_SPI_SwapByte((addr >> 0)  & 0xFF); /* 地址低 8 位。 */

    for (uint16_t i = 0; i < len; i++)
    {
        /* 发送空字节产生时钟，同时读取 Flash 返回的数据。 */
        data[i] = Driver_SPI_SwapByte(0x00);
    }
    Driver_SPI_Stop();
}
