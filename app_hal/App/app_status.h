#ifndef APP_STATUS_H
#define APP_STATUS_H

#include <stdint.h>

/*
 * 该结构体用于给 FreeRTOS 打印任务提供“当前 App 状态快照”，
 * 这样打印任务不需要知道 OTA 内部细节，只负责周期输出。
 */
typedef struct
{
    uint32_t version;        /* 当前正在运行的 App 固件版本号。 */
    uint32_t ota_active;     /* 当前是否已经进入 OTA 升级会话。 */
    uint32_t staging_addr;   /* 新 App 固件在 W25Q32 中的暂存起始地址。 */
    uint32_t received_size;  /* 当前已经成功接收并写入的固件字节数。 */
} app_status_t;

/* 获取一份当前 App 状态快照。 */
void app_status_get(app_status_t *status);

#endif /* APP_STATUS_H */
