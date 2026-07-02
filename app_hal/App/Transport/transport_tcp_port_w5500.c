#include "transport_tcp_port.h"

#include "app_log.h"
#include "upgrade_protocol.h"
#include "w5500.h"
#include "w5500_port.h"
#include "socket.h"

/*
 * W5500 TCP 升级通道说明：
 * - 本文件只负责“网络收发”和“按升级协议拼完整帧”。
 * - 协议解析、CRC 校验、写 Flash、保存 pending 参数仍由 transport_common/ota_manager 统一完成。
 * - 这样 UART、W5500 TCP、Wi-Fi 三种通道可以共用同一套 OTA 状态机。
 */

/* 使用 W5500 socket 0 作为升级服务端。W5500 一共有 0~7 共 8 个硬件 socket。 */
#define W5500_TCP_SOCKET              0U

/* 协议帧头固定 4 字节：cmd(2) + len(2)，先读到它才能知道整帧长度。 */
#define TCP_FRAME_HEADER_SIZE         UPGRADE_FRAME_HEADER_SIZE

/* 本地接收缓存：TCP 是流式传输，可能一次只来半帧，也可能一次来多帧的一部分。 */
static uint8_t g_tcp_rx_cache[UPGRADE_FRAME_MAX_SIZE];

/* 当前缓存中已经累计的字节数。 */
static size_t g_tcp_rx_cached = 0U;

/* TCP 服务监听端口，来自 APP_TCP_UPGRADE_PORT。 */
static uint16_t g_tcp_listen_port = 0U;

/* 标记 socket 是否已经被成功打开，避免每次轮询都 close/open。 */
static uint8_t g_tcp_socket_opened = 0U;
static uint8_t g_tcp_connected_logged = 0U;
static uint8_t g_tcp_link_logged = 0xFFU;

/* 按小端序读取协议帧里的 payload 长度字段。 */
static uint16_t load_payload_len(const uint8_t *buffer)
{
    return (uint16_t)buffer[2] | ((uint16_t)buffer[3] << 8U);
}

/* 清空 TCP 接收缓存，常用于连接断开或发现异常长度后重新同步。 */
static void tcp_clear_rx_cache(void)
{
    g_tcp_rx_cached = 0U;
}

/* 关闭 socket 并清掉本地状态，下一次轮询会重新打开监听。 */
static void tcp_close_socket(void)
{
    (void)close(W5500_TCP_SOCKET);
    g_tcp_socket_opened = 0U;
    g_tcp_connected_logged = 0U;
    tcp_clear_rx_cache();
}

static int tcp_link_is_up(void)
{
    int link_up = w5500_port_is_link_up();
    uint8_t link_state = (link_up != 0) ? 1U : 0U;

    if (g_tcp_link_logged != link_state) {
        g_tcp_link_logged = link_state;
        (void)app_log_printf("[W5500] link %s\r\n", (link_up != 0) ? "up" : "down");
    }

    return link_up;
}

/* 打开 socket 0 并进入 TCP Server listen 状态。 */
static int tcp_open_listen_socket(void)
{
    if (g_tcp_listen_port == 0U) {
        return -1;
    }

    /*
     * 使用 SF_IO_NONBLOCK 后，recv/send 在没有数据或发送缓冲不足时会返回 SOCK_BUSY，
     * FreeRTOS 任务就能按 APP_TRANSPORT_POLL_MS 周期继续轮询，不会被 W5500 库阻塞。
     */
    if (socket(W5500_TCP_SOCKET,
               Sn_MR_TCP,
               g_tcp_listen_port,
               (uint8_t)SF_TCP_NODELAY) != (int8_t)W5500_TCP_SOCKET) {
        g_tcp_socket_opened = 0U;
        return -1;
    }

    if (listen(W5500_TCP_SOCKET) != SOCK_OK) {
        tcp_close_socket();
        return -1;
    }

    g_tcp_socket_opened = 1U;
    g_tcp_connected_logged = 0U;
    tcp_clear_rx_cache();
    (void)app_log_printf("[TCP OTA] listen port=%u\r\n", (unsigned int)g_tcp_listen_port);
    return 0;
}

/* 根据 W5500 socket 状态确保 TCP Server 处于可接收状态。 */
static int tcp_service_socket(void)
{
    uint8_t status;

    if (w5500_port_is_ready() == 0) {
        return -1;
    }

    /* 网线未连接时不打开 socket，避免上位机误以为服务可用。 */
    if (tcp_link_is_up() == 0) {
        if (g_tcp_socket_opened != 0U) {
            tcp_close_socket();
        }
        return -1;
    }

    status = getSn_SR(W5500_TCP_SOCKET);
    switch (status) {
        case SOCK_ESTABLISHED:
            /* 已经有上位机连接，可以收发升级帧。 */
            g_tcp_socket_opened = 1U;
            if (g_tcp_connected_logged == 0U) {
                g_tcp_connected_logged = 1U;
                (void)app_log_printf("[TCP OTA] connected\r\n");
            }
            return 0;

        case SOCK_LISTEN:
            /* 正在监听，暂时还没有上位机连接。 */
            g_tcp_socket_opened = 1U;
            return -1;

        case SOCK_INIT:
            /* socket 已打开但还没 listen，补一次 listen。 */
            if (listen(W5500_TCP_SOCKET) == SOCK_OK) {
                g_tcp_socket_opened = 1U;
            }
            return -1;

        case SOCK_CLOSE_WAIT:
            /* 对端已经发起关闭，收尾后重新进入监听。 */
            tcp_close_socket();
            return -1;

        case SOCK_CLOSED:
        default:
            /* CLOSED 或其他异常状态都重新建监听 socket。 */
            tcp_close_socket();
            return tcp_open_listen_socket();
    }
}

