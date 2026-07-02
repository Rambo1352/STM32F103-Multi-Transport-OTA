#include "ota_manager.h"

#include "Int_w24c02.h"
#include "Int_w25q32.h"
#include "app_config.h"
#include "app_log.h"
#include "app_status.h"
#include "crc32.h"

#include "FreeRTOS.h"
#include "semphr.h"

#include <string.h>

/*
 * 这里直接适配你当前的三工程分工：
 * 1. app_hal 负责通过 UART / W5500 / ESP32 Wi-Fi 接收新固件；
 * 2. 收到的新固件先写入 W25Q32 外部 Flash；
 * 3. 写入 metadata，并通过 24C02 告诉 bootloader 下次上电执行升级；
 * 4. 复位后由 bootloader_hal 把 W25Q32 中的镜像搬运到 STM32 内部 Flash。
 *
 * 也就是说：
 * - STM32 始终是升级主控；
 * - ESP32 只是 USART2 外设网络模块；
 * - 真正的升级控制流依旧全部在 STM32 侧完成。
 */

/* 与 bootloader_hal 保持一致的升级标志地址与校验 key。 */
#define CHECK_UPDATE_ADDR              0x10U
#define CHECK_KEY_ADDR                 0x11U
#define CHECK_KEY                      0x5A6BU
#define BOOT_UPDATE                    0x01U
#define BOOT_NO_UPDATE                 0x02U

/* metadata 固定放在 W25Q32 起始处。 */
#define META_APP_ADDR_BLOCK            0x00U
#define META_APP_ADDR_SECTOR           0x00U
#define META_APP_ADDR_PAGE             0x00U
#define META_APP_ADDR_OFFSET           0x00U

/* 固件实体从 W25Q32 的 0x00001000 开始存放，避开 metadata 区域。 */
#define OTA_W25Q32_IMAGE_START_ADDR    0x00001000UL

/* W25Q32 基本几何参数。 */
#define W25Q32_SECTOR_SIZE             4096UL
#define W25Q32_PAGE_SIZE               256UL
#define OTA_OWNER_CHANNEL_NONE         0xFFU

/* OTA 运行期上下文。 */
typedef struct
{
    uint8_t owner_channel;
    uint8_t active;          /* 当前是否已经进入 OTA 会话。 */
    uint8_t reset_pending;   /* END/RESET_MCU 已处理，等待 ACK 发出后复位。 */
    uint32_t image_size;     /* 待升级镜像总大小。 */
    uint32_t image_crc32;    /* 整包镜像 CRC32。 */
    uint32_t version;        /* 新镜像版本号。 */
    uint32_t received_size;  /* 已成功接收并写入的最大连续字节数。 */
} ota_context_t;

/* 全局 OTA 上下文。 */
static ota_context_t g_ota;
static SemaphoreHandle_t g_ota_mutex = 0;

static void ota_manager_lock(void)
{
    if (g_ota_mutex != 0) {
        (void)xSemaphoreTake(g_ota_mutex, portMAX_DELAY);
    }
}

static void ota_manager_unlock(void)
{
    if (g_ota_mutex != 0) {
        (void)xSemaphoreGive(g_ota_mutex);
    }
}

static upgrade_status_t ota_manager_claim_channel(uint16_t cmd, uint8_t channel)
{
    if (cmd == UPGRADE_CMD_QUERY_INFO) {
        return UPGRADE_STATUS_OK;
    }

    if (g_ota.owner_channel == OTA_OWNER_CHANNEL_NONE) {
        if ((cmd == UPGRADE_CMD_ENTER) || (cmd == UPGRADE_CMD_RESET_MCU)) {
            g_ota.owner_channel = channel;
            return UPGRADE_STATUS_OK;
        }

        if (cmd == UPGRADE_CMD_BEGIN) {
            return UPGRADE_STATUS_OK;
        }

        return UPGRADE_STATUS_INVALID_STATE;
    }

    return (g_ota.owner_channel == channel) ? UPGRADE_STATUS_OK : UPGRADE_STATUS_INVALID_STATE;
}

