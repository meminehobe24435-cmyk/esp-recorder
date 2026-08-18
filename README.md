# 维特智能 ESP32-S3 WiFi 数据记录仪

面向维特智能客户（负责人：陈英炜）定制的 WiFi 数据记录仪，基于 ESP32-S3 + ESP-IDF（C / FreeRTOS），配套 Flutter 手机/桌面 App。

设备通过 AP+Station 混合模式工作，支持串口数据双向透传、SD 卡记录、USB CDC/MSC 复合设备（U 盘模式）、网页端配置与监控。

## 目录结构

```
├── firmware/       # ESP-IDF 固件（C），按版本迭代
│   ├── esp_recorder_v1.1/               # v1.1（早期）
│   ├── esp_recorder_v1.1(1)/            # v1.1 另一快照（与 v1.1 近似）
│   ├── esp_recorder_v1.1_full_features/ # v1.1 完整功能版
│   ├── esp_recorder_v1.2/               # v1.2
│   ├── esp_recorder_v1.2_dev/           # v1.2 开发版（新增 usb_cdc_bridge）
│   └── esp_recorder_v1.3_release_test/  # v1.3 最新（含 cdc 测试协议/时间同步）
├── app/            # Flutter 应用（Android/桌面），esp_recorder_app_v1.0
├── docs/           # 产品需求、开发任务、完整报告、环境配置等文档
├── scripts/        # 报告生成脚本（build_report_docx.py / build_tech_report.py）
├── screenshots/    # 功能界面截图与硬件验证截图
└── releases/       # 发布包（APK/压缩包/手册 docx）—— 不上传 git，见下方「发布包」
```

## 版本沿革

v1.1（双路 UART 接入）→ v1.2（USB CDC/MSC 复合设备）→ v1.2_dev（USB 桥接）→ **v1.3_release_test（最新，含 cdc 测试协议、时间同步、安全测试模式）**。

## 发布包（走 GitHub Releases）

以下文件体积大或为二进制发布物，通过 **GitHub Releases** 分发，不进 git 仓库：

- `releases/app-debug.apk`（Android 调试包，153MB）
- `releases/esp_recorder_v1.3_release_test.zip`
- `releases/esp_recorder_v1.1_full_features.zip`
- `releases/esp_recorder_v1.1.rar`
- `releases/ESP32_WiFi数据记录仪_V1.3_功能验证与使用手册(1).docx`

## 构建

固件（ESP-IDF 5.x，目标 esp32s3）：

```bash
cd firmware/esp_recorder_v1.3_release_test
idf.py set-target esp32s3   # 首次
idf.py build
```

App（Flutter）：见 `docs/FLUTTER_ENV_SETUP.md`。

## 其他

- `backup/` 为本地备份（含 git bundle、源码快照），不入库。
- 产品需求详见 `docs/ESP32.md`，开发任务详见 `docs/=任务.txt`。