/* 从 W5500 socket RX 缓冲搬运尽可能多的数据到本地缓存。 */
static int tcp_fill_rx_cache(void)
{
    uint16_t rx_available;
    size_t free_space;
    uint16_t read_size;
    int32_t result;

    if (tcp_service_socket() != 0) {
        return -1;
    }

    free_space = sizeof(g_tcp_rx_cache) - g_tcp_rx_cached;
    if (free_space == 0U) {
        /* 缓存满还没有拼出合法帧，说明数据流已经错位，清掉后等待下一帧。 */
        tcp_clear_rx_cache();
        return -1;
    }

    rx_available = getSn_RX_RSR(W5500_TCP_SOCKET);
    if (rx_available == 0U) {
        return -1;
    }

    read_size = rx_available;
    if ((size_t)read_size > free_space) {
        read_size = (uint16_t)free_space;
    }

    result = recv(W5500_TCP_SOCKET, &g_tcp_rx_cache[g_tcp_rx_cached], read_size);
    if (result > 0) {
        g_tcp_rx_cached += (size_t)result;
        return 0;
    }

    if ((result != SOCK_BUSY) && (result != SOCKERR_SOCKSTATUS)) {
        tcp_close_socket();
    }

    return -1;
}

/* 缓存中如果已经有完整升级帧，就拷贝给上层。 */
static int tcp_pop_frame(uint8_t *buffer, size_t capacity, size_t *received)
{
    uint16_t payload_len;
    size_t frame_size;
    size_t remain_size;
    size_t i;

    if ((buffer == 0) || (received == 0) || (capacity < UPGRADE_FRAME_OVERHEAD)) {
        return -1;
    }

    *received = 0U;

    if (g_tcp_rx_cached < TCP_FRAME_HEADER_SIZE) {
        return -1;
    }

    payload_len = load_payload_len(g_tcp_rx_cache);
    if (payload_len > UPGRADE_FRAME_MAX_DATA) {
        /* 长度字段超过协议上限，直接丢弃缓存，避免后续一直被错误数据卡住。 */
        tcp_clear_rx_cache();
        return -1;
    }

    frame_size = UPGRADE_FRAME_OVERHEAD + (size_t)payload_len;
    if (frame_size > capacity) {
        tcp_clear_rx_cache();
        return -1;
    }

    if (g_tcp_rx_cached < frame_size) {
        return -1;
    }

    for (i = 0U; i < frame_size; ++i) {
        buffer[i] = g_tcp_rx_cache[i];
    }
    *received = frame_size;

    /*
     * 如果 TCP 一次收到多帧数据，取走第一帧后把后面的字节前移，
     * 下一轮 transport_tcp_poll 可以继续处理下一帧。
     */
    remain_size = g_tcp_rx_cached - frame_size;
    for (i = 0U; i < remain_size; ++i) {
        g_tcp_rx_cache[i] = g_tcp_rx_cache[frame_size + i];
    }
    g_tcp_rx_cached = remain_size;

    return 0;
}

void transport_tcp_port_init(uint16_t port)
{
    g_tcp_listen_port = port;
    tcp_clear_rx_cache();

    /*
     * W5500 的 SPI2/GPIO 初始化已经在 App main 中完成，这里只做 WIZnet 库回调、
     * socket buffer 和静态 IP 参数初始化。
     */
    if (w5500_port_init() != 0) {
        g_tcp_socket_opened = 0U;
        return;
    }

    (void)tcp_service_socket();
}

int transport_tcp_port_recv_frame(uint8_t *buffer, size_t capacity, size_t *received)
{
    /*
     * 先尝试从已有缓存中取完整帧，再从 W5500 搬运新数据。
     * 这样即使上一轮 TCP 收到了两帧，也不会漏处理第二帧。
     */
    if (tcp_pop_frame(buffer, capacity, received) == 0) {
        return 0;
    }

    (void)tcp_fill_rx_cache();

    return tcp_pop_frame(buffer, capacity, received);
}

int transport_tcp_port_send(const uint8_t *buffer, size_t size)
{
    size_t sent_total = 0U;
    int32_t sent_now;

    if ((buffer == 0) || (size == 0U) || (size > 0xFFFFU)) {
        return -1;
    }

    if (tcp_service_socket() != 0) {
        return -1;
    }

    while (sent_total < size) {
        sent_now = send(W5500_TCP_SOCKET,
                        (uint8_t *)&buffer[sent_total],
                        (uint16_t)(size - sent_total));
        if (sent_now > 0) {
            sent_total += (size_t)sent_now;
            continue;
        }

        if (sent_now != SOCK_BUSY) {
            tcp_close_socket();
        }
        (void)app_log_printf("[TCP OTA] send failed=%ld\r\n", (long)sent_now);
        return -1;
    }

    return 0;
}
