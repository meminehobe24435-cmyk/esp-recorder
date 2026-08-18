# ESP32-S3 WiFi 数据记录仪第一阶段接手审计

审计日期：2026-07-27  
项目目录：`D:\zhuomian\weite\ESP32\esp_recorder_v1.1`  
审计范围：现有代码、实际构建、功能调用链和硬件实测边界  
限制遵守情况：未修改业务代码，未执行 `idf.py set-target`，未擦除 Flash，未烧录开发板。

## 1. 仓库基本信息

- 项目不是 Git 仓库。`git status`、`git branch --show-current`、`git log -5 --oneline` 均返回 `fatal: not a git repository`。
- 当前项目没有 Git 版本管理，接手修改存在回退风险。
- 顶层没有 README、需求文档、`managed_components`、`docs`（审计前）或项目自有自动化测试脚本。
- 存在 `CMakeLists.txt`、`main`、`components`、`sdkconfig`、`sdkconfig.defaults`、`partitions.csv`、`web`。
- `components` 内含本地 vendored `espressif__esp_tinyusb` 和 `espressif__tinyusb`。
- `.vscode/settings.json` 配置了 `COM25`，但审计时系统仅枚举到 `COM4`、`COM6`、`COM7`、`COM90`，不能确认哪一个连接 ESP32-S3。
- `.vscode/settings.json` 的 `idf.currentSetup` 指向不存在的 `C:\Espressif\5.5.4\...`，`c_cpp_properties.json` 的编译器指向不存在的 `E:\Espressif\...`。实际可用环境位于 `D:\23178\esp-idf` 和 `C:\Users\23178\.espressif`。

## 2. ESP-IDF、目标与构建结果

- 实际 ESP-IDF：`ESP-IDF v5.4`，`idf` 组件版本 `5.4.0`。
- Python：系统 `Python 3.8.0`；构建使用 `C:\Users\23178\.espressif\python_env\idf5.4_py3.8_env\Scripts\python.exe`。
- `sdkconfig` 当前目标：`CONFIG_IDF_TARGET="esp32s3"`，符合预期，因此未执行 `set-target`。
- 实际执行：用现有 IDF Python 环境调用 `D:\23178\esp-idf\tools\idf.py build`。
- 构建结果：成功，生成 `build\esp_recorder_v1.0.bin` 和 `build\esp_recorder_v1.0.elf`。
- 固件大小：`0xfbb40` 字节（1,030,976 字节）；当前实际最小 App 分区为 `0x100000`，仅剩 `0x44c0` 字节（约 2%）。
- 高风险配置矛盾：项目提供的 `partitions.csv` 是大容量 OTA 布局，但 `sdkconfig` 中 `CONFIG_PARTITION_TABLE_CUSTOM` 未启用，实际使用 `partitions_singleapp.csv`；同时 Flash 配置为 2 MB。不能把项目自带 `partitions.csv` 当作当前已生效布局。
- 完整构建日志：`artifacts\idf_build_20260727.txt`。
- VS Code 构建证据截图：`artifacts\01_VSCode_构建成功.png`。
- VS Code 审计报告截图：`artifacts\02_VSCode_审计报告.png`。
- 审计结束时 VS Code 已打开本项目并停留在本报告；截图由实际 VS Code 窗口捕获，不是拼接图。
- 编译成功只证明当前配置下代码能够构建，不证明 WiFi、UART、SD、USB 或网页在开发板上工作。

## 3. 七项功能验证矩阵

状态定义沿用要求：B=编译验证通过、仍需硬件实测；C=部分实现；F=存在明显缺陷。

