# ESP32 WiFi 记录仪完整功能副本

## 工程位置

- 完整功能副本：`D:\zhuomian\weite\ESP32\esp_recorder_v1.1_full_features`
- 原工程：`D:\zhuomian\weite\ESP32\esp_recorder_v1.1`
- 已核对原工程 1952 个文件，未发生修改。

## 已完成功能

1. 双路数据接入
   - CH1：UART0，TX GPIO43，RX GPIO44
   - CH2：UART2，TX GPIO17，RX GPIO18
   - 两路状态及 RX/TX 字节计数可在手机网页查看。
2. 数据路由
   - CH1 RX 自动转发到 CH2 TX。
   - CH2 RX 自动转发到 CH1 TX。
   - 网页可启停路由，并保存到 NVS。
3. SD 卡记录
   - 两路 RX 分别保存为 `REC_CH1_*.bin` 和 `REC_CH2_*.bin`。
   - 使用独立缓存、定时刷新和 `fsync`，并统计丢失字节。
   - 文件名唯一，不覆盖已有记录。
4. 文件管理接口
   - 手机网页支持列表、下载、删除。
   - 记录期间禁止文件操作，避免与写卡冲突。
   - Type-C 作为 U 盘时网页显示“USB 占用”，ESP32 不再同时访问文件系统。
5. 参数持久化
   - UART 波特率、路由开关、自动记录、Stream gap、AP/STA 参数写入 NVS。
   - SD 卡可用时同步生成 `/sdcard/config.json` 镜像。
   - 网页修改失败时自动回滚，不会显示假成功。

## 编译结果

- ESP-IDF：5.4
- 芯片目标：ESP32-S3
- 结果：`Project build complete`
- 固件：`build\esp_recorder_v1.0.bin`
- 固件大小：1033504 字节
- SHA-256：`D5EDC38367544F4D7F15144754D0E3608B69BF6CBD4359BFB0A7BC5C7E593FC7`
- 编译日志：`artifacts\full_features_build_20260803.txt`

## 手机验证顺序

1. 烧录本副本生成的固件，断开电脑 USB 数据连接后重新供电。
2. 手机连接设备 AP，打开状态页显示的 AP IP。
3. 在“运行参数”保存波特率、路由开关和自动记录，重启后确认数值不变。
4. 分别向 CH1 RX、CH2 RX 发送数据，确认两路 RX 计数增加，交叉路由计数增加。
5. 开始记录并发送两路数据，确认 CH1/CH2 记录字节数增加且丢失数为 0。
6. 停止记录，刷新文件列表，分别下载 `REC_CH1_*.bin`、`REC_CH2_*.bin` 检查内容。
7. 连接电脑 USB 后，确认网页显示“USB 占用”，电脑可独占访问 SD 卡；断开后网页恢复挂载。

本次只完成独立副本开发和编译，没有执行烧录。
