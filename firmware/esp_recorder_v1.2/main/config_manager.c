#include "config_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs.h"

#define TAG "config"
#define CONFIG_MAX_SIZE 4096
#define CONFIG_SCHEMA_VERSION 2
#define CONFIG_NVS_NAMESPACE "recorder_cfg"
#define CONFIG_NVS_KEY "config_json"

static config_manager_t s_instance;

static int clamp_authmode(int value)
{
    switch (value) {
    case WIFI_AUTH_OPEN:
    case WIFI_AUTH_WPA_PSK:
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return value;
    default:
        return WIFI_AUTH_OPEN;
    }
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    if (src == dst) {
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static int json_get_int(const cJSON *root, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static bool json_get_bool(const cJSON *root, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    if (cJSON_IsNumber(item)) {
        return item->valueint != 0;
    }
    return fallback;
}

static const char *json_get_string(const cJSON *root, const char *key,
                                   const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring != NULL
               ? item->valuestring
               : fallback;
}

static esp_err_t parse_json(config_manager_t *manager, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *uart = cJSON_GetObjectItemCaseSensitive(root, "uart");
    if (cJSON_IsObject(uart)) {
        int baudrate = json_get_int(uart, "baudrate", manager->cfg.uart.baudrate);
        if (config_manager_valid_baudrate(baudrate)) {
            manager->cfg.uart.baudrate = baudrate;
        }
        manager->cfg.uart.databits = json_get_int(uart, "databits", 8);
        manager->cfg.uart.stopbits = json_get_int(uart, "stopbits", 1);
        manager->cfg.uart.parity = json_get_int(uart, "parity", 0);
    }

    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        manager->cfg.wifi.enable_ap =
            json_get_bool(wifi, "enable_ap", manager->cfg.wifi.enable_ap);
        copy_string(manager->cfg.wifi.ap_ssid, sizeof(manager->cfg.wifi.ap_ssid),
                    json_get_string(wifi, "ap_ssid", manager->cfg.wifi.ap_ssid));
        copy_string(manager->cfg.wifi.ap_pass, sizeof(manager->cfg.wifi.ap_pass),
                    json_get_string(wifi, "ap_pass", manager->cfg.wifi.ap_pass));
        copy_string(manager->cfg.wifi.ap_ip, sizeof(manager->cfg.wifi.ap_ip),
                    json_get_string(wifi, "ap_ip", manager->cfg.wifi.ap_ip));
        manager->cfg.wifi.enable_sta =
            json_get_bool(wifi, "enable_sta", manager->cfg.wifi.enable_sta);
        copy_string(manager->cfg.wifi.sta_ssid, sizeof(manager->cfg.wifi.sta_ssid),
                    json_get_string(wifi, "sta_ssid", manager->cfg.wifi.sta_ssid));
        copy_string(manager->cfg.wifi.sta_pass, sizeof(manager->cfg.wifi.sta_pass),
                    json_get_string(wifi, "sta_pass", manager->cfg.wifi.sta_pass));
        manager->cfg.wifi.sta_authmode = clamp_authmode(
            json_get_int(wifi, "sta_authmode", manager->cfg.wifi.sta_authmode));
    }

    cJSON *recorder = cJSON_GetObjectItemCaseSensitive(root, "recorder");
    if (cJSON_IsObject(recorder)) {
        manager->cfg.recorder.auto_start = json_get_bool(
            recorder, "auto_start", manager->cfg.recorder.auto_start);
        manager->cfg.recorder.stream_gap_ms = config_manager_clamp_stream_gap_ms(
            json_get_int(recorder, "stream_gap_ms",
                         manager->cfg.recorder.stream_gap_ms));
    }

    cJSON *router = cJSON_GetObjectItemCaseSensitive(root, "router");
    if (cJSON_IsObject(router)) {
        manager->cfg.router.enabled =
            json_get_bool(router, "enabled", manager->cfg.router.enabled);
    }

    cJSON_Delete(root);
    manager->loaded = true;
    return ESP_OK;
}

static char *build_json(const config_manager_t *manager)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "schema_version", CONFIG_SCHEMA_VERSION);

    cJSON *uart = cJSON_AddObjectToObject(root, "uart");
    cJSON_AddNumberToObject(uart, "baudrate", manager->cfg.uart.baudrate);
    cJSON_AddNumberToObject(uart, "databits", manager->cfg.uart.databits);
    cJSON_AddNumberToObject(uart, "stopbits", manager->cfg.uart.stopbits);
    cJSON_AddNumberToObject(uart, "parity", manager->cfg.uart.parity);

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "enable_ap", manager->cfg.wifi.enable_ap);
    cJSON_AddStringToObject(wifi, "ap_ssid", manager->cfg.wifi.ap_ssid);
    cJSON_AddStringToObject(wifi, "ap_pass", manager->cfg.wifi.ap_pass);
    cJSON_AddStringToObject(wifi, "ap_ip", manager->cfg.wifi.ap_ip);
    cJSON_AddBoolToObject(wifi, "enable_sta", manager->cfg.wifi.enable_sta);
    cJSON_AddStringToObject(wifi, "sta_ssid", manager->cfg.wifi.sta_ssid);
    cJSON_AddStringToObject(wifi, "sta_pass", manager->cfg.wifi.sta_pass);
    cJSON_AddNumberToObject(wifi, "sta_authmode", manager->cfg.wifi.sta_authmode);

    cJSON *recorder = cJSON_AddObjectToObject(root, "recorder");
    cJSON_AddBoolToObject(recorder, "auto_start", manager->cfg.recorder.auto_start);
    cJSON_AddNumberToObject(recorder, "stream_gap_ms",
                           manager->cfg.recorder.stream_gap_ms);

    cJSON *router = cJSON_AddObjectToObject(root, "router");
    cJSON_AddBoolToObject(router, "enabled", manager->cfg.router.enabled);

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t load_nvs_json(char **out_json)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t size = 0;
    err = nvs_get_str(handle, CONFIG_NVS_KEY, NULL, &size);
    if (err != ESP_OK || size == 0 || size > CONFIG_MAX_SIZE) {
        nvs_close(handle);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }

    char *json = malloc(size);
    if (json == NULL) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_str(handle, CONFIG_NVS_KEY, json, &size);
    nvs_close(handle);
    if (err != ESP_OK) {
        free(json);
        return err;
    }
    *out_json = json;
    return ESP_OK;
}