| 功能 | 状态 | 入口文件 | 核心函数 | 真实调用链 | 编译证据 | 尚需实测 | 主要风险 |
|---|---|---|---|---|---|---|---|
| WiFi AP + Station 混合模式 | B | `main/app_main.c` | `wifi_config_init`、`wifi_apply_ap`、`wifi_connect_sta`、`wifi_event_handler` | `app_main` -> `wifi_config_init` 创建 AP/STA netif 并注册 WIFI/IP 事件 -> `wifi_apply_ap` 设置 `WIFI_MODE_APSTA` 并启动 -> 可选 `wifi_connect_sta` | 相关源文件编译、最终链接成功 | AP 启动、STA 获取 IP、掉线重连、信号与长期稳定性 | 断线回调内直接 `vTaskDelay(5000)` 阻塞默认事件循环；AP 密码会明文打印日志；`enable_ap` 配置未被启动逻辑采用 |
| USB 或 AP 网页配网 | C | `main/app_main.c`、`main/web_server.c`、`main/app.js`、`main/usb_cdc.c` | `wifi_set_handler`、`wifi_connect_sta`、`usb_cdc_rx_handler` | 后端真实接口是 `POST /api/wifi` -> 保存 `/sdcard/config.json` -> 连接 STA；但嵌入前端调用不存在的 `/api/wifi/connect`、`/api/wifi/disconnect`、`/api/wifi/scan`；USB CDC 接收回调直接丢弃数据 | 全部编译成功 | 需要修复后再实测 AP 配网；USB 配网当前没有闭环 | 配置未保存到 NVS，而是 SD 卡；没有 USB 命令解析；前后端路由不一致使网页按钮无法完成配网 |
| 手机连接 AP 后访问网页 | B | `main/app_main.c`、`main/web_server.c`、`main/CMakeLists.txt` | `web_server_start`、`index_handler`、`css_handler`、`js_handler` | `app_main` -> `web_server_start` -> `/`、`/app.css`、`/app.js`；静态资源由 `EMBED_FILES` 编入固件 | 嵌入资源和 HTTP 服务编译、链接成功 | 手机连接 AP 后访问配置 IP、静态资源、404、断连恢复 | 默认 IP 来自 SD 配置，缺省为 `192.168.4.1`；无外部 CDN；页面包含本地模拟数据，但设备模式并非全部接口都有后端实现 |
| 单串口实时监控和双向透传 | C | `main/app_main.c`、`main/uart_driver.c`、`main/data_recorder.c`、`main/web_server.c` | `uart_rx_task`、`user_uart_read/write`、`recorder_live_fetch`、`send_tx_handler` | RX：UART0 -> `uart_rx_task` -> `recorder_write_rx` -> live ring -> `GET /api/stream`；TX：`POST /api/send` -> `user_uart_write` -> UART0 | 相关源文件和固件链接成功 | GPIO43/44 电气连接、吞吐、丢包、双向压力、Type-C 数据可见性 | `recorder_write_tx` 与 `user_uart_set_tx_callback` 从未接入，TX 不进入网页 live；USB CDC 不转发 UART；控制台与用户串口都配置为 UART0，启动 REPL 可能失败并触发 `ESP_ERROR_CHECK` 重启/中止 |
| 局域网开始/停止 SD 卡记录 | F | `main/app_main.c`、`main/sd_card.c`、`main/data_recorder.c`、`main/web_server.c` | `sd_card_init`、`recorder_start/stop`、`recorder_write_rx`、`recorder_io_task` | `POST /api/recorder/start` -> 生成待用文件名；UART RX -> ring buffer -> IO task -> FATFS 文件；`POST /api/recorder/stop` -> 同步/关闭 | 编译成功，但存在可由代码确认的数据完整性缺陷 | SD SPI 引脚、卡拔出、写满、写失败、持续吞吐、断电恢复 | `recorder_stop` 先把 `s_running=false`，随后 `recorder_force_sync` 调用的 `recorder_flush_save_rb` 会因 `!s_running` 立即返回，停止时环形缓冲区残留数据可能丢失；仅 RX 落盘，TX 不落盘 |
| 串口波特率配置 | C | `main/app.js`、`main/config_manager.c`、`main/uart_driver.c`、`main/web_server.c` | `user_uart_init`、`user_uart_set_baudrate` | 启动时从 `/sdcard/config.json` 读取 baudrate 并调用 `user_uart_init`；前端提交 `POST /api/uart/config`，后端未注册该接口，`user_uart_set_baudrate` 没有调用方 | 启动配置相关代码编译成功 | 启动恢复、运行时切换、错误参数、切换期间数据完整性 | 页面保存按钮当前不能改变硬件 UART，也不能写回配置；数据位、停止位、校验位只被读取，初始化仍固定为 8N1 |
| Type-C 模拟 U 盘 | B | `main/app_main.c`、`main/usb_cdc.c`、`main/usb_msc.c`、`main/sd_card.c` | `tinyusb_driver_install`、`tud_msc_read10_cb`、`tud_msc_write10_cb`、`tud_mount_cb`、`tud_umount_cb` | TinyUSB 安装 -> MSC 回调直接读写 SD 扇区；USB mount -> 停止记录并卸载 FATFS；USB disconnect -> 重新挂载 FATFS | TinyUSB CDC/MSC 配置启用，MSC 回调和最终固件链接成功 | Windows 枚举盘符、读写、弹出、断连、重挂载、SD 容量与扇区边界 | 自定义 USB 配置描述符只声明 MSC 接口却仍初始化 CDC；GPIO48 被硬编码为 VBUS monitor；停止记录的数据丢失缺陷会影响切换安全；`usb_msc_init` 本身只是占位入口，真正实现依赖静态 TinyUSB 回调 |