static void ota_manager_clear_session(void)
{
    g_ota.owner_channel = OTA_OWNER_CHANNEL_NONE;
    g_ota.active = 0U;
    g_ota.image_size = 0U;
    g_ota.image_crc32 = 0U;
    g_ota.received_size = 0U;
}

/* 把 32bit 无符号数按小端序写入字节流。 */
static void ota_store_u32_le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8U) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16U) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

/* 把线性地址拆成 block / sector / page / offset 四段。 */
static void ota_split_addr(uint32_t addr,
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

/* 擦除新固件将要覆盖到的 W25Q32 区域。 */
static int ota_w25q32_erase_image_region(uint32_t image_size)
{
    uint32_t start_addr;
    uint32_t end_addr;
    uint32_t sector_addr;
    uint8_t block;
    uint8_t sector;

    if ((image_size == 0U) || (image_size > APP_SIZE_MAX)) {
        return -1;
    }

    /*
     * 先擦 metadata 所在的起始扇区，
     * 这样新的镜像头信息才能重新写入。
     */
    Int_w25q32_erase_sector(META_APP_ADDR_BLOCK, META_APP_ADDR_SECTOR);

    start_addr = OTA_W25Q32_IMAGE_START_ADDR;
    end_addr = start_addr + image_size;
    sector_addr = start_addr & ~(W25Q32_SECTOR_SIZE - 1UL);

    while (sector_addr < end_addr) {
        ota_split_addr(sector_addr, &block, &sector, 0, 0);
        Int_w25q32_erase_sector(block, sector);
        sector_addr += W25Q32_SECTOR_SIZE;
    }

    return 0;
}

/* 按页切分数据并写入 W25Q32。 */
static int ota_w25q32_write(uint32_t addr, const uint8_t *data, uint16_t len)
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
        ota_split_addr(addr, &block, &sector, &page, &offset);

        chunk = (uint16_t)(W25Q32_PAGE_SIZE - offset);
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

/* 从 W25Q32 读回整包数据并重新计算 CRC32。 */
static int ota_w25q32_verify_crc(uint32_t image_size, uint32_t expected_crc32)
{
    uint8_t buffer[256];
    uint32_t offset = 0U;
    uint32_t crc = crc32_init();
    uint16_t chunk;

    while (offset < image_size) {
        chunk = (uint16_t)(image_size - offset);
        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }

        Int_w25q32_read_data_with_32addr(OTA_W25Q32_IMAGE_START_ADDR + offset, buffer, chunk);
        crc = crc32_update(crc, buffer, chunk);
        offset += chunk;
    }

    return (crc32_finish(crc) == expected_crc32) ? 0 : -1;
}

/* 向 W25Q32 起始位置写入 bootloader 需要的 metadata。 */
static int ota_write_metadata(uint32_t image_size)
{
    uint8_t meta[8];

    ota_store_u32_le(&meta[0], OTA_W25Q32_IMAGE_START_ADDR);
    ota_store_u32_le(&meta[4], image_size);

    Int_w25q32_write_data(META_APP_ADDR_BLOCK,
                          META_APP_ADDR_SECTOR,
                          META_APP_ADDR_PAGE,
                          META_APP_ADDR_OFFSET,
                          meta,
                          sizeof(meta));
    return 0;
}

/* 通知 bootloader 下次启动时执行升级。 */
static int ota_set_bootloader_update_flag(void)
{
    uint8_t data[3];

    data[0] = BOOT_UPDATE;
    data[1] = (uint8_t)(CHECK_KEY >> 8U);
    data[2] = (uint8_t)(CHECK_KEY & 0xFFU);

    Int_w24c02_write_bytes(CHECK_UPDATE_ADDR, data, sizeof(data));
    return 0;
}

