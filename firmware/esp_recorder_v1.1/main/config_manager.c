#include "config_manager.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "cJSON.h"

#define TAG "config"
#define CONFIG_MAX_SIZE 4096

/* 把任意 authmode 数字 clamp 到 IDF 支持的合法取值；
 * 旧 config.json 写入过 0/1/4/5 等非法枚举时也按合法值规约。 */
static int clamp_authmode(int v)
{
    switch (v) {
    case WIFI_AUTH_OPEN:
    case WIFI_AUTH_WPA_PSK:
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return v;
    default:
        return WIFI_AUTH_OPEN;
    }
}

static config_manager_t s_instance = {0};

config_manager_t* config_manager_instance(void)
{
    return &s_instance;
}

void config_manager_reset_defaults(config_manager_t* manager)
{
    if (!manager) return;
    
    memset(manager, 0, sizeof(config_manager_t));
    
    manager->cfg.uart.baudrate = 115200;
    manager->cfg.uart.databits = 8;
    manager->cfg.uart.stopbits = 1;
    manager->cfg.uart.parity = 0;

    manager->cfg.wifi.enable_ap = true;
    manager->cfg.wifi.ap_ssid[0] = '\0';          /* 空 → wifi_start_ap 按 MAC 生成默认 SSID */
    strncpy(manager->cfg.wifi.ap_pass, "12345678", sizeof(manager->cfg.wifi.ap_pass) - 1);
    manager->cfg.wifi.ap_pass[sizeof(manager->cfg.wifi.ap_pass) - 1] = '\0';
    strncpy(manager->cfg.wifi.ap_ip, "192.168.4.1", sizeof(manager->cfg.wifi.ap_ip) - 1);
    manager->cfg.wifi.ap_ip[sizeof(manager->cfg.wifi.ap_ip) - 1] = '\0';

    manager->cfg.wifi.enable_sta = false;
    manager->cfg.wifi.sta_ssid[0] = '\0';
    manager->cfg.wifi.sta_pass[0] = '\0';
    manager->cfg.wifi.sta_authmode = WIFI_AUTH_OPEN;

    manager->cfg.recorder.auto_start = true;
    manager->cfg.recorder.stream_gap_ms = 100;
    manager->loaded = false;
}

static int get_json_int(cJSON* root, const char* key, int default_val)
{
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (item && cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return default_val;
}

static const char* get_json_string(cJSON* root, const char* key, const char* default_val)
{
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (item && cJSON_IsString(item) && item->valuestring != NULL) {
        return item->valuestring;
    }
    return default_val;
}

static void copy_string(char* dst, size_t dst_size, const char* src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

esp_err_t config_manager_load(config_manager_t* manager, const char* path)
{
    if (!manager || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    config_manager_reset_defaults(manager);

    FILE* f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "config file not found: %s, using defaults", path);
        manager->loaded = false;
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > CONFIG_MAX_SIZE) {
        ESP_LOGE(TAG, "invalid config file size: %ld", size);
        fclose(f);
        manager->loaded = false;
        return ESP_ERR_INVALID_SIZE;
    }

    char* buf = (char*)malloc(size + 1);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buf, 1, size, f);
    fclose(f);
    if (read != (size_t)size) {
        ESP_LOGE(TAG, "failed to read config file");
        free(buf);
        manager->loaded = false;
        return ESP_FAIL;
    }
    buf[size] = '\0';

    cJSON* root = cJSON_Parse(buf);
    free(buf);

    if (root == NULL) {
        ESP_LOGE(TAG, "failed to parse config.json");
        manager->loaded = false;
        return ESP_ERR_INVALID_STATE;
    }

    cJSON* uart = cJSON_GetObjectItem(root, "uart");
    if (uart) {
        manager->cfg.uart.baudrate = get_json_int(uart, "baudrate", manager->cfg.uart.baudrate);
        manager->cfg.uart.databits = get_json_int(uart, "databits", manager->cfg.uart.databits);
        manager->cfg.uart.stopbits = get_json_int(uart, "stopbits", manager->cfg.uart.stopbits);
        manager->cfg.uart.parity = get_json_int(uart, "parity", manager->cfg.uart.parity);
    }

    cJSON* wifi = cJSON_GetObjectItem(root, "wifi");
    if (wifi) {
        manager->cfg.wifi.enable_ap = get_json_int(wifi, "enable_ap", manager->cfg.wifi.enable_ap) != 0;
        copy_string(manager->cfg.wifi.ap_ssid, sizeof(manager->cfg.wifi.ap_ssid),
                   get_json_string(wifi, "ap_ssid", manager->cfg.wifi.ap_ssid));
        copy_string(manager->cfg.wifi.ap_pass, sizeof(manager->cfg.wifi.ap_pass),
                   get_json_string(wifi, "ap_pass", manager->cfg.wifi.ap_pass));
        copy_string(manager->cfg.wifi.ap_ip, sizeof(manager->cfg.wifi.ap_ip),
                   get_json_string(wifi, "ap_ip", manager->cfg.wifi.ap_ip));

        manager->cfg.wifi.enable_sta = get_json_int(wifi, "enable_sta", manager->cfg.wifi.enable_sta) != 0;
        copy_string(manager->cfg.wifi.sta_ssid, sizeof(manager->cfg.wifi.sta_ssid),
                   get_json_string(wifi, "sta_ssid", manager->cfg.wifi.sta_ssid));
        copy_string(manager->cfg.wifi.sta_pass, sizeof(manager->cfg.wifi.sta_pass),
                   get_json_string(wifi, "sta_pass", manager->cfg.wifi.sta_pass));
        manager->cfg.wifi.sta_authmode = clamp_authmode(
            get_json_int(wifi, "sta_authmode", manager->cfg.wifi.sta_authmode));
    }

    cJSON* recorder = cJSON_GetObjectItem(root, "recorder");
    if (recorder) {
        manager->cfg.recorder.auto_start = get_json_int(recorder, "auto_start", manager->cfg.recorder.auto_start) != 0;
        int gap = get_json_int(recorder, "stream_gap_ms", manager->cfg.recorder.stream_gap_ms);
        manager->cfg.recorder.stream_gap_ms = config_manager_clamp_stream_gap_ms(gap);
    } else {
        /* recorder 段缺失时也走一次 clamp（默认值兜底） */
        manager->cfg.recorder.stream_gap_ms =
            config_manager_clamp_stream_gap_ms(manager->cfg.recorder.stream_gap_ms);
    }

    cJSON_Delete(root);

    manager->loaded = true;
    ESP_LOGI(TAG, "config loaded from %s", path);
    ESP_LOGI(TAG, "uart: baud=%d db=%d sb=%d parity=%d",
             manager->cfg.uart.baudrate, manager->cfg.uart.databits,
             manager->cfg.uart.stopbits, manager->cfg.uart.parity);
    ESP_LOGI(TAG, "wifi: ap_en=%d sta_en=%d",
             manager->cfg.wifi.enable_ap, manager->cfg.wifi.enable_sta);
    return ESP_OK;
}