static esp_err_t save_nvs_json(const char *json)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, CONFIG_NVS_KEY, json);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t load_file_json(const char *path, char **out_json)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long size = ftell(file);
    rewind(file);
    if (size <= 0 || size > CONFIG_MAX_SIZE) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    char *json = malloc((size_t)size + 1);
    if (json == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(json, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(json);
        return ESP_FAIL;
    }
    json[size] = '\0';
    *out_json = json;
    return ESP_OK;
}

static esp_err_t write_file_json(const char *path, const char *json)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t size = strlen(json);
    size_t written = fwrite(json, 1, size, file);
    int close_result = fclose(file);
    return written == size && close_result == 0 ? ESP_OK : ESP_FAIL;
}

config_manager_t *config_manager_instance(void)
{
    return &s_instance;
}

void config_manager_reset_defaults(config_manager_t *manager)
{
    if (manager == NULL) {
        return;
    }
    memset(manager, 0, sizeof(*manager));
    manager->cfg.uart.baudrate = 115200;
    manager->cfg.uart.databits = 8;
    manager->cfg.uart.stopbits = 1;
    manager->cfg.wifi.enable_ap = true;
    copy_string(manager->cfg.wifi.ap_pass, sizeof(manager->cfg.wifi.ap_pass),
                "12345678");
    copy_string(manager->cfg.wifi.ap_ip, sizeof(manager->cfg.wifi.ap_ip),
                "192.168.4.1");
    manager->cfg.wifi.sta_authmode = WIFI_AUTH_OPEN;
    manager->cfg.recorder.auto_start = true;
    manager->cfg.recorder.stream_gap_ms = 100;
    manager->cfg.router.enabled = true;
}

