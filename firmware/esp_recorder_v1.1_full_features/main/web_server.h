#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

/* 需求：3.2 / 4.1 / 6.3 — AP 模式本地 Web 配网与状态/控制接口
 * 监听端口 80；静态页面与 REST API 共同完成：
 *   - 查看设备状态（AP/STA、SD 卡、记录）
 *   - 设置 STA SSID/密码（写入 /sdcard/config.json 并触发重连）
 *   - 启动 / 停止记录
 *   - 列出与下载 CSV 记录文件
 */

esp_err_t web_server_start(void);
esp_err_t web_server_stop(void);

#endif /* WEB_SERVER_H */