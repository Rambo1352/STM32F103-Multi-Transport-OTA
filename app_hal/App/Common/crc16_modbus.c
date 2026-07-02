#include "crc16_modbus.h"

uint16_t crc16_modbus(const uint8_t *data, size_t length)
{
    /* Modbus CRC16 初值固定为 0xFFFF，多项式反向表示为 0xA001。 */
    uint16_t crc = 0xFFFFU;
    size_t i;
    uint8_t bit;

    for (i = 0U; i < length; ++i) {
        /* 每个字节先与 CRC 低 8 位异或，再逐位右移计算。 */
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 0x0001U) != 0U) {
                crc = (uint16_t)((crc >> 1U) ^ 0xA001U);
            } else {
                crc >>= 1U;
            }
        }
    }

    return crc;
}
