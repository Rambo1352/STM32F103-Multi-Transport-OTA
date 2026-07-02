#include "crc32.h"

uint32_t crc32_init(void)
{
    /* 标准 CRC32 初值，用于固件整包校验。 */
    return 0xFFFFFFFFUL;
}

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    size_t i;
    uint8_t bit;

    for (i = 0U; i < length; ++i) {
        /* 使用反射算法逐字节更新，适合小内存单片机分块计算。 */
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 1UL) != 0UL) {
                crc = (crc >> 1U) ^ 0xEDB88320UL;
            } else {
                crc >>= 1U;
            }
        }
    }

    return crc;
}

uint32_t crc32_finish(uint32_t crc)
{
    /* 结束时取反，得到和常见上位机工具一致的 CRC32。 */
    return crc ^ 0xFFFFFFFFUL;
}

uint32_t crc32_compute(const uint8_t *data, size_t length)
{
    /* 便捷接口：一次性完成初始化、更新和结束处理。 */
    return crc32_finish(crc32_update(crc32_init(), data, length));
}
