#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 设备运行配置，由 /sdcard/config.json 解析得到。
 * 缺失字段使用下方默认值。 */
typedef struct {
    struct {
        int baudrate;   /* 默认 115200 */
        int databits;   /* 默认 8 */
        int stopbits;   /* 默认 1 */
        int parity;     /* 0=none, 1=odd, 2=even，默认 0 */
    } uart;

    struct {
        bool enable_ap;       /* 默认 true */
        char ap_ssid[32];     /* 默认空 → fallback 到 "ESP_Recorder_XXXX" (MAC 后两位) */
        char ap_pass[64];     /* 默认 "12345678"；空串表示开放 AP */
        char ap_ip[16];       /* 默认 "192.168.4.1"；网关/掩码按 /24 推算 */
        bool enable_sta;      /* 默认 false */
        char sta_ssid[32];    /* 默认空 */
        char sta_pass[64];    /* 默认空 */
        int  sta_authmode;    /* esp_wifi_auth_mode_t；默认 WIFI_AUTH_OPEN(0) */
    } wifi;

    struct {
        bool auto_start;      /* 默认 true */
        int  stream_gap_ms;   /* 默认 100，范围 [1, 1000]；live 区 "等待数据..." 阈值 */
    } recorder;
} device_config_t;

/* 配置管理器结构体 */
typedef struct {
    device_config_t cfg;
    bool loaded;
} config_manager_t;

/* 获取配置管理器实例（单例） */
config_manager_t* config_manager_instance(void);

/* 从指定路径加载 config.json，失败时保留默认值 */
esp_err_t config_manager_load(config_manager_t* manager, const char* path);

/* 将当前默认/已加载的配置保存到指定路径 */
esp_err_t config_manager_save(const config_manager_t* manager, const char* path);

/* 获取当前配置指针；load 前返回的是带默认值的配置 */
const device_config_t* config_manager_get(const config_manager_t* manager);

/* 重置为默认值 */
void config_manager_reset_defaults(config_manager_t* manager);

/* 把 stream_gap_ms clamp 到 [1, 1000] */
int  config_manager_clamp_stream_gap_ms(int v);

/* 设置 stream_gap_ms（先 clamp，再写内存，最后落盘；失败回滚） */
esp_err_t config_manager_set_stream_gap_ms(config_manager_t* manager, int v,
                                          const char* save_path);

#endif /* CONFIG_MANAGER_H */