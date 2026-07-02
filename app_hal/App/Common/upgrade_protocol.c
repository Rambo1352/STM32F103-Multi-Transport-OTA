#include "upgrade_protocol.h"

#include "crc16_modbus.h"

static uint16_t load_u16_le(const uint8_t *p)
{
    /* 协议字段统一采用小端序，便于 STM32 直接解析。 */
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8U);
}

static uint32_t load_u32_le(const uint8_t *p)
{
    /* 从字节流中恢复 32 位小端整数。 */
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}

static void store_u16_le(uint8_t *p, uint16_t value)
{
    /* 把 16 位整数写成小端字节序。 */
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static void store_u32_le(uint8_t *p, uint32_t value)
{
    /* 把 32 位整数写成小端字节序。 */
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8U) & 0xFFU);
    p[2] = (uint8_t)((value >> 16U) & 0xFFU);
    p[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

upgrade_parse_result_t upgrade_protocol_parse(const uint8_t *buffer,
                                               size_t size,
                                               upgrade_frame_t *frame)
{
    uint16_t len;
    size_t expected_size;
    uint16_t actual_crc;
    uint16_t expected_crc;

    if ((buffer == 0) || (frame == 0) || (size < UPGRADE_FRAME_OVERHEAD)) {
        return UPGRADE_PARSE_TOO_SHORT;
    }

    /* 先读数据区长度，再计算整帧应有长度。 */
    len = load_u16_le(&buffer[2]);
    if (len > UPGRADE_FRAME_MAX_DATA) {
        return UPGRADE_PARSE_TOO_LONG;
    }

    expected_size = UPGRADE_FRAME_OVERHEAD + len;
    if (size < expected_size) {
        return UPGRADE_PARSE_TOO_SHORT;
    }

    if (size > expected_size) {
        return UPGRADE_PARSE_TOO_LONG;
    }

    actual_crc = crc16_modbus(buffer, expected_size - UPGRADE_FRAME_CRC_SIZE);
    expected_crc = load_u16_le(&buffer[expected_size - UPGRADE_FRAME_CRC_SIZE]);
    if (actual_crc != expected_crc) {
        return UPGRADE_PARSE_BAD_CRC;
    }

    /* CRC 通过后才把帧内容交给上层状态机。 */
    frame->cmd = load_u16_le(&buffer[0]);
    frame->len = len;
    frame->data = &buffer[4];
    frame->reserve = load_u32_le(&buffer[4 + len]);

    return UPGRADE_PARSE_OK;
}

int upgrade_protocol_build(uint16_t cmd,
                           const uint8_t *data,
                           uint16_t len,
                           uint8_t *out,
                           size_t out_capacity,
                           size_t *out_size)
{
    size_t total;
    uint16_t crc;
    uint16_t i;

    if ((out == 0) || (out_size == 0) || (len > UPGRADE_FRAME_MAX_DATA)) {
        return -1;
    }

    /* 输出缓冲区必须能容纳帧头、数据、保留字段和 CRC。 */
    total = UPGRADE_FRAME_OVERHEAD + len;
    if (out_capacity < total) {
        return -1;
    }

    store_u16_le(&out[0], cmd);
    store_u16_le(&out[2], len);

    for (i = 0U; i < len; ++i) {
        if (data == 0) {
            return -1;
        }
        out[4U + i] = data[i];
    }

    store_u32_le(&out[4U + len], 0UL);
    /* CRC 覆盖除最后 CRC 字段之外的全部内容。 */
    crc = crc16_modbus(out, total - UPGRADE_FRAME_CRC_SIZE);
    store_u16_le(&out[total - UPGRADE_FRAME_CRC_SIZE], crc);
    *out_size = total;

    return 0;
}

int upgrade_protocol_decode_begin(const upgrade_frame_t *frame, upgrade_begin_t *begin)
{
    if ((frame == 0) || (begin == 0) || (frame->cmd != UPGRADE_CMD_BEGIN) || (frame->len != 13U)) {
        return -1;
    }

    /* BEGIN 负载：镜像大小、镜像 CRC32、版本号、保留字段。 */
    begin->image_size = load_u32_le(&frame->data[0]);
    begin->image_crc32 = load_u32_le(&frame->data[4]);
    begin->version = load_u32_le(&frame->data[8]);
    begin->reserved = frame->data[12];

    return 0;
}

int upgrade_protocol_decode_data(const upgrade_frame_t *frame, upgrade_data_packet_t *packet)
{
    uint16_t size;

    if ((frame == 0) || (packet == 0) || (frame->cmd != UPGRADE_CMD_DATA) || (frame->len < 6U)) {
        return -1;
    }

    /* DATA 负载：偏移(4) + 本包长度(2) + 数据。 */
    size = load_u16_le(&frame->data[4]);
    if ((uint16_t)(size + 6U) != frame->len) {
        return -1;
    }

    packet->offset = load_u32_le(&frame->data[0]);
    packet->size = size;
    packet->bytes = &frame->data[6];

    return 0;
}

int upgrade_protocol_build_ack(const upgrade_ack_t *ack,
                               uint8_t *out,
                               size_t out_capacity,
                               size_t *out_size)
{
    uint8_t payload[8];

    if (ack == 0) {
        return -1;
    }

    /* ACK 负载固定 8 字节，detail 可用于返回进度或错误细节。 */
    store_u16_le(&payload[0], ack->request_cmd);
    store_u16_le(&payload[2], ack->status);
    store_u32_le(&payload[4], ack->detail);

    return upgrade_protocol_build(UPGRADE_CMD_ACK, payload, sizeof(payload), out, out_capacity, out_size);
}
