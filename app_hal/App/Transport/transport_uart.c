#include "transport_uart.h"

#include "app_config.h"
#include "app_log.h"
#include "Int_w24c02.h"
#include "Int_w25q32.h"
#include "ota_manager.h"
#include "transport_common.h"
#include "uart_bus.h"
#include "upgrade_protocol.h"
#include "usart.h"

#include <stdlib.h>
#include <string.h>

#define UART_DIRECT_CMD_MAX_LEN         32U
#define UART_DIRECT_WRITE_CHUNK         256U
#define UART_RX_RING_SIZE               512U
#define UART_DIRECT_IMAGE_START_ADDR    0x00001000UL
#define UART_DIRECT_W25Q32_SECTOR_SIZE  4096UL
#define UART_DIRECT_CHECK_UPDATE_ADDR   0x10U
#define UART_DIRECT_CHECK_KEY           0x5A6BU
#define UART_DIRECT_BOOT_UPDATE         0x01U

typedef enum {
    UART_DIRECT_IDLE = 0,
    UART_DIRECT_RECEIVING
} uart_direct_state_t;

static uart_direct_state_t g_uart_direct_state = UART_DIRECT_IDLE;
static uint32_t g_uart_direct_image_size = 0U;
static uint32_t g_uart_direct_received_size = 0U;
static uint8_t g_uart_direct_buffer[UART_DIRECT_WRITE_CHUNK];
static uint16_t g_uart_direct_buffer_len = 0U;
static volatile uint16_t g_uart_rx_head = 0U;
static volatile uint16_t g_uart_rx_tail = 0U;
static volatile uint8_t g_uart_rx_overflow = 0U;
static uint8_t g_uart_rx_ring[UART_RX_RING_SIZE];

static const uint8_t UART_DIRECT_MSG_ERASE[] = "[UART OTA] erase...\r\n";
static const uint8_t UART_DIRECT_MSG_ERASE_FAILED[] = "[UART OTA] erase failed\r\n";
static const uint8_t UART_DIRECT_MSG_READY[] = "[UART OTA] ready\r\n";
static const uint8_t UART_DIRECT_MSG_RECV_OK[] = "[UART OTA] recv ok, reset\r\n";
static const uint8_t UART_DIRECT_MSG_WRITE_FAILED[] = "[UART OTA] write failed\r\n";

static void uart_rx_ring_push(uint8_t byte)
{
    uint16_t next = (uint16_t)(g_uart_rx_head + 1U);

    if (next >= UART_RX_RING_SIZE) {
        next = 0U;
    }

    if (next == g_uart_rx_tail) {
        g_uart_rx_overflow = 1U;
        return;
    }

    g_uart_rx_ring[g_uart_rx_head] = byte;
    g_uart_rx_head = next;
}

static int uart_rx_ring_pop(uint8_t *byte)
{
    uint16_t tail;

    if (byte == 0) {
        return -1;
    }

    __disable_irq();
    if (g_uart_rx_tail == g_uart_rx_head) {
        __enable_irq();
        return -1;
    }

    tail = g_uart_rx_tail;
    *byte = g_uart_rx_ring[tail];

    tail = (uint16_t)(tail + 1U);
    if (tail >= UART_RX_RING_SIZE) {
        tail = 0U;
    }
    g_uart_rx_tail = tail;
    __enable_irq();

    return 0;
}

static void uart_rx_ring_reset(void)
{
    __disable_irq();
    g_uart_rx_head = 0U;
    g_uart_rx_tail = 0U;
    g_uart_rx_overflow = 0U;
    __enable_irq();
}

/* 从 UART 环形缓冲读取 1 字节，超时返回 -1。 */
static int uart_recv_byte(uint8_t *byte, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < timeout_ms) {
        if (uart_rx_ring_pop(byte) == 0) {
            return 0;
        }
    }

    return -1;
}

void transport_uart_irq_handler(void)
{
    uint32_t sr = huart1.Instance->SR;
    uint8_t data;

    if ((sr & (USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)) != 0U) {
        (void)huart1.Instance->SR;
        (void)huart1.Instance->DR;
        huart1.ErrorCode = HAL_UART_ERROR_NONE;
        return;
    }

    if ((sr & USART_SR_RXNE) != 0U) {
        data = (uint8_t)(huart1.Instance->DR & 0xFFU);
        uart_rx_ring_push(data);
    }
}