矩阵统计：

- B（编译通过、仍需硬件实测）：3 项。
- C（部分实现）：3 项。
- D（仅框架/占位）：0 项（但 `usb_msc_init` 单个入口函数为占位性质）。
- E（未找到实现）：0 项。
- F（明显缺陷）：1 项。

## 4. 真实文件、函数与配置证据

### WiFi

- AP 和 STA 接口：`esp_netif_create_default_wifi_ap`、`esp_netif_create_default_wifi_sta`。
- 混合模式：`esp_wifi_set_mode(WIFI_MODE_APSTA)`。
- 事件：注册 `WIFI_EVENT/ESP_EVENT_ANY_ID` 和 `IP_EVENT/IP_EVENT_STA_GOT_IP`。
- AP SSID：配置为空时由 `AP_SSID_PREFIX + MAC 后两字节` 生成。
- AP 密码：缺省硬编码为 `12345678`，也可由 `/sdcard/config.json` 覆盖。
- STA 参数：从 `/sdcard/config.json` 或 `POST /api/wifi` 读取/更新；不是 NVS。

### HTTP 和嵌入页面

- HTTP Server 在 `app_main` 中调用 `web_server_start`。
- 根页面及 CSS/JS 在 `main/CMakeLists.txt` 通过 `EMBED_FILES` 编入固件。
- 服务端实际路由集中在 `web_server_start` 的 `uris[]`。
- 页面不依赖外部 CDN。
- `web` 目录是未参与当前 `main/CMakeLists.txt` 嵌入的重复副本，固件实际使用 `main/index.html`、`main/app.css`、`main/app.js`。

### UART 和记录

- 用户串口：`UART_NUM_0`，TX GPIO43，RX GPIO44，缺省 115200。
- GPIO16 还被 `app_main` 硬编码为输出低电平，注释称用于串口接收使能，但没有原理图证据。
- SD：SDSPI/SPI2，CS=10、MOSI=38、MISO=40、SCK=39，20 MHz。
- 记录数据是 UART RX 原始字节；不是 IMU 解析结果；TX 不落盘。
- 记录文件：`/sdcard/REC_yyyyMMdd_HHmmss[_N].bin`，首个 RX 字节到达时才创建。

### USB

- ESP32-S3 原生 TinyUSB，CDC 与 MSC Kconfig 均启用。
- MSC 扇区读写调用 `sdmmc_read_sectors`/`sdmmc_write_sectors`。
- USB 与 FATFS 切换通过 `tud_mount_cb`/`tud_umount_cb`。
- USB CDC 收到的数据未解析、未转发，因此不能认定 USB 配网或 Type-C 串口透传已完成。

## 5. 未调用、占位、模拟和重复实现

### 疑似废弃或未接入

- `wifi_start_ap`：没有项目调用方。
- `web_server_stop`：没有项目调用方。
- `user_uart_set_baudrate`：没有项目调用方。
- `user_uart_set_tx_callback`：没有项目调用方。
- `recorder_write_tx`：没有项目调用方。
- `usb_cdc_write`、`usb_cdc_read`：没有项目调用方。
- `usb_cdc_rx_handler`：被注册但函数体明确丢弃收到的数据。
- `web` 目录：与 `main` 中静态页面重复，当前固件不嵌入该目录。

### 模拟数据

- `main/app.js` 和 `web/app.js` 在 localhost/127.0.0.1/8080 下进入模拟模式。
- 模拟模式生成 IMU、WiFi、SD、文件列表、运行时间等假数据，只能用于页面预览，不能作为硬件功能证据。

### TODO/FIXME/空函数

- 项目自有 `main` 与 `web` 中未发现 TODO/FIXME。
- `usb_msc_init` 只记录“callbacks registered”并返回成功，属于占位入口；MSC 真正逻辑在 TinyUSB 回调函数中。

