#include "spi1.h"
#include "spi_bus.h"

void Driver_SPI_Start(void)
{
  spi_bus_lock();
  /* 拉低片选引脚，开始一次 SPI Flash 通信。 */
  HAL_GPIO_WritePin(W25Q32_CS_GPIO_Port, W25Q32_CS_Pin, GPIO_PIN_RESET);
}

void Driver_SPI_Stop(void)
{
  /* 拉高片选引脚，结束本次 SPI Flash 通信。 */
  HAL_GPIO_WritePin(W25Q32_CS_GPIO_Port, W25Q32_CS_Pin, GPIO_PIN_SET);
  spi_bus_unlock();
}

uint8_t Driver_SPI_SwapByte(uint8_t byte)
{
  uint8_t receivedByte = 0;

  /* SPI 全双工传输：发送 1 字节的同时接收 1 字节。 */
  if (HAL_SPI_TransmitReceive(&hspi1, &byte, &receivedByte, 1, HAL_MAX_DELAY) != HAL_OK)
  {
    /* 传输失败时返回 0，调用方可结合读到的数据判断异常。 */
    return 0;
  }

  /* 返回从 MISO 线上收到的字节。 */
  return receivedByte;
}
