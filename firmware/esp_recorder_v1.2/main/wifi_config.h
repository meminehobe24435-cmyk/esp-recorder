#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <stdbool.h>
#include "esp_err.h"

/* 需求：3.1 / 3.2 / 3.3 — AP + Station 混合模式、配网与重试策略
 * WiFi 凭证统一存放在 /sdcard/config.json（不再使用 NVS） */

#define AP_SSID_PREFIX  "ESP_Recorder_"
#define AP_DEFAULT_PASS "12345678"
#define AP_IP           "192.168.4.1"
#define AP_GATEWAY      "192.168.4.1"
#define AP_NETMASK      "255.255.255.0"

typedef enum {
    REC_WIFI_MODE_AP_ONLY = 0,
    REC_WIFI_MODE_STA_ONLY,
    REC_WIFI_MODE_AP_STA,
} rec_wifi_mode_t;

typedef struct {
    rec_wifi_mode_t mode;
    bool ap_started;
    bool sta_connected;
    char sta_ssid[32];
    char sta_pass[64];
} wifi_state_t;

esp_err_t wifi_config_init(void);
esp_err_t wifi_start_ap(void);

/* 用新参数启动或重启 AP（IP/SSID/密码/authmode 改变时调用）。
 * ap_ssid 为空时按 MAC 自动生成；ap_pass 为空表示开放 AP。 */
esp_err_t wifi_apply_ap(const char *ap_ssid, const char *ap_pass, const char *ap_ip);

/* authmode 取 esp_wifi_auth_mode_t (WIFI_AUTH_OPEN=0 / WPA2_PSK=3 / WPA3_PSK=6 / ...)。 */
esp_err_t wifi_connect_sta(const char *ssid, const char *pass, int authmode);
esp_err_t wifi_disconnect_sta(void);
const wifi_state_t *wifi_get_state(void);

/* 获取 STA 接口当前的 IP 地址（点分十进制）。 */
esp_err_t wifi_get_sta_ip(char *out, size_t out_size);

/* 获取 STA 接口的信号强度 (RSSI)，未连接时返回 0。 */
int wifi_get_sta_rssi(void);

#endif