## 6. 高风险问题

1. **UART0 资源冲突**：`USER_UART_NUM` 为 UART0，`sdkconfig` 同时配置 ESP Console UART0；`console_start` 创建 UART REPL。已有驱动再次安装或重配可能失败，且错误由 `ESP_ERROR_CHECK` 处理，可能导致运行中止。代码注释声称控制台是 UART1/GPIO17/18，但当前 `sdkconfig` 明确是 UART0，注释与配置不一致。
2. **停止记录可能丢失缓冲数据**：`recorder_stop` 的状态切换顺序使最终 ring buffer flush 被跳过。
3. **前后端 API 不一致**：网页 WiFi、UART 配置按钮调用不存在的服务端路由，页面演示不能证明功能闭环。
4. **分区配置不一致且空间逼近上限**：自带 `partitions.csv` 未启用，实际 App 分区只剩 2%，后续极小改动也可能链接失败。
5. **VS Code 工具链与串口配置失效**：配置路径不存在、COM25 不存在；本次构建通过手动定位本机现有 IDF 才完成。
6. **USB 描述符组合可疑**：配置启用 CDC+MSC，但自定义 configuration descriptor 只列 MSC；必须实机确认 CDC/MSC 是否按预期同时枚举。
7. **并发/阻塞风险**：WiFi 默认事件循环中有 5 秒延迟；记录 live 暂存区部分状态未统一受互斥保护；HTTP 任务直接执行 SD 配置文件读写和 WiFi stop/start。

## 7. 需要补充的硬件资料

- ESP32-S3 具体模组/开发板型号、Flash 容量、PSRAM 容量。
- 完整原理图和 PCB 引脚表。
- GPIO10/38/39/40 的 SD 卡连接及上拉、电平和供电说明。
- GPIO43/44 用户串口外接接口、电平、电气方向；GPIO16 串口使能的真实作用。
- GPIO48 是否真实连接 USB VBUS 检测。
- Type-C 口是否直连 ESP32-S3 原生 USB D-/D+，是否另有 USB-UART 芯片。
- 当前四个 COM 口与实际开发板的对应关系。
- 实测用 SD 卡型号、容量、文件系统和速度等级。

## 8. 建议后续硬件验证顺序

1. 先核对原理图、开发板型号、Flash 容量和 VS Code 串口，不烧录前记录当前板上固件状态。
2. 修复 UART0/Console 冲突并明确调试口与用户数据口。
3. 修复 `recorder_stop` 最终 flush 顺序，做短包停止、持续满速、拔卡、写满和断电测试。
4. 对齐网页与后端 WiFi/UART API，移除硬件验证流程中的模拟模式证据。
5. 确认实际分区表与 Flash 容量，给 App 保留合理余量。
6. 分别验证 AP 网页、STA 重连和双向 UART，再做组合压力测试。
7. 最后验证 USB MSC 枚举、读写、弹出、断连重挂载及记录/MSC 互斥。

## 9. 阶段结束条件检查

- [x] 完整读取项目自有源码、配置、构建入口和相关本地 TinyUSB 集成结构
- [x] 检查 Git 状态
- [x] 检查 ESP-IDF 环境
- [x] 检查目标芯片
- [x] 实际执行 IDF build
- [x] 对七项功能逐项给出代码证据
- [x] 区分代码存在、编译通过和硬件验证
- [x] 创建本报告
- [x] 未修改业务代码
- [x] 未烧录开发板

## 10. `idf.py build` 最后 20 行

```text
Generated D:/zhuomian/weite/ESP32/esp_recorder_v1.1/build/esp_recorder_v1.0.bin

[1066/1066] ... check_sizes.py ... esp_recorder_v1.0.bin

esp_recorder_v1.0.bin binary size 0xfbb40 bytes.
Smallest app partition is 0x100000 bytes.
0x44c0 bytes (2%) free.

Warning: The smallest app partition is nearly full (2% free space left)!

Project build complete. To flash, run:
 idf.py flash
or
 idf.py -p PORT flash
or
 python -m esptool --chip esp32s3 ... write_flash ...
or from the build directory:
 python -m esptool --chip esp32s3 ... write_flash "@flash_args"
```

注意：以上只是构建工具给出的后续烧录提示。本阶段没有执行任何一条烧录命令。