static void uart_direct_split_addr(uint32_t addr,
                                   uint8_t *block,
                                   uint8_t *sector,
                                   uint8_t *page,
                                   uint8_t *offset)
{
    if (block != 0) {
        *block = (uint8_t)((addr >> 16U) & 0x3FU);
    }

    if (sector != 0) {
        *sector = (uint8_t)((addr >> 12U) & 0x0FU);
    }

    if (page != 0) {
        *page = (uint8_t)((addr >> 8U) & 0x0FU);
    }

    if (offset != 0) {
        *offset = (uint8_t)(addr & 0xFFU);
    }
}

static void uart_direct_store_u32_le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8U) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16U) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static int uart_direct_erase_image_region(uint32_t image_size)
{
    uint32_t start_addr;
    uint32_t end_addr;
    uint32_t sector_addr;
    uint8_t block;
    uint8_t sector;

    if ((image_size == 0U) || (image_size > APP_SIZE_MAX)) {
        return -1;
    }

    Int_w25q32_erase_sector(0U, 0U);

    start_addr = UART_DIRECT_IMAGE_START_ADDR;
    end_addr = start_addr + image_size;
    sector_addr = start_addr & ~(UART_DIRECT_W25Q32_SECTOR_SIZE - 1UL);

    while (sector_addr < end_addr) {
        uart_direct_split_addr(sector_addr, &block, &sector, 0, 0);
        Int_w25q32_erase_sector(block, sector);
        sector_addr += UART_DIRECT_W25Q32_SECTOR_SIZE;
    }

    return 0;
}

static int uart_direct_write(uint32_t addr, const uint8_t *data, uint16_t len)
{
    uint16_t chunk;
    uint8_t block;
    uint8_t sector;
    uint8_t page;
    uint8_t offset;

    if ((data == 0) || (len == 0U)) {
        return -1;
    }

    while (len > 0U) {
        uart_direct_split_addr(addr, &block, &sector, &page, &offset);

        chunk = (uint16_t)(UART_DIRECT_WRITE_CHUNK - offset);
        if (chunk > len) {
            chunk = len;
        }

        Int_w25q32_write_data(block, sector, page, offset, data, chunk);

        addr += chunk;
        data += chunk;
        len = (uint16_t)(len - chunk);
    }

    return 0;
}

static int uart_direct_flush_buffer(void)
{
    uint32_t write_addr;

    if (g_uart_direct_buffer_len == 0U) {
        return 0;
    }

    write_addr = UART_DIRECT_IMAGE_START_ADDR + g_uart_direct_received_size;
    if (uart_direct_write(write_addr, g_uart_direct_buffer, g_uart_direct_buffer_len) != 0) {
        return -1;
    }

    g_uart_direct_received_size += g_uart_direct_buffer_len;
    g_uart_direct_buffer_len = 0U;

    return 0;
}

static void uart_direct_write_metadata(uint32_t image_size)
{
    uint8_t meta[8];

    uart_direct_store_u32_le(&meta[0], UART_DIRECT_IMAGE_START_ADDR);
    uart_direct_store_u32_le(&meta[4], image_size);

    Int_w25q32_write_data(0U, 0U, 0U, 0U, meta, sizeof(meta));
}

static void uart_direct_set_update_flag(void)
{
    uint8_t data[3];

    data[0] = UART_DIRECT_BOOT_UPDATE;
    data[1] = (uint8_t)(UART_DIRECT_CHECK_KEY >> 8U);
    data[2] = (uint8_t)(UART_DIRECT_CHECK_KEY & 0xFFU);

    Int_w24c02_write_bytes(UART_DIRECT_CHECK_UPDATE_ADDR, data, sizeof(data));
}