void ota_manager_init(void)
{
    if (g_ota_mutex == 0) {
        g_ota_mutex = xSemaphoreCreateMutex();
    }

    /* 每次上电先把 App 侧 OTA 会话状态清零。 */
    ota_manager_lock();
    (void)memset(&g_ota, 0, sizeof(g_ota));
    g_ota.owner_channel = OTA_OWNER_CHANNEL_NONE;
    ota_manager_unlock();
}

static upgrade_status_t ota_manager_handle_frame_locked(const upgrade_frame_t *frame,
                                                        uint32_t *detail,
                                                        uint8_t channel)
{
    upgrade_status_t owner_status;

    if (detail != 0) {
        *detail = 0U;
    }

    if (frame == 0) {
        return UPGRADE_STATUS_INVALID_CMD;
    }

    owner_status = ota_manager_claim_channel(frame->cmd, channel);
    if (owner_status != UPGRADE_STATUS_OK) {
        return owner_status;
    }

    switch (frame->cmd) {
    case UPGRADE_CMD_QUERY_INFO:
        if (detail != 0) {
            /*
             * 为了兼容简单上位机，这里把信息压缩进一个 32bit 值：
             * 高 16 位返回总大小低 16 位快照，
             * 低 16 位返回当前接收进度低 16 位快照。
             */
            *detail = ((g_ota.image_size & 0xFFFFU) << 16U) |
                      (g_ota.received_size & 0xFFFFU);
        }
        return UPGRADE_STATUS_OK;

    case UPGRADE_CMD_ENTER:
        g_ota.active = 1U;
        return UPGRADE_STATUS_OK;

    case UPGRADE_CMD_BEGIN: {
        upgrade_begin_t begin;
        uint32_t erase_start;
        uint32_t erase_ms;
        uint32_t erase_sectors;

        if (upgrade_protocol_decode_begin(frame, &begin) != 0) {
            return UPGRADE_STATUS_INVALID_CMD;
        }

        if ((begin.image_size == 0U) || (begin.image_size > APP_SIZE_MAX)) {
            return UPGRADE_STATUS_INVALID_CMD;
        }

        /*
         * 当前方案只有 boot 区 + app 区，BEGIN 最后 1 字节仅保留，
         * 新 App 固件固定暂存在 W25Q32 的 OTA 区。
        */
        (void)begin.reserved;

        erase_sectors = (begin.image_size + W25Q32_SECTOR_SIZE - 1UL) / W25Q32_SECTOR_SIZE;
        erase_start = HAL_GetTick();
        (void)app_log_printf("[OTA] begin ch=%u size=%lu crc=0x%08lX ver=0x%08lX\r\n",
                             (unsigned int)channel,
                             (unsigned long)begin.image_size,
                             (unsigned long)begin.image_crc32,
                             (unsigned long)begin.version);
        (void)app_log_printf("[OTA] erase start sectors=%lu\r\n",
                             (unsigned long)(erase_sectors + 1UL));

        if (ota_w25q32_erase_image_region(begin.image_size) != 0) {
            erase_ms = HAL_GetTick() - erase_start;
            (void)app_log_printf("[OTA] erase failed %lu ms\r\n", (unsigned long)erase_ms);
            ota_manager_clear_session();
            return UPGRADE_STATUS_FLASH_ERROR;
        }

        erase_ms = HAL_GetTick() - erase_start;
        (void)app_log_printf("[OTA] erase ok %lu ms\r\n", (unsigned long)erase_ms);

        g_ota.active = 1U;
        g_ota.owner_channel = channel;
        g_ota.image_size = begin.image_size;
        g_ota.image_crc32 = begin.image_crc32;
        g_ota.version = begin.version;
        g_ota.received_size = 0U;
        return UPGRADE_STATUS_OK;
    }

    case UPGRADE_CMD_DATA: {
        upgrade_data_packet_t packet;

        if (g_ota.active == 0U) {
            return UPGRADE_STATUS_INVALID_STATE;
        }

        if (upgrade_protocol_decode_data(frame, &packet) != 0) {
            return UPGRADE_STATUS_INVALID_CMD;
        }

        if ((packet.size == 0U) ||
            ((packet.offset + packet.size) < packet.offset) ||
            ((packet.offset + packet.size) > g_ota.image_size)) {
            return UPGRADE_STATUS_INVALID_CMD;
        }

        if (packet.offset != g_ota.received_size) {
            return UPGRADE_STATUS_INVALID_STATE;
        }

        if (ota_w25q32_write(OTA_W25Q32_IMAGE_START_ADDR + packet.offset,
                             packet.bytes,
                             packet.size) != 0) {
            ota_manager_clear_session();
            return UPGRADE_STATUS_FLASH_ERROR;
        }

        g_ota.received_size = packet.offset + packet.size;

        if (detail != 0) {
            *detail = g_ota.received_size;
        }
        return UPGRADE_STATUS_OK;
    }

    case UPGRADE_CMD_END:
        if ((g_ota.active == 0U) || (g_ota.received_size != g_ota.image_size)) {
            return UPGRADE_STATUS_INVALID_STATE;
        }

        if (ota_w25q32_verify_crc(g_ota.image_size, g_ota.image_crc32) != 0) {
            ota_manager_clear_session();
            return UPGRADE_STATUS_VERIFY_FAILED;
        }

        if ((ota_write_metadata(g_ota.image_size) != 0) ||
            (ota_set_bootloader_update_flag() != 0)) {
            ota_manager_clear_session();
            return UPGRADE_STATUS_FLASH_ERROR;
        }

        g_ota.active = 0U;
        g_ota.owner_channel = OTA_OWNER_CHANNEL_NONE;
        g_ota.reset_pending = 1U;

        /*
         * ACK 发回上位机后再复位进入 bootloader_hal，
         * 由 bootloader 完成“W25Q32 -> STM32 内部 Flash”的最终烧录。
         */
        if (detail != 0) {
            *detail = g_ota.image_size;
        }
        return UPGRADE_STATUS_OK;

    case UPGRADE_CMD_RESET_MCU:
        g_ota.reset_pending = 1U;
        return UPGRADE_STATUS_OK;

    default:
        return UPGRADE_STATUS_UNSUPPORTED;
    }
}

