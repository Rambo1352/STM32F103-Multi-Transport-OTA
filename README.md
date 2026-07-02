# STM32F103-Multi-Transport-OTA

基于 STM32F103ZET6 的 Bootloader + App 在线升级项目，支持 UART、W5500 Ethernet TCP、ESP32 Wi-Fi TCP 三种升级方式。三种传输通道共用同一套升级协议和 OTA 状态机，新固件先暂存到 W25Q32 外部 Flash，再由 bootloader 搬运到 STM32 内部 Flash 的 App 区。

## 项目结构

```text
result/
├── bootloader_hal/          # Bootloader 工程，负责检查升级标志、搬运新 App、跳转 App/恢复程序
├── bootloader_reset_hal/    # 恢复/出厂程序，升级失败时由 bootloader 跳转到这里
├── app_hal/                 # 主 App 工程，运行 FreeRTOS，接收 UART/ETH/WiFi 升级数据
└── pc_host/                 # PC 上位机 Python 工具，负责发送 bin 文件
```

## Flash 分区

STM32 内部 Flash：

| 区域 | 地址 | 说明 |
| --- | --- | --- |
| Bootloader | `0x08000000` | 上电后首先运行，检查是否需要升级 |
| Reset/Factory App | `0x08004000` | 升级失败时回退运行 |
| Main App | `0x08008000` | 正常业务程序和 OTA 接收程序 |

W25Q32 外部 Flash：

| 地址 | 说明 |
| --- | --- |
| `0x00000000` | metadata，保存新固件在 W25Q32 中的起始地址和大小 |
| `0x00001000` | 新固件 bin 数据暂存区 |

`0x00001000` 正好是一个 4KB 扇区边界，metadata 和固件本体分开存放，避免固件覆盖升级描述信息。

24C02 EEPROM：

| 地址 | 内容 |
| --- | --- |
| `0x10` | boot 状态：`BOOT_UPDATE` / `BOOT_NO_UPDATE` / `BOOT_RESET` |
| `0x11 ~ 0x12` | 校验 key：`0x5A6B` |

## 升级协议

三种升级方式共用同一套帧协议：

```text
cmd(2 bytes) + len(2 bytes) + payload + reserve(4 bytes) + crc16(2 bytes)
```

字段采用小端格式。

| 命令 | 值 | 说明 |
| --- | --- | --- |
| `QUERY_INFO` | `0x0001` | 查询升级状态 |
| `ENTER` | `0x0002` | 进入升级流程 |
| `BEGIN` | `0x0003` | 发送固件大小、CRC32、版本号 |
| `DATA` | `0x0004` | 分片发送固件数据，带 offset 和 size |
| `END` | `0x0005` | 结束传输，App 从 W25Q32 读回整包并校验 CRC32 |
| `RESET_MCU` | `0x0006` | 请求 MCU 复位，让 bootloader 执行搬运 |
| `ACK` | `0x8000` | 下位机回复 |

校验分两层：

- `CRC16`：每一帧校验，保证单帧数据正确。
- `CRC32`：整包固件校验，保证完整 bin 文件正确。

## 三种升级方式

### UART 升级

- 使用 USART1。
- 默认波特率：`9600`。
- PC 通过 `pc_host/serial_upgrade.py` 发送升级协议帧。

### Ethernet 升级

- 使用 W5500。
- 设备 IP：`192.168.1.88`。
- TCP 端口：`5000`。
- PC 通过 `pc_host/tcp_upgrade.py` 连接并发送升级协议帧。

### Wi-Fi 升级

- 使用 ESP32 AT 固件作为 Wi-Fi/TCP 透传模块。
- STM32 通过 USART2 控制 ESP32。
- ESP32 AP SSID：`dengziqi`。
- AP IP：`192.168.36.1`。
- TCP 端口：`5001`。
- PC 连接 ESP32 热点后，通过 `pc_host/tcp_upgrade.py` 发送升级协议帧。

ESP32 只负责网络接入和 TCP 数据转串口，真正的协议解析、CRC 校验、W25Q32 写入、24C02 升级标志写入都在 STM32 App 侧完成。

## 完整升级流程

