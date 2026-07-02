#include "w5500_port.h"

#include "app_log.h"
#include "spi_bus.h"
#include "main.h"
#include "spi.h"
#include "w5500.h"
#include "wizchip_conf.h"

/*
 * 说明：
 * 当前 App 工程实际配置的是 SPI2。
 * 注意：W5500 必须拥有独立片选脚，不能和 W25Q32 共用 PC13。
 * 片选脚在 main.h 中集中配置；接板调试时请按原理图修改。
 */
#define W5500_CS_GPIO_PORT        W5500_CS_GPIO_Port
#define W5500_CS_GPIO_PIN         W5500_CS_Pin
#define W5500_RST_GPIO_PORT       W5500_RST_GPIO_Port
#define W5500_RST_GPIO_PIN        W5500_RST_Pin

/* W5500 共有 8 个 socket，每个 socket 分配 2KB TX/RX 缓冲，总计 16KB，符合 W5500 内部 RAM 限制。 */
#define W5500_SOCKET_BUFFER_KB    2U

/* 静态 IP 参数：电脑上位机可配置到同一网段，例如 192.168.1.x，然后连接 192.168.1.88:5000。 */
#define W5500_NETINFO_MAC0        0x00U
#define W5500_NETINFO_MAC1        0x08U
#define W5500_NETINFO_MAC2        0xDCU
#define W5500_NETINFO_MAC3        0x11U
#define W5500_NETINFO_MAC4        0x22U
#define W5500_NETINFO_MAC5        0x33U

#define W5500_NETINFO_IP0         192U
#define W5500_NETINFO_IP1         168U
#define W5500_NETINFO_IP2         1U
#define W5500_NETINFO_IP3         88U

#define W5500_NETINFO_MASK0       255U
#define W5500_NETINFO_MASK1       255U
#define W5500_NETINFO_MASK2       255U
#define W5500_NETINFO_MASK3       0U

#define W5500_NETINFO_GATEWAY0    192U
#define W5500_NETINFO_GATEWAY1    168U
#define W5500_NETINFO_GATEWAY2    1U
#define W5500_NETINFO_GATEWAY3    1U

#define W5500_NETINFO_DNS0        8U
#define W5500_NETINFO_DNS1        8U
#define W5500_NETINFO_DNS2        8U
#define W5500_NETINFO_DNS3        8U

/* 初始化成功后置 1，避免 TCP 任务每次轮询都重复 reset W5500。 */
static uint8_t g_w5500_ready = 0U;