/* 先读帧头拿到长度，再继续接收完整协议帧。 */
static int uart_recv_frame_with_first(uint8_t first_byte,
                                      uint8_t *buffer,
                                      size_t capacity,
                                      size_t *received,
                                      uint32_t timeout_ms)
{
    uint16_t payload_len;
    size_t need_size;
    size_t i;

    if ((buffer == 0) || (received == 0) || (capacity < UPGRADE_FRAME_OVERHEAD)) {
        return -1;
    }

    *received = 0U;
    buffer[0] = first_byte;

    for (i = 1U; i < 4U; ++i) {
        if (uart_recv_byte(&buffer[i], timeout_ms) != 0) {
            return -1;
        }
    }

    payload_len = (uint16_t)buffer[2] | ((uint16_t)buffer[3] << 8U);
    need_size = UPGRADE_FRAME_OVERHEAD + (size_t)payload_len;
    if (need_size > capacity) {
        return -1;
    }

    app_log_set_enabled(0U);

    for (i = 4U; i < need_size; ++i) {
        if (uart_recv_byte(&buffer[i], timeout_ms) != 0) {
            return -1;
        }
    }

    *received = need_size;
    return 0;
}

/* 通过 USART1 发送 ACK 帧。 */
static void uart_send_bytes(const uint8_t *buffer, size_t size)
{
    uart_bus_lock();
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)buffer, (uint16_t)size, HAL_MAX_DELAY);
    uart_bus_unlock();
}

static void uart_direct_reset_state(void)
{
    g_uart_direct_state = UART_DIRECT_IDLE;
    g_uart_direct_image_size = 0U;
    g_uart_direct_received_size = 0U;
    g_uart_direct_buffer_len = 0U;
}

static int uart_direct_try_start(uint8_t first_byte)
{
    char cmd[UART_DIRECT_CMD_MAX_LEN];
    uint16_t len = 0U;
    uint8_t byte;
    uint32_t image_size;

    if (first_byte != (uint8_t)'s') {
        return 0;
    }

    cmd[len++] = (char)first_byte;

    while (len < (UART_DIRECT_CMD_MAX_LEN - 1U)) {
        if (uart_recv_byte(&byte, 20U) != 0) {
            break;
        }

        if ((byte == (uint8_t)'\r') || (byte == (uint8_t)'\n')) {
            break;
        }

        cmd[len++] = (char)byte;
    }

    cmd[len] = '\0';

    if (strncmp(cmd, "start:", 6U) != 0) {
        return 0;
    }

    image_size = (uint32_t)strtoul(&cmd[6], 0, 10);
    if ((image_size == 0U) || (image_size > APP_SIZE_MAX)) {
        (void)app_log_printf("[UART OTA] len error: %lu\r\n", (unsigned long)image_size);
        return 1;
    }

    uart_rx_ring_reset();
    app_log_set_enabled(0U);
    uart_bus_lock();
    (void)HAL_UART_Transmit(&huart1,
                            (uint8_t *)UART_DIRECT_MSG_ERASE,
                            (uint16_t)(sizeof(UART_DIRECT_MSG_ERASE) - 1U),
                            HAL_MAX_DELAY);

    if (uart_direct_erase_image_region(image_size) != 0) {
        (void)HAL_UART_Transmit(&huart1,
                                (uint8_t *)UART_DIRECT_MSG_ERASE_FAILED,
                                (uint16_t)(sizeof(UART_DIRECT_MSG_ERASE_FAILED) - 1U),
                                HAL_MAX_DELAY);
        uart_bus_unlock();
        app_log_set_enabled(1U);
        uart_direct_reset_state();
        return 1;
    }

    g_uart_direct_state = UART_DIRECT_RECEIVING;
    g_uart_direct_image_size = image_size;
    g_uart_direct_received_size = 0U;
    g_uart_direct_buffer_len = 0U;

    (void)HAL_UART_Transmit(&huart1,
                            (uint8_t *)UART_DIRECT_MSG_READY,
                            (uint16_t)(sizeof(UART_DIRECT_MSG_READY) - 1U),
                            HAL_MAX_DELAY);
    uart_bus_unlock();

    return 1;
}

static void uart_direct_finish_ok(void)
{
    uart_direct_write_metadata(g_uart_direct_image_size);
    uart_direct_set_update_flag();

    uart_bus_lock();
    (void)HAL_UART_Transmit(&huart1,
                            (uint8_t *)UART_DIRECT_MSG_RECV_OK,
                            (uint16_t)(sizeof(UART_DIRECT_MSG_RECV_OK) - 1U),
                            HAL_MAX_DELAY);
    uart_bus_unlock();

    HAL_Delay(50U);
    HAL_NVIC_SystemReset();
}