```text
PC 发送 ENTER
App 回复 ACK

PC 发送 BEGIN(size, crc32, version)
App 擦除 W25Q32 暂存区
App 保存本次升级上下文
App 回复 ACK

PC 分片发送 DATA(offset, size, data)
App 按 offset 写入 W25Q32 0x00001000 后的固件暂存区
App 回复 ACK

PC 发送 END
App 从 W25Q32 读回整包计算 CRC32
校验成功后写 W25Q32 metadata
App 写 24C02 升级标志 BOOT_UPDATE
App 回复 ACK

PC 发送 RESET_MCU
App 复位 MCU

Bootloader 启动
Bootloader 读取 24C02
发现 BOOT_UPDATE
Bootloader 读取 W25Q32 metadata
Bootloader 校验新固件向量表
Bootloader 擦除 STM32 内部 Flash App 区 0x08008000
Bootloader 将 W25Q32 新固件搬运到内部 Flash
成功后写 BOOT_NO_UPDATE
跳转到新 App
```

如果 bootloader 搬运失败，会写入 `BOOT_RESET`，下次启动跳转到 `0x08004000` 的恢复程序。

## PC 上位机用法

UART：

```powershell
python pc_host\serial_upgrade.py app_hal\MDK-ARM\app_hal\app_hal.bin --port COMx --baud 9600
```

Ethernet：

```powershell
python pc_host\tcp_upgrade.py app_hal\MDK-ARM\app_hal\app_hal.bin --host 192.168.1.88 --port 5000
```

Wi-Fi：

```powershell
python pc_host\tcp_upgrade.py app_hal\MDK-ARM\app_hal\app_hal.bin --host 192.168.36.1 --port 5001 --timeout 30 --packet-size 128 --frame-gap-ms 100
```

## 关键调试问题

项目调试过程中解决过以下问题：

1. UART 升级不更新  
   统一 PC 和 STM32 的串口参数，最终使用 `9600` 波特率，并检查协议帧、ACK、W25Q32 写入和 24C02 升级标志。

2. Python 上位机模块导入失败  
   修复 `pc_host` 脚本路径，避免出现 `ModuleNotFoundError: No module named 'protocol_frame'`。

3. W5500 读取版本号为 `0xFF`  
   定位为 SPI/CS/RST/连线等硬件通信问题，修正后能够正常打印 W5500 ready 和 TCP listen。

4. Ethernet link down  
   区分驱动初始化和物理链路状态，确认网线、交换机/路由器、PC 网卡同网段配置。

5. ESP32 AT 不响应  
   检查 USART2 TX/RX、ESP32 EN 引脚、AT 固件波特率，最终默认保持 EN high，避免频繁复位 ESP32。

6. Wi-Fi 端口 ping 通但 TCP 连接失败  
   通过 `AT+CIPSERVER?` 判断 TCP Server 实际状态，兼容 `AT+CIPSERVER=1,5001` 返回超时但服务已经启动的情况。

7. Wi-Fi ACK 超时  
   ESP32 `AT+CIPSEND` 响应中可能混入下一帧 `+IPD` 数据，导致原代码误吞下一帧。后续改为 USART2 中断 ring buffer 接收，并在 CIPSEND 阶段保护/恢复 `+IPD` 数据。

8. 传输成功后 bootloader 回退到恢复版本  
   定位到 bootloader 搬运阶段，修正 bootloader 侧 W25Q32 驱动：SPI 读字节使用 `HAL_SPI_TransmitReceive` 发送 dummy byte，busy 轮询按命令周期释放 CS，扇区地址按 24 位地址发送。

9. 日志太多导致流水灯看起来不跑  
   因调试串口为 `9600`，大量日志会阻塞任务。运行期日志队列满时改为丢弃日志，并提高 LED 任务优先级，保证系统状态可观察。

## 面试描述重点

这个项目的核心设计是“传输层和升级状态机解耦”：

- UART、W5500 TCP、ESP32 Wi-Fi 都只是 transport。
- 升级协议、ACK、CRC、W25Q32 暂存、24C02 标志、bootloader 搬运逻辑完全复用。
- 后续如果新增 4G、CAN、BLE 等升级方式，只需要实现新的收发帧接口，不需要重写 OTA 状态机。

