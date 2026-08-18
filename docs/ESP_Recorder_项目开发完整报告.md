# ESP Recorder — ESP32-S3 双路串口数据记录仪 项目开发完整报告

> **项目名称**: esp_recorder_v1.0  
> **主控芯片**: ESP32-S3  
> **ESP-IDF 版本**: v5.4  
> **开发工具**: VS Code + Espressif IDF Extension v2.1.0  
> **工作目录**: `D:\zhuomian\weite\ESP32\esp_recorder_v1.1`  
> **文档日期**: 2026-08-03

---

## 目录

1. [项目概述](#1-项目概述)
2. [功能清单](#2-功能清单)
3. [硬件接线](#3-硬件接线)
4. [软件架构](#4-软件架构)
5. [环境搭建步骤](#5-环境搭建步骤)
6. [编译构建](#6-编译构建)
7. [配置文件说明](#7-配置文件说明)
8. [各模块详细说明](#8-各模块详细说明)
9. [Web 管理界面](#9-web-管理界面)
10. [数据流与工作流程](#10-数据流与工作流程)
11. [烧录与调试](#11-烧录与调试)
12. [已知问题与注意事项](#12-已知问题与注意事项)

---

## 1. 项目概述

### 1.1 项目定位

ESP Recorder 是一个基于 ESP32-S3 的**双路串口数据记录仪**。它能够同时监听两路独立 UART 串口的数据，将所有接收到的原始字节实时保存到 SD 卡，并通过 WiFi 热点提供 Web 管理界面，支持远程查看状态、启停记录、下载文件和实时监控数据流。

### 1.2 核心应用场景

- 工业设备串口通信数据采集与记录
- 嵌入式系统调试时的长时间数据抓取
- 多路传感器数据的集中采集存储
- 需要"黑匣子"功能的串口通信场景

### 1.3 项目目录结构

```
esp_recorder_v1.1/
├── CMakeLists.txt                  # 顶层 CMake（项目入口）
├── sdkconfig                       # ESP-IDF 配置（esp32s3, 各外设参数）
├── sdkconfig.defaults              # 默认 sdkconfig
├── main/
│   ├── CMakeLists.txt              # 编译源文件清单 + 嵌入 index.html
│   ├── idf_component.yml           # 组件依赖声明
│   ├── app_main.c                  # ★ 主入口 — 初始化所有模块
│   ├── index.html                  # ★ Web 前端单页应用（内嵌到固件）
│   ├── uart_driver.c/h            # 双路 UART 驱动（CH1/CH2）
│   ├── sd_card.c/h                 # SD 卡 SPI 驱动 + FATFS 挂载
│   ├── usb_cdc.c/h                 # USB CDC 虚拟串口（TinyUSB）
│   ├── usb_msc.c/h                 # USB MSC 大容量存储（TinyUSB）
│   ├── wifi_config.c/h             # WiFi AP+STA 混合模式管理
│   ├── web_server.c/h              # HTTP REST API 服务（端口 80）
│   ├── data_recorder.c/h           # 双通道数据记录引擎（环形缓冲→落盘）
│   ├── data_router.c/h             # 跨通道数据路由（CH1↔CH2 互转）
│   ├── config_manager.c/h          # JSON 配置文件读写（/sdcard/config.json）
│   ├── console_cmd.c/h             # 串口调试控制台（sd_test 命令）
│   └── sd_speed_test.c/h           # SD 卡读写速度测试
├── components/
│   ├── espressif__esp_tinyusb/     # Espressif TinyUSB 封装层
│   └── espressif__tinyusb/         # 上游 TinyUSB 协议栈
├── artifacts/                      # 构建日志归档
└── .vscode/
    ├── settings.json               # VS Code ESP-IDF 工作区配置
    ├── launch.json                 # 调试启动配置
    └── c_cpp_properties.json       # C/C++ IntelliSense 配置
```

---

## 2. 功能清单

| 编号 | 功能模块 | 描述 | 状态 |
|------|---------|------|------|
| F01 | **双路 UART 采集** | CH1 (UART0, TX43/RX44) + CH2 (UART2, TX17/RX18)，独立初始化，一通道失败不影响另一通道 | ✅ 编译通过 |
| F02 | **SD 卡 FATFS 存储** | SPI 模式 (CS=10, MOSI=38, MISO=40, SCK=39)，FATFS 文件系统，自动创建 REC_CH*.bin | ✅ 编译通过 |
| F03 | **WiFi AP 热点** | 默认 SSID `ESP_Recorder_XXXX`，密码 12345678，IP 192.168.4.1，最多 4 客户端 | ✅ 编译通过 |
| F04 | **WiFi STA 连接** | 支持 WPA2/WPA3/开放网络，自动重连（最多 3 次），断线保持 AP 可用 | ✅ 编译通过 |
| F05 | **Web 管理界面** | 内嵌 index.html，端口 80，单页应用，支持状态查看/配网/记录控制/文件管理/实时数据 | ✅ 编译通过 |
| F06 | **REST API** | 14 个 API 端点（状态/配网/AP配置/文件列表/下载/删除/记录启停/实时流/发送TX/重启等） | ✅ 编译通过 |
| F07 | **实时数据流** | 256 条环形缓冲，Web 端 200ms 轮询增量拉取，HEX 格式显示 RX/TX | ✅ 编译通过 |
| F08 | **数据路由** | CH1 RX → CH2 TX，CH2 RX → CH1 TX，硬件级跨通道转发，丢包统计 | ✅ 编译通过 |
| F09 | **USB 复合设备** | TinyUSB CDC (虚拟串口) + MSC (SD 卡读卡器) 复合设备，VID=303A PID=4002 | ✅ 编译通过 |
| F10 | **USB 热插拔保护** | USB 插入时自动卸载 FATFS 停止记录→交给主机；拔出后重新挂载→恢复记录 | ✅ 编译通过 |
| F11 | **JSON 配置系统** | /sdcard/config.json 持久化存储，含 UART/WiFi/Recorder 三段配置，缺省自动创建 | ✅ 编译通过 |
| F12 | **记录启停控制** | Web API 远程控制，auto_start 选项控制上电自动开始，环形缓冲+阈值/超时双条件落盘 | ✅ 编译通过 |
| F13 | **双通道独立统计** | 每通道独立 RX/TX 字节数、丢字节数、文件打开状态、落盘文件名 | ✅ 编译通过 |
| F14 | **调试控制台** | UART1 (TX1/RX2)，115200 baud，sd_test 命令测试 SD 读写速度 | ✅ 编译通过 |
| F15 | **文件管理** | Web 端列出 REC_*.bin 文件、下载、删除，路径穿越防护 | ✅ 编译通过 |
| F16 | **远程重启** | Web API 触发 esp_restart()，2 秒延迟保证响应发出 | ✅ 编译通过 |

---

## 3. 硬件接线

### 3.1 SD 卡 (SPI 模式)

| SD 卡引脚 | ESP32-S3 GPIO | 说明 |
|-----------|---------------|------|
| CS   | GPIO 10 | 片选 |
| MOSI | GPIO 38 | 主机输出 |
| MISO | GPIO 40 | 主机输入 |
| SCK  | GPIO 39 | 时钟 |
| VCC  | 3.3V | 供电 |
| GND  | GND | 共地 |

SPI 主机: SPI2_HOST，最大频率 20 MHz，最大传输 4000 字节。

### 3.2 用户串口

| 通道 | UART 编号 | TX Pin | RX Pin | 默认波特率 |
|------|----------|--------|--------|-----------|
| CH1  | UART0    | GPIO 43 | GPIO 44 | 115200 |
| CH2  | UART2    | GPIO 17 | GPIO 18 | 115200 |

### 3.3 调试控制台

| 信号 | GPIO | 说明 |
|------|------|------|
| TX   | GPIO 1  | 控制台输出 |
| RX   | GPIO 2  | 控制台输入 |
| 波特率 | 115200 | — |

### 3.4 其他 GPIO

| 功能 | GPIO | 说明 |
|------|------|------|
| UART 接收使能 | GPIO 16 | 输出低电平使能串口接收 |
| USB VBUS 检测 | GPIO 48 | TinyUSB VBUS Monitor |

### 3.5 USB 接口

ESP32-S3 原生 USB OTG 接口，枚举为复合设备：
- **VID**: 303A (Espressif)
- **PID**: 4002
- **CDC**: 虚拟串口 (ESP Recorder CDC)
- **MSC**: SD 卡读卡器 (ESP Recorder MSC)

> **接线提示**: 使用开发板的 USB OTG 口（非 UART 口）连接电脑才能枚举 USB 复合设备。如果使用 USB-TTL 模块连接串口，需交叉 TX/RX、共地，使用 3.3V 电平，不要给 VCC 供电（开发板独立供电）。

---

## 4. 软件架构

### 4.1 模块依赖关系

```
app_main.c (主入口)
├── nvs_flash_init()              # NVS 初始化
├── sd_card_init()                # SD 卡挂载 (SPI → FATFS)
├── usb_cdc_init()                # USB CDC 初始化 (TinyUSB)
├── usb_msc_init()                # USB MSC 回调注册
├── config_manager_load()         # 加载 /sdcard/config.json
├── recorder_init()               # 数据记录引擎初始化 (环形缓冲 + IO/Sync 任务)
├── user_uart_init_channel() ×2   # 双路 UART 独立初始化
├── data_router_init()            # 跨通道路由初始化
├── wifi_config_init()            # WiFi 协议栈初始化
├── wifi_apply_ap()               # 启动 AP 热点
├── web_server_start()            # 启动 HTTP 服务器 (端口 80)
├── wifi_connect_sta()            # STA 连接外部 WiFi (可选)
├── recorder_start()              # 自动开始记录 (可选)
├── xTaskCreate(uart_rx_task) ×2  # 创建两路 UART RX 任务
└── console_start()               # 调试控制台
```

### 4.2 FreeRTOS 任务一览

| 任务名称 | 优先级 | 栈大小 | 功能 |
|---------|--------|--------|------|
| uart_rx_ch1 | 5 | 4096 | CH1 UART 接收→写入 recorder + router |
| uart_rx_ch2 | 5 | 4096 | CH2 UART 接收→写入 recorder + router |
| rec_io | 5 | 4096 | 环形缓冲→SD 卡落盘 (阈值/超时触发) |
| rec_sync | 1 | 3072 | 每秒 fsync 强制同步，3 次失败关闭文件 |
| tinyusb | 5 | 4096 | TinyUSB 协议栈任务 (CPU1 亲和) |

### 4.3 数据流

```
外部设备 TX ──→ CH1 UART (RX44) ──→ uart_rx_ch1 task
                                        ├──→ recorder_write_rx_channel()
                                        │      ├──→ live_push() (实时流环形缓冲)
                                        │      └──→ ringbuf_write() (落盘环形缓冲 16KB)
                                        │             └──→ rec_io task → fwrite(SD卡)
                                        └──→ data_router_forward() → CH2 UART (TX17)

外部设备 TX ──→ CH2 UART (RX18) ──→ uart_rx_ch2 task
                                        ├──→ recorder_write_rx_channel()
                                        │      ├──→ live_push()
                                        │      └──→ ringbuf_write() → fwrite(SD卡)
                                        └──→ data_router_forward() → CH1 UART (TX43)

Web /api/send POST ──→ user_uart_write_channel() → TX 发送 + recorder_write_tx_channel()
                                                         └──→ live_push() (TX 标记)

Web /api/stream GET ──→ recorder_live_fetch(since) → JSON {items, max_seq}
```

---

## 5. 环境搭建步骤

### 5.1 安装 ESP-IDF

1. **安装 ESP-IDF v5.4**（本项目使用的版本）
   - 推荐使用 VS Code Espressif IDF 扩展的安装管理器自动安装
   - 或从 [Espressif 官网](https://docs.espressif.com/projects/esp-idf/) 下载离线安装包
   - 安装路径: `D:\23178\esp-idf`（本项目实际使用路径）

2. **安装 VS Code 扩展**
   - 安装 `espressif.esp-idf-extension` (本项目使用 v2.1.0)
   - 扩展会自动检测已安装的 ESP-IDF 并配置工具链

3. **验证安装**
   ```powershell
   # 在 ESP-IDF 命令提示符中
   idf.py --version
   python --version
   ```

### 5.2 配置 VS Code 工作区

本项目 `.vscode/settings.json` 的关键配置：

```json
{
    "idf.currentSetup": "D:\\23178\\esp-idf",
    "idf.customExtraVars": {
        "IDF_TARGET": "esp32s3"
    },
    "idf.flashType": "UART"
}
```

> **重要**: 不要在工作区配置中预设 `idf.portWin`，让每次烧录时根据实际硬件枚举结果选择正确的 COM 口。

### 5.3 ESP-IDF 安装注册表

VS Code 扩展通过 `C:\Users\23178\.espressif\esp_idf.json` 识别已安装的工具链：

```json
{
    "idfSelectedId": "esp-idf-v5.4",
    "setups": [{
        "id": "esp-idf-v5.4",
        "idfPath": "D:\\23178\\esp-idf",
        "python": "C:\\Users\\23178\\.espressif\\python_env\\idf5.4_py3.8_env\\Scripts\\python.exe",
        ...
    }]
}
```

### 5.4 克隆/打开项目

```powershell
# 项目已在本地
cd D:\zhuomian\weite\ESP32\esp_recorder_v1.1

# 用 VS Code 打开
code .
```

---

## 6. 编译构建

### 6.1 构建命令

```powershell
# 在 ESP-IDF 终端中（已 source export.ps1）
cd D:\zhuomian\weite\ESP32\esp_recorder_v1.1

# 完整构建
idf.py build

# 保存构建日志
idf.py build 2>&1 | Tee-Object -FilePath artifacts\idf_build.txt

# 清理后重新构建
idf.py fullclean
idf.py build
```

### 6.2 构建产物

成功构建后，产物位于 `build/` 目录：

| 文件 | 说明 |
|------|------|
| `build/esp_recorder_v1.0.bin` | 主应用程序固件 |
| `build/bootloader/bootloader.bin` | 启动引导程序 |
| `build/partition_table/partition-table.bin` | 分区表 |
| `build/ota_data_initial.bin` | OTA 初始数据 |
| `build/flash_args` | 烧录参数清单 |
| `build/*.elf` | ELF 调试文件 |
| `build/*.map` | 链接映射文件 |

### 6.3 烧录到 ESP32-S3

```powershell
# 确认 COM 口后执行（替换 COMx 为实际串口号）
idf.py -p COMx flash

# 查看串口输出
idf.py -p COMx monitor
```

**Flash 布局**:

| 偏移 | 分区 | 文件 |
|------|------|------|
| 0x0 | bootloader | bootloader/bootloader.bin |
| 0x8000 | partition_table | partition-table.bin |
| 0xf000 | ota_data | ota_data_initial.bin |
| 0x20000 | factory | esp_recorder_v1.0.bin |

Flash 参数: dio 模式, 80MHz, 16MB Flash 容量

---

## 7. 配置文件说明

### 7.1 config.json 位置与格式

路径: `/sdcard/config.json`（SD 卡根目录）

首次开机如果 SD 卡上没有此文件，系统会自动创建默认配置。

### 7.2 完整配置结构

```json
{
    "uart": {
        "baudrate": 115200,
        "databits": 8,
        "stopbits": 1,
        "parity": 0
    },
    "wifi": {
        "enable_ap": 1,
        "ap_ssid": "",
        "ap_pass": "12345678",
        "ap_ip": "192.168.4.1",
        "enable_sta": 0,
        "sta_ssid": "",
        "sta_pass": "",
        "sta_authmode": 0
    },
    "recorder": {
        "auto_start": 1,
        "stream_gap_ms": 100
    }
}
```

### 7.3 配置项说明

| 段 | 字段 | 默认值 | 说明 |
|----|------|--------|------|
| uart | baudrate | 115200 | 两路 UART 共用波特率 |
| uart | databits | 8 | 数据位（当前固定 8） |
| uart | stopbits | 1 | 停止位（当前固定 1） |
| uart | parity | 0 | 校验位（0=无, 1=奇, 2=偶） |
| wifi | enable_ap | 1 | 是否启用 AP 热点 |
| wifi | ap_ssid | "" | AP 名称（空=自动生成 ESP_Recorder_XXXX） |
| wifi | ap_pass | "12345678" | AP 密码（空=开放热点） |
| wifi | ap_ip | "192.168.4.1" | AP 静态 IP |
| wifi | enable_sta | 0 | 是否启用 STA 连接外部 WiFi |
| wifi | sta_ssid | "" | 外部 WiFi SSID |
| wifi | sta_pass | "" | 外部 WiFi 密码 |
| wifi | sta_authmode | 0 | 加密方式 (0=开放, 3=WPA2, 6=WPA3, 7=WPA2/WPA3) |
| recorder | auto_start | 1 | 上电自动开始记录 |
| recorder | stream_gap_ms | 100 | 实时流超过此毫秒数显示"等待数据..." |

---

## 8. 各模块详细说明

### 8.1 app_main.c — 主入口

**职责**: 按顺序初始化所有子系统，创建 FreeRTOS 任务，进入主循环。

**初始化顺序**:
1. `gpio_config(GPIO_NUM_16)` — 拉低 GPIO16 使能串口接收
2. `nvs_flash_init()` — 初始化 NVS（WiFi 需要）
3. `sd_card_init()` — 挂载 SD 卡 FATFS
4. `usb_cdc_init()` + `usb_msc_init()` — 初始化 USB 复合设备
5. `config_manager_load()` — 加载配置（缺失则创建默认）
6. `recorder_init()` — 创建记录引擎任务
7. `user_uart_init_channel()` ×2 — 独立初始化两路 UART
8. `data_router_init()` — 初始化跨通道路由
9. `wifi_config_init()` + `wifi_apply_ap()` — 启动 WiFi AP
10. `web_server_start()` — 启动 HTTP 服务器
11. `wifi_connect_sta()` — 可选 STA 连接
12. `recorder_start()` — 可选自动记录
13. `xTaskCreate(uart_rx_task)` ×2 — 创建 RX 任务
14. `console_start()` — 调试控制台

**USB 热插拔回调**:
- `tud_mount_cb()`: USB 主机连接时 → 停止记录 → 卸载 FATFS → 把 SD 卡交给 USB MSC
- `tud_umount_cb()`: USB 断开时 → 重新挂载 FATFS → 恢复记录

### 8.2 uart_driver.c — 双路 UART 驱动

**功能**:
- 两路独立 UART (UART0 CH1, UART2 CH2)
- 支持独立初始化（一通道失败不影响另一通道）
- 支持运行时修改波特率
- RX/TX 字节数统计（线程安全）
- TX 回调机制（用于实时流显示已发送数据）

**关键数据**:
- 每路 UART 缓冲区 2048 字节
- 使用 `portMUX_TYPE` 自旋锁保护计数器
- 默认波特率 115200，8N1 配置
- CH1: UART0, TX=GPIO43, RX=GPIO44
- CH2: UART2, TX=GPIO17, RX=GPIO18

### 8.3 sd_card.c — SD 卡驱动

**功能**:
- SPI 模式访问 SD 卡 (SPI2_HOST, 20MHz)
- FATFS 文件系统挂载到 `/sdcard`
- 支持安全卸载/重新挂载（配合 USB MSC 使用）
- `sd_card_unmount_fs()`: 卸载 FATFS 但保留块设备（交给 USB MSC）
- `sd_card_remount_fs()`: USB 断开后重新挂载 FATFS

**SD 卡检测状态**:
```c
typedef struct {
    bool mounted;
    char mount_point[16];
} sd_card_state_t;
```

### 8.4 usb_cdc.c + usb_msc.c — USB 复合设备

**CDC (虚拟串口)**:
- TinyUSB CDC-ACM，可用于调试或作为配网备用通道
- RX 回调注册机制，收到数据后调用注册的回调函数
- 支持 write/read 操作

**MSC (大容量存储)**:
- 将 SD 卡暴露为 USB 读卡器
- 实现完整的 SCSI READ10/WRITE10 命令
- 3 次重试机制，提高可靠性
- 自动获取 SD 卡容量和扇区大小

**USB 描述符配置**:
- 配置描述符: CDC 控制接口 + CDC 数据接口 + MSC 接口
- 支持远程唤醒
- 自供电模式
- VBUS 检测 GPIO48

### 8.5 wifi_config.c — WiFi 管理

**功能**:
- AP+STA 混合模式 (WIFI_MODE_APSTA)
- AP: 默认 SSID `ESP_Recorder_XXXX` (XXXX=MAC 后两位)，密码 `12345678`
- AP IP: 192.168.4.1/24，内置 DHCP 服务器
- STA: 支持 WPA2/WPA3/开放网络
- STA 自动重连: 最多 3 次重试，间隔 5 秒
- 运行时修改 AP 参数（SSID/密码/IP）立即生效
- 运行时修改 STA 凭证并重连

**关键函数**:
- `wifi_apply_ap(ssid, pass, ip)`: 应用并重启 AP
- `wifi_connect_sta(ssid, pass, authmode)`: 连接外部 WiFi
- `wifi_disconnect_sta()`: 断开 STA

### 8.6 data_recorder.c — 数据记录引擎

**功能**:
- 双通道独立记录，每通道一个 `.bin` 文件
- 文件名格式: `REC_CH1_YYYYMMDD_HHMMSS.bin`
- 每通道独立 16KB 环形缓冲区
- 双条件触发落盘: 缓冲 ≥ 2048 字节 OR 1 秒超时
- 每秒 fsync 强制同步（3 次失败则关闭文件标记故障）
- 实时流环形缓冲区 (256 条, 每条最多 160 字节)
- 双通道独立统计: RX 总字节、丢字节数

**文件名生成规则**:
```
/sdcard/REC_CH1_20260727_193428.bin       ← 首个文件
/sdcard/REC_CH1_20260727_193428_1.bin     ← 同名时加后缀
```

**实时流数据格式**:
```
2026-07-27T19:34:28.123,CH1,RX,01 03 00 00 00 0A C5 CD
2026-07-27T19:34:28.234,CH1,TX,01 03 02 00 00 B8 44
```

### 8.7 data_router.c — 跨通道数据路由

**功能**:
- CH1 收到的数据自动转发到 CH2 的 TX
- CH2 收到的数据自动转发到 CH1 的 TX
- 路由启停控制 (默认启用)
- 双向转发字节数和丢包数统计

**使用场景**: 两个外部设备分别接在 CH1 和 CH2，通过路由实现它们之间的透明通信，同时全程记录和监控。

### 8.8 web_server.c — HTTP 服务器

**技术栈**: esp_http_server (ESP-IDF 原生 HTTP 服务器组件)

**注册的 API 端点 (14 个)**:

| 方法 | URI | 功能 |
|------|-----|------|
| GET | `/` | 返回内嵌的 index.html |
| GET | `/api/status` | 获取设备完整状态 JSON |
| POST | `/api/wifi` | 设置 STA WiFi (SSID/密码/加密) |
| POST | `/api/ap` | 设置 AP 参数 (SSID/密码/IP) |
| GET | `/api/files` | 获取 REC_*.bin 文件列表 |
| GET | `/api/file?name=` | 下载指定文件 |
| POST | `/api/file/delete?name=` | 删除指定文件 |
| POST | `/api/recorder/start` | 开始记录 |
| POST | `/api/recorder/stop` | 停止记录 |
| GET | `/api/stream?since=N` | 拉取实时数据流 (增量) |
| POST | `/api/stream/clear` | 清空实时流缓冲 |
| POST | `/api/send` | 发送 hex 数据到指定通道 TX |
| POST | `/api/config/stream_gap` | 设置实时流间隔阈值 |
| POST | `/api/reboot` | 重启设备 |

**安全特性**:
- 路径穿越防护 (`is_safe_filename()`)
- IPv4 格式校验
- JSON 解析错误不崩溃
- 线程安全的配置读写

### 8.9 config_manager.c — 配置管理器

**功能**:
- 从 `/sdcard/config.json` 加载 JSON 配置
- 缺失字段使用硬编码默认值
- 配置写入后立即持久化
- authmode 合法性校验并 clamp
- stream_gap_ms 限制在 [1, 1000] 范围
- 写盘失败时回滚到旧值

### 8.10 console_cmd.c — 调试控制台

**串口参数**: UART1, TX=GPIO1, RX=GPIO2, 115200 baud

**内置命令**:
```bash
esp> sd_test                    # 标准测试 (1/4/16/64 KB 四档)
esp> sd_test 4096 1048576       # 自定义测试 (4KB 块, 1MB 总量)
```

### 8.11 index.html — Web 前端

**技术栈**: 纯 HTML + CSS + Vanilla JavaScript，零依赖

**功能区域**:
1. **设备状态面板**: AP/STA 状态、SD 卡、记录状态、字节统计、UART 状态、路由统计
2. **AP 热点配置**: SSID/密码/IP 设置
3. **STA 配网**: SSID/加密方式/密码，支持 WPA2/WPA3/开放
4. **实时数据监控**: HEX 数据流显示，发送通道选择，自动滚动，"等待数据..." 间隔注入
5. **设备控制**: 开始/停止记录、重启设备
6. **文件管理**: 列出 REC_*.bin、下载、删除

**轮询机制**:
- 状态刷新: 2 秒间隔
- 实时流: 200ms 间隔增量拉取 (since_seq 机制)
- 自动限制最多 500 条显示行

---

## 9. Web 管理界面

### 9.1 访问方式

1. ESP32-S3 上电后自动启动 AP 热点
2. 手机/电脑搜索 WiFi: `ESP_Recorder_XXXX`（XXXX 为设备 MAC 后两位）
3. 连接密码: `12345678`（默认值，可在 Web 页面修改）
4. 浏览器访问: `http://192.168.4.1`

### 9.2 界面截图位置

> **请在此处插入功能演示图片**

| 截图编号 | 内容 | 建议截图要点 |
|---------|------|------------|
| 图 1 | 设备状态面板 | 显示 AP 已开启、SD 已挂载、记录中的状态 |
| 图 2 | Web 首页全貌 | 展示完整页面，含所有功能区域 |
| 图 3 | AP 配置区 | 修改 SSID/密码/IP 的界面 |
| 图 4 | STA 配网区 | 选择 WiFi 并输入密码的界面 |
| 图 5 | 实时数据监控 | HEX 数据流动态滚动显示 |
| 图 6 | 文件管理区 | REC_CH1_*.bin 和 REC_CH2_*.bin 文件列表 |
| 图 7 | 设备控制按钮 | 开始/停止记录、重启按钮 |

---

## 10. 数据流与工作流程

### 10.1 典型使用流程

```
┌──────────────────────────────────────────────────────────────┐
│  1. 上电                                                      │
│     ├── ESP32-S3 初始化所有外设                                │
│     ├── 挂载 SD 卡 FATFS                                      │
│     ├── 加载 /sdcard/config.json（不存在则创建默认）             │
│     ├── 启动 WiFi AP 热点                                     │
│     └── 可选 STA 连接外部 WiFi                                │
│                         ↓                                     │
│  2. 开始记录 (auto_start=1 时自动开始)                         │
│     ├── 创建 REC_CH1_YYYYMMDD_HHMMSS.bin                      │
│     ├── 创建 REC_CH2_YYYYMMDD_HHMMSS.bin                      │
│     └── 两路 UART 开始接收数据                                 │
│                         ↓                                     │
│  3. 数据采集循环                                               │
│     ├── CH1 RX → recorder 落盘 + router → CH2 TX              │
│     ├── CH2 RX → recorder 落盘 + router → CH1 TX              │
│     ├── Web 实时流 200ms 增量推送                              │
│     └── 每秒 fsync 保证数据安全                                │
│                         ↓                                     │
│  4. 停止记录 (手动或 USB 连接时自动)                            │
│     ├── 刷新剩余缓冲数据到 SD 卡                               │
│     ├── fsync + fclose 关闭文件                                │
│     └── 可通过 Web 下载或 USB 读卡器获取文件                    │
└──────────────────────────────────────────────────────────────┘
```

### 10.2 USB 连接时的行为

```
USB 插入:
  1. tud_mount_cb() 触发
  2. recorder_stop() → 刷新缓冲 → 关闭文件
  3. sd_card_unmount_fs() → 卸载 FATFS
  4. SD 卡完全交给 USB MSC (电脑显示为 U 盘)
  5. 用户可在电脑上直接读写 SD 卡文件

USB 拔出:
  1. tud_umount_cb() 触发
  2. sd_card_remount_fs() → 重新初始化 SPI → 挂载 FATFS
  3. 如果 config.recorder.auto_start=1 → recorder_start()
  4. 恢复 UART 数据记录
```

---

## 11. 烧录与调试

### 11.1 烧录前检查

1. **确认 COM 口**: 使用设备管理器确认 ESP32-S3 的串口号
   - Espressif 原生 USB: VID=303A
   - CH340 芯片: VID=1A86
   - CP210x 芯片: VID=10C4
   - **排除蓝牙串口** (蓝牙链接上的标准串行)

2. **确认芯片型号**: `sdkconfig` 中 `CONFIG_IDF_TARGET="esp32s3"`

3. **确认 Flash 容量**: `sdkconfig` 中 Flash 大小 (本项目 16MB)

### 11.2 烧录命令

```powershell
# 仅烧录（不擦除）
idf.py -p COMx flash

# 烧录并打开串口监视器
idf.py -p COMx flash monitor

# 擦除整个 Flash 后烧录
idf.py -p COMx erase-flash flash
```

### 11.3 调试技巧

**串口控制台** (UART1, TX1/RX2, 115200):
```bash
esp> sd_test              # SD 卡读写速度测试
esp> sd_test 4096 1048576 # 自定义测试
```

**查看日志**: 启动时串口输出标签包括:
- `main` — 主流程
- `sd_card` — SD 卡操作
- `usb_cdc` / `usb_msc` — USB 状态
- `wifi_config` — WiFi 连接
- `web_server` — HTTP 请求
- `recorder` — 记录状态
- `uart_driver` — UART 操作
- `config` — 配置加载/保存

### 11.4 常见问题

| 现象 | 原因 | 解决方法 |
|------|------|---------|
| `A serial exception error: Write timeout` | COM 口是蓝牙虚拟串口，不是 ESP32 | 用设备管理器确认 ESP32 的真实 COM 口 |
| SD 卡 "not mounted" | SD 卡未插入或接触不良 | 检查 SD 卡座焊接和 SPI 接线 |
| WiFi AP 看不到 | GPIO48 VBUS 检测异常 | 检查 USB 连接或 sdkconfig 配置 |
| config.json 不生效 | 文件格式错误或路径问题 | 删除后让系统自动重新生成默认配置 |
| 记录文件 0 字节 | 串口没有收到数据 | 检查外部设备接线，确认 TX→RX 交叉 |

---

## 12. 已知问题与注意事项

### 12.1 已验证通过

- ✅ ESP-IDF v5.4 环境完整配置
- ✅ `idf.py build` 编译成功（esp32s3 目标）
- ✅ 固件 `esp_recorder_v1.0.bin` 正常生成 (0xfa000 bytes, 76% free)
- ✅ VS Code 工作区配置正确
- ✅ 所有 16 个功能模块代码完整

### 12.2 待硬件验证

- ⚠️ CH1 UART (TX43/RX44) 实际收发功能 — 需连接外部设备测试
- ⚠️ CH2 UART (TX17/RX18) 实际收发功能 — 需连接外部设备测试
- ⚠️ SD 卡 SPI 模式读写 — 需插入 SD 卡验证
- ⚠️ WiFi AP 手机连接 — 需上电测试
- ⚠️ USB CDC/MSC 复合设备枚举 — 需连接电脑 USB OTG 口验证
- ⚠️ 长时间记录稳定性 — 需跑几个小时的压力测试
- ⚠️ USB 热插拔 FATFS/MSC 切换 — 需实际插拔测试

### 12.3 已知限制

| 限制 | 说明 |
|------|------|
| 串口波特率固定 | 当前两路共用同一波特率，不支持独立设置 |
| Web 不支持 HTTPS | 仅 HTTP 明文，不建议在公网环境使用 |
| 文件格式为 raw .bin | 不是 CSV/文本格式，需自行解析 |
| 无数据压缩 | 长时间记录会占用较大 SD 空间 |
| 串口参数有限 | 仅支持 8N1 固定格式 |
| 无 MQTT/云平台 | 当前仅 WiFi 局域网内访问 |

### 12.4 烧录提醒

> **⚠️ 重要**: 烧录前务必确认：
> 1. COM 口是 ESP32-S3 的真实串口（不是蓝牙虚拟串口）
> 2. 芯片型号是 ESP32-S3（不是 ESP32 或其他变体）
> 3. Flash 容量匹配（16MB）
> 4. 不要执行 `erase-flash`，除非需要完全清除

---

## 附录 A: 源文件清单

| 文件 | 行数(约) | 功能说明 |
|------|---------|---------|
| `main/app_main.c` | 183 | 主入口，系统初始化 |
| `main/web_server.c` | 649 | HTTP 服务器 + 14 个 API 端点 |
| `main/data_recorder.c` | 614 | 双通道记录引擎 |
| `main/wifi_config.c` | 253 | WiFi AP+STA 管理 |
| `main/config_manager.c` | 278 | JSON 配置读写 |
| `main/uart_driver.c` | 221 | 双路 UART 驱动 |
| `main/sd_card.c` | 171 | SD 卡 SPI 驱动 |
| `main/usb_cdc.c` | 150 | USB CDC 虚拟串口 |
| `main/usb_msc.c` | 189 | USB MSC 读卡器 |
| `main/data_router.c` | 79 | 跨通道数据路由 |
| `main/console_cmd.c` | 91 | 调试控制台 |
| `main/sd_speed_test.c` | — | SD 读写速度测试 |
| `main/index.html` | 309 | Web 前端 SPA |

---

## 附录 B: VS Code 环境恢复步骤

如果在新的电脑上继续开发，按以下步骤恢复环境：

```powershell
# 1. 确认 ESP-IDF 安装
idf.py --version
# 应输出: ESP-IDF v5.4

# 2. 确认 Python 环境
python --version
# 应输出: Python 3.8.x

# 3. 确认 VS Code 扩展
code --list-extensions --show-versions | Select-String espressif
# 应输出: espressif.esp-idf-extension@2.1.0

# 4. 确认 esp_idf.json 注册表
Get-Content C:\Users\23178\.espressif\esp_idf.json

# 5. 打开项目
code D:\zhuomian\weite\ESP32\esp_recorder_v1.1

# 6. 编译验证
idf.py build
```

---

## 附录 C: 关键设计决策

| 决策 | 理由 |
|------|------|
| SD 卡走 SPI 而非 SDMMC | 释放 SDMMC 引脚给其他外设，SPI 模式足够满足记录吞吐量 |
| 配置放 SD 卡而非 NVS | 用户可直接在电脑上编辑 config.json，无需专用工具 |
| AP+STA 混合模式 | 保证本地 Web 管理永远可用，同时可连接外网 |
| 双通道独立初始化 | 某通道硬件故障不影响另一通道正常工作 |
| TinyUSB 复合设备 | 虚拟串口调试 + 读卡器取文件，一根 USB 线解决 |
| Web 使用 cJSON 拼装 | 避免 snprintf 拼接 JSON 时 SSID 含特殊字符导致损坏 |
| 内嵌 HTML (EMBED_FILES) | 固件自包含，不依赖 SD 卡上的 Web 文件 |
| 环形缓冲 + 阈值/超时双条件落盘 | 减少 SD 卡写入次数，又保证数据不丢失 |

---

> **文档版本**: v1.0  
> **最后更新**: 2026-08-03  
> **作者**: 尤译庆  
> **项目路径**: `D:\zhuomian\weite\ESP32\esp_recorder_v1.1`