static void w5500_hardware_reset(void)
{
    HAL_GPIO_WritePin(W5500_CS_GPIO_PORT, W5500_CS_GPIO_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(W5500_RST_GPIO_PORT, W5500_RST_GPIO_PIN, GPIO_PIN_RESET);
    HAL_Delay(10U);
    HAL_GPIO_WritePin(W5500_RST_GPIO_PORT, W5500_RST_GPIO_PIN, GPIO_PIN_SET);
    HAL_Delay(100U);
}

/* 片选拉低：WIZnet ioLibrary 每次访问寄存器/缓冲区前都会调用。 */
static void w5500_select(void)
{
    spi_bus_lock();
    HAL_GPIO_WritePin(W5500_CS_GPIO_PORT, W5500_CS_GPIO_PIN, GPIO_PIN_RESET);
}

/* 片选拉高：一次 W5500 SPI 事务结束。 */
static void w5500_deselect(void)
{
    HAL_GPIO_WritePin(W5500_CS_GPIO_PORT, W5500_CS_GPIO_PIN, GPIO_PIN_SET);
    spi_bus_unlock();
}

/* 单字节读：W5500 在读寄存器/缓存时需要发送 dummy byte 来换回 MISO 数据。 */
static uint8_t w5500_spi_read_byte(void)
{
    uint8_t tx = 0xFFU;
    uint8_t rx = 0U;

    (void)HAL_SPI_TransmitReceive(&hspi2, &tx, &rx, 1U, HAL_MAX_DELAY);
    return rx;
}

/* 单字节写：W5500 地址阶段和小数据写入会走这个回调。 */
static void w5500_spi_write_byte(uint8_t value)
{
    (void)HAL_SPI_Transmit(&hspi2, &value, 1U, HAL_MAX_DELAY);
}

/* 连续读：W5500 的 socket RX 缓冲区通常一次读多个字节，用 burst 方式更高效。 */
static void w5500_spi_read_burst(uint8_t *buffer, uint16_t length)
{
    uint16_t i;

    if (buffer == 0) {
        return;
    }

    for (i = 0U; i < length; ++i) {
        buffer[i] = w5500_spi_read_byte();
    }
}

/* 连续写：ACK 或数据帧发送时会把一段数据连续写入 W5500 的 socket TX 缓冲区。 */
static void w5500_spi_write_burst(uint8_t *buffer, uint16_t length)
{
    if ((buffer == 0) || (length == 0U)) {
        return;
    }

    (void)HAL_SPI_Transmit(&hspi2, buffer, length, HAL_MAX_DELAY);
}

/*
 * 进入 W5500 临界区。
 * 当前工程只有 task_tcp_upgrade 一个 FreeRTOS 任务访问 W5500，所以这里保持轻量空实现，
 * 避免在 1KB 数据帧 burst 读写期间长时间关闭中断、影响 SysTick 和其他实时任务。
 * 如果后续 DHCP、HTTP 或其他任务也会访问 W5500，应在更高层加互斥锁保护整次 socket 操作。
 */
static void w5500_enter_critical(void)
{
    /* 单任务访问 W5500 时无需额外处理。 */
}

/* 退出 W5500 临界区，与 w5500_enter_critical 保持成对空实现。 */
static void w5500_exit_critical(void)
{
    /* 单任务访问 W5500 时无需额外处理。 */
}

/* 配置 W5500 静态网络参数。 */
static void w5500_apply_static_netinfo(void)
{
    wiz_NetInfo netinfo = {
        {W5500_NETINFO_MAC0, W5500_NETINFO_MAC1, W5500_NETINFO_MAC2,
         W5500_NETINFO_MAC3, W5500_NETINFO_MAC4, W5500_NETINFO_MAC5},
        {W5500_NETINFO_IP0, W5500_NETINFO_IP1, W5500_NETINFO_IP2, W5500_NETINFO_IP3},
        {W5500_NETINFO_MASK0, W5500_NETINFO_MASK1, W5500_NETINFO_MASK2, W5500_NETINFO_MASK3},
        {W5500_NETINFO_GATEWAY0, W5500_NETINFO_GATEWAY1, W5500_NETINFO_GATEWAY2, W5500_NETINFO_GATEWAY3},
        {W5500_NETINFO_DNS0, W5500_NETINFO_DNS1, W5500_NETINFO_DNS2, W5500_NETINFO_DNS3},
        NETINFO_STATIC
    };

    wizchip_setnetinfo(&netinfo);
}

static void w5500_apply_phy_config(void)
{
    wiz_PhyConf phyconf = {
        PHY_CONFBY_SW,
        PHY_MODE_AUTONEGO,
        PHY_SPEED_100,
        PHY_DUPLEX_FULL
    };

    wizphy_setphyconf(&phyconf);
    HAL_Delay(500U);
    (void)app_log_printf("[W5500] phycfgr=0x%02X\r\n", (unsigned int)getPHYCFGR());
}

int w5500_port_init(void)
{
    uint8_t tx_size[_WIZCHIP_SOCK_NUM_];
    uint8_t rx_size[_WIZCHIP_SOCK_NUM_];
    uint8_t i;

    if (g_w5500_ready != 0U) {
        return 0;
    }

    /* 默认不选中 W5500，避免 SPI 初始化前片选误拉低造成芯片误收数据。 */
    w5500_hardware_reset();

    /* 把 WIZnet 官方库中的弱/空回调替换成当前 STM32 HAL 工程的真实 SPI 操作。 */
    reg_wizchip_cris_cbfunc(w5500_enter_critical, w5500_exit_critical);
    reg_wizchip_cs_cbfunc(w5500_select, w5500_deselect);
    reg_wizchip_spi_cbfunc(w5500_spi_read_byte, w5500_spi_write_byte);
    reg_wizchip_spiburst_cbfunc(w5500_spi_read_burst, w5500_spi_write_burst);

    /* 给每个 socket 分配相同大小的 TX/RX 缓冲，当前只用 socket 0，保留其他 socket 扩展空间。 */
    for (i = 0U; i < _WIZCHIP_SOCK_NUM_; ++i) {
        tx_size[i] = W5500_SOCKET_BUFFER_KB;
        rx_size[i] = W5500_SOCKET_BUFFER_KB;
    }

    if (wizchip_init(tx_size, rx_size) != 0) {
        g_w5500_ready = 0U;
        (void)app_log_printf("[W5500] init failed\r\n");
        return -1;
    }

    /* VERSIONR 正常应为 0x04，如果读不到，通常是 SPI 接线、片选或供电有问题。 */
    if (getVERSIONR() != 0x04U) {
        g_w5500_ready = 0U;
        (void)app_log_printf("[W5500] version error: 0x%02X\r\n", (unsigned int)getVERSIONR());
        return -1;
    }

    w5500_apply_phy_config();
    w5500_apply_static_netinfo();
    g_w5500_ready = 1U;
    (void)app_log_printf("[W5500] ready ip=%u.%u.%u.%u port=5000\r\n",
                         W5500_NETINFO_IP0,
                         W5500_NETINFO_IP1,
                         W5500_NETINFO_IP2,
                         W5500_NETINFO_IP3);
    return 0;
}

int w5500_port_is_link_up(void)
{
    if (g_w5500_ready == 0U) {
        return 0;
    }

    return (wizphy_getphylink() == PHY_LINK_ON) ? 1 : 0;
}

int w5500_port_is_ready(void)
{
    return (g_w5500_ready != 0U) ? 1 : 0;
}

uint8_t w5500_port_get_version(void)
{
    return getVERSIONR();
}
