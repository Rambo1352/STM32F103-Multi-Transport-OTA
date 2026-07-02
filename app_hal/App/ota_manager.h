#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdint.h>

#include "upgrade_protocol.h"

/* 初始化 App 侧 OTA 状态机。 */
void ota_manager_init(void);

/* 处理一帧升级协议数据，并返回处理结果与附加信息。 */
upgrade_status_t ota_manager_handle_frame(const upgrade_frame_t *frame, uint32_t *detail, uint8_t channel);

/* 供外部读取当前 OTA 状态，便于日志任务打印。 */
void ota_manager_get_status(uint32_t *active, uint32_t *received_size, uint32_t *image_size);

/* ACK 已经发送后调用；如果 OTA 已完成，则复位进入 bootloader 执行升级。 */
void ota_manager_poll_reset(void);

#endif /* OTA_MANAGER_H */