static void uart_direct_receive_bin(void)
{
    uint8_t byte;

    while (g_uart_direct_received_size + g_uart_direct_buffer_len < g_uart_direct_image_size) {
        if (uart_recv_byte(&byte, 1000U) != 0) {
            return;
        }

        g_uart_direct_buffer[g_uart_direct_buffer_len++] = byte;

        if (g_uart_direct_buffer_len >= sizeof(g_uart_direct_buffer)) {
            if (uart_direct_flush_buffer() != 0) {
                uart_bus_lock();
                (void)HAL_UART_Transmit(&huart1,
                                        (uint8_t *)UART_DIRECT_MSG_WRITE_FAILED,
                                        (uint16_t)(sizeof(UART_DIRECT_MSG_WRITE_FAILED) - 1U),
                                        HAL_MAX_DELAY);
                uart_bus_unlock();
                app_log_set_enabled(1U);
                uart_direct_reset_state();
                return;
            }
        }
    }

    if (uart_direct_flush_buffer() != 0) {
        uart_bus_lock();
        (void)HAL_UART_Transmit(&huart1,
                                (uint8_t *)UART_DIRECT_MSG_WRITE_FAILED,
                                (uint16_t)(sizeof(UART_DIRECT_MSG_WRITE_FAILED) - 1U),
                                HAL_MAX_DELAY);
        uart_bus_unlock();
        app_log_set_enabled(1U);
        uart_direct_reset_state();
        return;
    }

    if (g_uart_direct_received_size == g_uart_direct_image_size) {
        uart_direct_finish_ok();
    }
}

void transport_uart_init(void)
{
    uart_rx_ring_reset();
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    __HAL_UART_CLEAR_FEFLAG(&huart1);
    __HAL_UART_CLEAR_NEFLAG(&huart1);
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_ERR);
}

#if 0
static int uart_recv_frame(uint8_t *buffer, size_t capacity, size_t *received, uint32_t timeout_ms)
{
    uint8_t first_byte;

    if (uart_recv_byte(&first_byte, timeout_ms) != 0) {
        return -1;
    }

    return uart_recv_frame_with_first(first_byte, buffer, capacity, received, timeout_ms);
}

void transport_uart_poll(void)
{
    uint8_t rx[UPGRADE_FRAME_MAX_SIZE];
    uint8_t tx[UPGRADE_FRAME_OVERHEAD + 8U];
    size_t rx_size = 0U;
    size_t tx_size = 0U;

    if (uart_recv_frame(rx, sizeof(rx), &rx_size, APP_UART_FRAME_TIMEOUT_MS) != 0) {
        return;
    }

    if (transport_common_handle_frame(TRANSPORT_CHANNEL_UART,
                                      rx,
                                      rx_size,
                                      tx,
                                      sizeof(tx),
                                      &tx_size) == 0) {
        uart_send_bytes(tx, tx_size);
        ota_manager_poll_reset();
    }
}
#endif

void transport_uart_poll(void)
{
    uint8_t rx[UPGRADE_FRAME_MAX_SIZE];
    uint8_t tx[UPGRADE_FRAME_OVERHEAD + 8U];
    uint8_t first_byte;
    size_t rx_size = 0U;
    size_t tx_size = 0U;

    if (g_uart_direct_state == UART_DIRECT_RECEIVING) {
        uart_direct_receive_bin();
        return;
    }

    if (uart_recv_byte(&first_byte, APP_UART_FRAME_TIMEOUT_MS) != 0) {
        return;
    }

    if (first_byte == (uint8_t)'s') {
        if (uart_direct_try_start(first_byte) != 0) {
            return;
        }
    }

    if (uart_recv_frame_with_first(first_byte, rx, sizeof(rx), &rx_size, APP_UART_FRAME_TIMEOUT_MS) != 0) {
        return;
    }

    if (transport_common_handle_frame(TRANSPORT_CHANNEL_UART,
                                      rx,
                                      rx_size,
                                      tx,
                                      sizeof(tx),
                                      &tx_size) == 0) {
        uart_send_bytes(tx, tx_size);
        ota_manager_poll_reset();
    }
}