upgrade_status_t ota_manager_handle_frame(const upgrade_frame_t *frame, uint32_t *detail, uint8_t channel)
{
    upgrade_status_t status;

    ota_manager_lock();
    status = ota_manager_handle_frame_locked(frame, detail, channel);
    ota_manager_unlock();

    return status;
}

void ota_manager_get_status(uint32_t *active, uint32_t *received_size, uint32_t *image_size)
{
    ota_manager_lock();

    if (active != 0) {
        *active = g_ota.active;
    }

    if (received_size != 0) {
        *received_size = g_ota.received_size;
    }

    if (image_size != 0) {
        *image_size = g_ota.image_size;
    }

    ota_manager_unlock();
}

void app_status_get(app_status_t *status)
{
    if (status == 0) {
        return;
    }

    ota_manager_lock();

    status->version = APP_VERSION;
    status->ota_active = g_ota.active;
    status->staging_addr = OTA_W25Q32_IMAGE_START_ADDR;
    status->received_size = g_ota.received_size;

    ota_manager_unlock();
}

void ota_manager_poll_reset(void)
{
    uint8_t reset_pending;

    ota_manager_lock();
    reset_pending = g_ota.reset_pending;
    g_ota.reset_pending = 0U;
    ota_manager_unlock();

    if (reset_pending == 0U) {
        return;
    }

    HAL_Delay(20U);
    HAL_NVIC_SystemReset();
}
