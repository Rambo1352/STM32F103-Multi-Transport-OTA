#ifndef W5500_PORT_H
#define W5500_PORT_H

#include <stdint.h>

/* W5500 项目适配层：把 WIZnet 官方 ioLibrary 需要的 SPI、片选和网络参数集中到这里。 */

/* 初始化 W5500 芯片、SPI 回调、Socket 缓冲区和静态网络参数。 */
int w5500_port_init(void);

/* 查询 W5500 PHY 网线连接状态，返回 1 表示 Link Up，返回 0 表示 Link Down。 */
int w5500_port_is_link_up(void);

/* 查询 W5500 是否已经完成基础初始化，TCP 任务可用它避免重复配置芯片。 */
int w5500_port_is_ready(void);

/* 返回当前 W5500 VERSIONR 寄存器，正常 W5500 通常读到 0x04。 */
uint8_t w5500_port_get_version(void);

#endif /* W5500_PORT_H */