const device_config_t* config_manager_get(const config_manager_t* manager)
{
    if (!manager) return NULL;
    return &manager->cfg;
}

esp_err_t config_manager_save(const config_manager_t* manager, const char* path)
{
    if (!manager || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON* root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON* uart = cJSON_CreateObject();
    cJSON_AddNumberToObject(uart, "baudrate", manager->cfg.uart.baudrate);
    cJSON_AddNumberToObject(uart, "databits", manager->cfg.uart.databits);
    cJSON_AddNumberToObject(uart, "stopbits", manager->cfg.uart.stopbits);
    cJSON_AddNumberToObject(uart, "parity", manager->cfg.uart.parity);
    cJSON_AddItemToObject(root, "uart", uart);

    cJSON* wifi = cJSON_CreateObject();
    cJSON_AddNumberToObject(wifi, "enable_ap", manager->cfg.wifi.enable_ap ? 1 : 0);
    cJSON_AddStringToObject(wifi, "ap_ssid", manager->cfg.wifi.ap_ssid);
    cJSON_AddStringToObject(wifi, "ap_pass", manager->cfg.wifi.ap_pass);
    cJSON_AddStringToObject(wifi, "ap_ip", manager->cfg.wifi.ap_ip);
    cJSON_AddNumberToObject(wifi, "enable_sta", manager->cfg.wifi.enable_sta ? 1 : 0);
    cJSON_AddStringToObject(wifi, "sta_ssid", manager->cfg.wifi.sta_ssid);
    cJSON_AddStringToObject(wifi, "sta_pass", manager->cfg.wifi.sta_pass);
    cJSON_AddNumberToObject(wifi, "sta_authmode", manager->cfg.wifi.sta_authmode);
    cJSON_AddItemToObject(root, "wifi", wifi);

    cJSON* recorder = cJSON_CreateObject();
    cJSON_AddNumberToObject(recorder, "auto_start", manager->cfg.recorder.auto_start ? 1 : 0);
    cJSON_AddNumberToObject(recorder, "stream_gap_ms", manager->cfg.recorder.stream_gap_ms);
    cJSON_AddItemToObject(root, "recorder", recorder);

    char* json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    FILE* f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "failed to create config file: %s", path);
        cJSON_free(json_str);
        return ESP_ERR_INVALID_STATE;
    }

    fputs(json_str, f);
    fclose(f);
    cJSON_free(json_str);

    ESP_LOGI(TAG, "default config saved to %s", path);
    return ESP_OK;
}

int config_manager_clamp_stream_gap_ms(int v)
{
    if (v < 1) return 1;
    if (v > 1000) return 1000;
    return v;
}

esp_err_t config_manager_set_stream_gap_ms(config_manager_t* manager, int v,
                                            const char* save_path)
{
    if (!manager) return ESP_ERR_INVALID_ARG;
    int clamped = config_manager_clamp_stream_gap_ms(v);
    int old = manager->cfg.recorder.stream_gap_ms;
    manager->cfg.recorder.stream_gap_ms = clamped;

    esp_err_t err = save_path ? config_manager_save(manager, save_path) : ESP_OK;
    if (err != ESP_OK) {
        /* 写盘失败回滚到旧值 */
        manager->cfg.recorder.stream_gap_ms = old;
        ESP_LOGE(TAG, "save config failed, roll back stream_gap_ms to %d", old);
        return err;
    }
    return ESP_OK;
}