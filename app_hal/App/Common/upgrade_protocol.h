#ifndef UPGRADE_PROTOCOL_H
#define UPGRADE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define UPGRADE_FRAME_HEADER_SIZE   4U
#define UPGRADE_FRAME_RESERVE_SIZE  4U
#define UPGRADE_FRAME_CRC_SIZE      2U
#define UPGRADE_FRAME_OVERHEAD      (UPGRADE_FRAME_HEADER_SIZE + UPGRADE_FRAME_RESERVE_SIZE + UPGRADE_FRAME_CRC_SIZE)
#define UPGRADE_FRAME_MAX_DATA      1024U
#define UPGRADE_FRAME_MAX_SIZE      (UPGRADE_FRAME_OVERHEAD + UPGRADE_FRAME_MAX_DATA)

typedef enum {
    UPGRADE_CMD_QUERY_INFO    = 0x0001U,
    UPGRADE_CMD_ENTER         = 0x0002U,
    UPGRADE_CMD_BEGIN         = 0x0003U,
    UPGRADE_CMD_DATA          = 0x0004U,
    UPGRADE_CMD_END           = 0x0005U,
    UPGRADE_CMD_RESET_MCU     = 0x0006U,
    UPGRADE_CMD_ACK           = 0x8000U
} upgrade_cmd_t;

typedef enum {
    UPGRADE_STATUS_OK = 0,
    UPGRADE_STATUS_CRC_ERROR = 1,
    UPGRADE_STATUS_INVALID_CMD = 2,
    UPGRADE_STATUS_INVALID_STATE = 3,
    UPGRADE_STATUS_FLASH_ERROR = 4,
    UPGRADE_STATUS_VERIFY_FAILED = 5,
    UPGRADE_STATUS_UNSUPPORTED = 6
} upgrade_status_t;

typedef struct {
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t version;
    uint8_t reserved;
} upgrade_begin_t;

typedef struct {
    uint32_t offset;
    uint16_t size;
    const uint8_t *bytes;
} upgrade_data_packet_t;

typedef struct {
    uint16_t request_cmd;
    uint16_t status;
    uint32_t detail;
} upgrade_ack_t;

typedef struct {
    uint16_t cmd;
    uint16_t len;
    const uint8_t *data;
    uint32_t reserve;
} upgrade_frame_t;

typedef enum {
    UPGRADE_PARSE_OK = 0,
    UPGRADE_PARSE_TOO_SHORT,
    UPGRADE_PARSE_TOO_LONG,
    UPGRADE_PARSE_BAD_CRC
} upgrade_parse_result_t;

upgrade_parse_result_t upgrade_protocol_parse(const uint8_t *buffer,
                                               size_t size,
                                               upgrade_frame_t *frame);
int upgrade_protocol_build(uint16_t cmd,
                           const uint8_t *data,
                           uint16_t len,
                           uint8_t *out,
                           size_t out_capacity,
                           size_t *out_size);
int upgrade_protocol_decode_begin(const upgrade_frame_t *frame, upgrade_begin_t *begin);
int upgrade_protocol_decode_data(const upgrade_frame_t *frame, upgrade_data_packet_t *packet);
int upgrade_protocol_build_ack(const upgrade_ack_t *ack,
                               uint8_t *out,
                               size_t out_capacity,
                               size_t *out_size);

#endif