esp_err_t config_manager_load(config_manager_t *manager, const char *path)
{
    if (manager == NULL || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    config_manager_reset_defaults(manager);

    char *json = NULL;
    esp_err_t nvs_err = load_nvs_json(&json);
    if (nvs_err == ESP_OK) {
        esp_err_t parse_err = parse_json(manager, json);
        free(json);
        if (parse_err == ESP_OK) {
            ESP_LOGI(TAG, "configuration loaded from NVS");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "invalid NVS configuration, trying SD mirror");
        config_manager_reset_defaults(manager);
    }

    esp_err_t file_err = load_file_json(path, &json);
    if (file_err != ESP_OK) {
        ESP_LOGW(TAG, "no usable configuration; defaults active");
        return file_err;
    }
    esp_err_t parse_err = parse_json(manager, json);
    free(json);
    if (parse_err != ESP_OK) {
        config_manager_reset_defaults(manager);
        return parse_err;
    }

    char *normalized = build_json(manager);
    if (normalized != NULL) {
        esp_err_t migrate_err = save_nvs_json(normalized);
        if (migrate_err != ESP_OK) {
            ESP_LOGW(TAG, "could not migrate SD configuration to NVS: %s",
                     esp_err_to_name(migrate_err));
        }
        cJSON_free(normalized);
    }
    ESP_LOGI(TAG, "configuration loaded from SD mirror");
    return ESP_OK;
}

esp_err_t config_manager_save(const config_manager_t *manager, const char *path)
{
    if (manager == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char *json = build_json(manager);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = save_nvs_json(json);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS configuration save failed: %s", esp_err_to_name(err));
        cJSON_free(json);
        return err;
    }

    if (path != NULL) {
        esp_err_t mirror_err = write_file_json(path, json);
        if (mirror_err != ESP_OK) {
            ESP_LOGW(TAG, "SD configuration mirror unavailable; NVS save is valid");
        }
    }
    cJSON_free(json);
    return ESP_OK;
}

const device_config_t *config_manager_get(const config_manager_t *manager)
{
    return manager == NULL ? NULL : &manager->cfg;
}

int config_manager_clamp_stream_gap_ms(int value)
{
    if (value < 1) return 1;
    if (value > 1000) return 1000;
    return value;
}

bool config_manager_valid_baudrate(int value)
{
    return value >= 1200 && value <= 2000000;
}

static esp_err_t save_or_rollback(config_manager_t *manager,
                                  const device_config_t *old_cfg,
                                  const char *save_path)
{
    esp_err_t err = config_manager_save(manager, save_path);
    if (err != ESP_OK) {
        manager->cfg = *old_cfg;
    }
    return err;
}

esp_err_t config_manager_set_stream_gap_ms(config_manager_t *manager, int value,
                                            const char *save_path)
{
    if (manager == NULL) return ESP_ERR_INVALID_ARG;
    device_config_t old_cfg = manager->cfg;
    manager->cfg.recorder.stream_gap_ms = config_manager_clamp_stream_gap_ms(value);
    return save_or_rollback(manager, &old_cfg, save_path);
}

esp_err_t config_manager_set_uart_baudrate(config_manager_t *manager, int value,
                                           const char *save_path)
{
    if (manager == NULL || !config_manager_valid_baudrate(value)) {
        return ESP_ERR_INVALID_ARG;
    }
    device_config_t old_cfg = manager->cfg;
    manager->cfg.uart.baudrate = value;
    return save_or_rollback(manager, &old_cfg, save_path);
}

esp_err_t config_manager_set_router_enabled(config_manager_t *manager, bool enabled,
                                            const char *save_path)
{
    if (manager == NULL) return ESP_ERR_INVALID_ARG;
    device_config_t old_cfg = manager->cfg;
    manager->cfg.router.enabled = enabled;
    return save_or_rollback(manager, &old_cfg, save_path);
}

esp_err_t config_manager_set_recorder_auto_start(config_manager_t *manager,
                                                 bool enabled,
                                                 const char *save_path)
{
    if (manager == NULL) return ESP_ERR_INVALID_ARG;
    device_config_t old_cfg = manager->cfg;
    manager->cfg.recorder.auto_start = enabled;
    return save_or_rollback(manager, &old_cfg, save_path);
}
