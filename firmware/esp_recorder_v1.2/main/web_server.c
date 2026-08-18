#include "web_server.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "wifi_config.h"
#include "uart_driver.h"
#include "data_recorder.h"
#include "data_router.h"
#include "config_manager.h"
#include "sd_card.h"

#define TAG "web_server"
#define WEB_PORT 80
#define FW_VERSION "v1.2.0"

static httpd_handle_t s_server = NULL;

/* ---------------- 工具 ---------------- */

/* 从 query string 中取 key */
static bool get_query_value(httpd_req_t *req, const char *key,
                            char *out, size_t out_size)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0) return false;
    char *qbuf = (char *)malloc(qlen + 1);
    if (qbuf == NULL) return false;
    if (httpd_req_get_url_query_str(req, qbuf, qlen + 1) != ESP_OK) {
        free(qbuf);
        return false;
    }
    esp_err_t err = httpd_query_key_value(qbuf, key, out, out_size);
    free(qbuf);
    return err == ESP_OK;
}

static bool is_safe_filename(const char *name)
{
    if (name == NULL || name[0] == '\0') return false;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') return false;
    }
    if (strstr(name, "..") != NULL) return false;
    return true;
}

static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json_ok(httpd_req_t *req) {
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t send_json_err(httpd_req_t *req, const char *err) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", err);
    return send_json(req, buf);
}

static bool valid_sta_authmode(int auth)
{
    return auth == WIFI_AUTH_OPEN || auth == WIFI_AUTH_WPA_PSK ||
           auth == WIFI_AUTH_WPA2_PSK || auth == WIFI_AUTH_WPA3_PSK ||
           auth == WIFI_AUTH_WPA2_WPA3_PSK;
}

/* ---------------- Handlers ---------------- */

static esp_err_t index_handler(httpd_req_t *req)
{
    extern const uint8_t _binary_index_html_start[];
    extern const uint8_t _binary_index_html_end[];
    const size_t len = (size_t)(_binary_index_html_end - _binary_index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)_binary_index_html_start, len);
}

static esp_err_t css_handler(httpd_req_t *req)
{
    extern const uint8_t _binary_app_css_start[];
    extern const uint8_t _binary_app_css_end[];
    const size_t len = (size_t)(_binary_app_css_end - _binary_app_css_start);
    httpd_resp_set_type(req, "text/css; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    return httpd_resp_send(req, (const char *)_binary_app_css_start, len);
}

static esp_err_t js_handler(httpd_req_t *req)
{
    extern const uint8_t _binary_app_js_start[];
    extern const uint8_t _binary_app_js_end[];
    const size_t len = (size_t)(_binary_app_js_end - _binary_app_js_start);
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    return httpd_resp_send(req, (const char *)_binary_app_js_start, len);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    const wifi_state_t *ws = wifi_get_state();
    const sd_card_state_t *sd = sd_card_get_state();
    const device_config_t *cfg = config_manager_get(config_manager_instance());
    data_router_stats_t route_stats;
    data_router_get_stats(&route_stats);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ap", ws->ap_started);
    cJSON_AddStringToObject(root, "ap_ssid",
        cfg->wifi.ap_ssid[0] ? cfg->wifi.ap_ssid : "-");
    cJSON_AddStringToObject(root, "ap_ip",
        cfg->wifi.ap_ip[0] ? cfg->wifi.ap_ip : "-");
    cJSON_AddBoolToObject(root, "sta_connected", ws->sta_connected);
    cJSON_AddStringToObject(root, "sta_ssid",
        cfg->wifi.sta_ssid[0] ? cfg->wifi.sta_ssid : "-");

    char sta_ip[16] = "-";
    wifi_get_sta_ip(sta_ip, sizeof(sta_ip));
    cJSON_AddStringToObject(root, "sta_ip", sta_ip);

    cJSON_AddBoolToObject(root, "sd", sd->mounted);
    cJSON_AddBoolToObject(root, "sd_usb_owned", sd->usb_owned);
    cJSON_AddBoolToObject(root, "file_api_available",
                          sd->mounted && !recorder_is_running());
    cJSON_AddBoolToObject(root, "recording", recorder_is_running());
    cJSON_AddBoolToObject(root, "file_open", recorder_is_file_open());

    cJSON_AddNumberToObject(root, "stream_gap_ms", cfg->recorder.stream_gap_ms);
    cJSON_AddNumberToObject(root, "uart_baudrate", cfg->uart.baudrate);
    cJSON_AddBoolToObject(root, "recorder_auto_start", cfg->recorder.auto_start);

    cJSON_AddNumberToObject(root, "rec_drop_count", recorder_get_drop_count());
    cJSON_AddNumberToObject(root, "rec_total_bytes", (double)recorder_get_total_bytes());
    cJSON_AddNumberToObject(root, "ch1_drop_count",
                            recorder_get_channel_drop_count(RECORDER_CHANNEL_1));
    cJSON_AddNumberToObject(root, "ch2_drop_count",
                            recorder_get_channel_drop_count(RECORDER_CHANNEL_2));
    cJSON_AddNumberToObject(root, "ch1_total_bytes",
                            (double)recorder_get_channel_total_bytes(RECORDER_CHANNEL_1));
    cJSON_AddNumberToObject(root, "ch2_total_bytes",
                            (double)recorder_get_channel_total_bytes(RECORDER_CHANNEL_2));
    cJSON_AddBoolToObject(root, "ch1_file_open",
                          recorder_is_channel_file_open(RECORDER_CHANNEL_1));
    cJSON_AddBoolToObject(root, "ch2_file_open",
                          recorder_is_channel_file_open(RECORDER_CHANNEL_2));
    cJSON_AddBoolToObject(root, "uart_ch1_ready",
                          user_uart_channel_is_ready(USER_UART_CHANNEL_1));
    cJSON_AddBoolToObject(root, "uart_ch2_ready",
                          user_uart_channel_is_ready(USER_UART_CHANNEL_2));
    cJSON_AddNumberToObject(root, "uart_ch1_rx_bytes",
                            (double)user_uart_get_rx_bytes(USER_UART_CHANNEL_1));
    cJSON_AddNumberToObject(root, "uart_ch2_rx_bytes",
                            (double)user_uart_get_rx_bytes(USER_UART_CHANNEL_2));
    cJSON_AddNumberToObject(root, "uart_ch1_tx_bytes",
                            (double)user_uart_get_tx_bytes(USER_UART_CHANNEL_1));
    cJSON_AddNumberToObject(root, "uart_ch2_tx_bytes",
                            (double)user_uart_get_tx_bytes(USER_UART_CHANNEL_2));
    cJSON_AddBoolToObject(root, "route_enabled", route_stats.enabled);
    cJSON_AddNumberToObject(root, "route_ch1_to_ch2_bytes",
                            (double)route_stats.ch1_to_ch2_bytes);
    cJSON_AddNumberToObject(root, "route_ch2_to_ch1_bytes",
                            (double)route_stats.ch2_to_ch1_bytes);
    cJSON_AddNumberToObject(root, "route_ch1_to_ch2_drop_bytes",
                            (double)route_stats.ch1_to_ch2_drop_bytes);
    cJSON_AddNumberToObject(root, "route_ch2_to_ch1_drop_bytes",
                            (double)route_stats.ch2_to_ch1_drop_bytes);

    cJSON_AddNumberToObject(root, "uptime",
        (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "free_heap",
        (double)esp_get_free_heap_size());
    cJSON_AddStringToObject(root, "fw_version", FW_VERSION);
    cJSON_AddStringToObject(root, "usb_mode", "CDC+MSC");

    /* 当前录制文件名 */
    const char *ch1_file = recorder_get_filename(RECORDER_CHANNEL_1);
    const char *ch2_file = recorder_get_filename(RECORDER_CHANNEL_2);
    char current_file[192] = "-";
    if (ch1_file && ch2_file) {
        snprintf(current_file, sizeof(current_file), "%s / %s", ch1_file, ch2_file);
    } else if (ch1_file) {
        snprintf(current_file, sizeof(current_file), "%s", ch1_file);
    } else if (ch2_file) {
        snprintf(current_file, sizeof(current_file), "%s", ch2_file);
    }
    cJSON_AddStringToObject(root, "current_file", current_file);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, body, strlen(body));
    cJSON_free(body);
    return err;
}

/* GET /api/wifi/scan */
static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100, .scan_time.active.max = 300,
    };

    esp_err_t scan_err = esp_wifi_scan_start(&scan_cfg, true);
    if (scan_err != ESP_OK) {
        return send_json_err(req, "scan_failed");
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        return send_json(req, "{\"ok\":true,\"networks\":[]}");
    }

    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (records == NULL) return httpd_resp_send_500(req);
    esp_wifi_scan_get_ap_records(&ap_count, records);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (uint16_t i = 0; i < ap_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(item, "auth", records[i].authmode);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    free(records);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

/* POST /api/wifi/disconnect */
static esp_err_t wifi_disconnect_handler(httpd_req_t *req)
{
    wifi_disconnect_sta();
    config_manager_t *mgr = config_manager_instance();
    mgr->cfg.wifi.enable_sta = false;
    mgr->cfg.wifi.sta_ssid[0] = '\0';
    config_manager_save(mgr, "/sdcard/config.json");
    return send_json_ok(req);
}

/* GET /api/wifi/status */
static esp_err_t wifi_status_handler(httpd_req_t *req)
{
    const wifi_state_t *ws = wifi_get_state();
    const device_config_t *cfg = config_manager_get(config_manager_instance());

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "sta_connected", ws->sta_connected);
    cJSON_AddStringToObject(root, "sta_ssid",
        cfg->wifi.sta_ssid[0] ? cfg->wifi.sta_ssid : "-");
    char sta_ip[16] = "-";
    wifi_get_sta_ip(sta_ip, sizeof(sta_ip));
    cJSON_AddStringToObject(root, "sta_ip", sta_ip);
    cJSON_AddNumberToObject(root, "rssi", wifi_get_sta_rssi());

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

/* POST /api/wifi */
static esp_err_t wifi_set_handler(httpd_req_t *req)
{
    char buf[256];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int got = httpd_req_recv(req, buf + total, sizeof(buf) - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    if (total <= 0) return httpd_resp_send_500(req);
    buf[total] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) return httpd_resp_send_500(req);
    cJSON *jssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *jpass = cJSON_GetObjectItem(root, "pass");
    cJSON *jauth = cJSON_GetObjectItem(root, "auth");
    if (!cJSON_IsString(jssid) || jssid->valuestring == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }
    const char *ssid = jssid->valuestring;
    const char *pass = (cJSON_IsString(jpass) && jpass->valuestring) ? jpass->valuestring : "";
    int auth = (cJSON_IsNumber(jauth)) ? (int)jauth->valueint : 0;

    if (strlen(ssid) > 31 || strlen(pass) > 63 || !valid_sta_authmode(auth) ||
        (auth != WIFI_AUTH_OPEN && ssid[0] != '\0' && strlen(pass) < 8)) {
        cJSON_Delete(root);
        return send_json_err(req, "invalid_wifi_config");
    }

    config_manager_t *mgr = config_manager_instance();
    device_config_t old_cfg = mgr->cfg;

    strncpy(mgr->cfg.wifi.sta_ssid, ssid, sizeof(mgr->cfg.wifi.sta_ssid) - 1);
    mgr->cfg.wifi.sta_ssid[sizeof(mgr->cfg.wifi.sta_ssid) - 1] = '\0';
    strncpy(mgr->cfg.wifi.sta_pass, pass, sizeof(mgr->cfg.wifi.sta_pass) - 1);
    mgr->cfg.wifi.sta_pass[sizeof(mgr->cfg.wifi.sta_pass) - 1] = '\0';
    mgr->cfg.wifi.enable_sta = (ssid[0] != '\0');
    mgr->cfg.wifi.sta_authmode = auth;

    esp_err_t save_err = config_manager_save(mgr, "/sdcard/config.json");
    cJSON_Delete(root);
    if (save_err != ESP_OK) {
        mgr->cfg = old_cfg;
        return send_json_err(req, "save_failed");
    }

    if (mgr->cfg.wifi.sta_ssid[0] != '\0') {
        wifi_connect_sta(mgr->cfg.wifi.sta_ssid, mgr->cfg.wifi.sta_pass,
                         mgr->cfg.wifi.sta_authmode);
    } else {
        wifi_disconnect_sta();
    }
    return send_json_ok(req);
}

/* POST /api/ap */
static bool valid_ipv4(const char *s)
{
    if (s == NULL || *s == '\0') return true;
    int a, b, c, d;
    if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
    if (a == 127 || (a >= 224 && a <= 239)) return false;
    return true;
}

static esp_err_t ap_set_handler(httpd_req_t *req)
{
    char body[256];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int got = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    if (total <= 0) return httpd_resp_send_500(req);
    body[total] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return send_json(req, "{\"ok\":false}");
    }

    cJSON *jssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *jpass = cJSON_GetObjectItem(root, "pass");
    cJSON *jip = cJSON_GetObjectItem(root, "ip");
    const char *ssid = cJSON_IsString(jssid) ? jssid->valuestring : NULL;
    const char *pass = cJSON_IsString(jpass) ? jpass->valuestring : NULL;
    const char *ip = cJSON_IsString(jip) ? jip->valuestring : NULL;

    if ((ssid && strlen(ssid) > 31) || (pass && strlen(pass) > 63) ||
        (pass && pass[0] != '\0' && strlen(pass) < 8) || !valid_ipv4(ip)) {
        cJSON_Delete(root);
        return send_json(req, "{\"ok\":false}");
    }

    config_manager_t *mgr = config_manager_instance();
    device_config_t old_cfg = mgr->cfg;
    if (ssid != NULL) {
        strncpy(mgr->cfg.wifi.ap_ssid, ssid, sizeof(mgr->cfg.wifi.ap_ssid) - 1);
        mgr->cfg.wifi.ap_ssid[sizeof(mgr->cfg.wifi.ap_ssid) - 1] = '\0';
    }
    if (pass != NULL) {
        strncpy(mgr->cfg.wifi.ap_pass, pass, sizeof(mgr->cfg.wifi.ap_pass) - 1);
        mgr->cfg.wifi.ap_pass[sizeof(mgr->cfg.wifi.ap_pass) - 1] = '\0';
    }
    if (ip != NULL && ip[0] != '\0') {
        strncpy(mgr->cfg.wifi.ap_ip, ip, sizeof(mgr->cfg.wifi.ap_ip) - 1);
        mgr->cfg.wifi.ap_ip[sizeof(mgr->cfg.wifi.ap_ip) - 1] = '\0';
    }

    esp_err_t save_err = config_manager_save(mgr, "/sdcard/config.json");
    cJSON_Delete(root);
    if (save_err != ESP_OK) {
        mgr->cfg = old_cfg;
        return send_json_err(req, "save_failed");
    }

    wifi_apply_ap(mgr->cfg.wifi.ap_ssid, mgr->cfg.wifi.ap_pass, mgr->cfg.wifi.ap_ip);
    return send_json_ok(req);
}

/* GET /api/files */
static esp_err_t files_list_handler(httpd_req_t *req)
{
    DIR *dir = opendir("/sdcard");
    if (dir == NULL) {
        return send_json(req, "{\"files\":[]}");
    }

    char *buf = (char *)malloc(4096);
    if (buf == NULL) { closedir(dir); return httpd_resp_send_500(req); }
    size_t off = 0;
    off += (size_t)snprintf(buf + off, 4096 - off, "{\"files\":[");
    bool first = true;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "REC_", 4) != 0) continue;
        if (!strstr(ent->d_name, ".bin")) continue;
        if (!is_safe_filename(ent->d_name)) continue;

        char path[300];
        snprintf(path, sizeof(path), "/sdcard/%s", ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (!first) buf[off++] = ',';
        first = false;
        int n = snprintf(buf + off, 4096 - off,
                         "{\"name\":\"%s\",\"size\":%ld}",
                         ent->d_name, (long)st.st_size);
        if (n < 0 || (size_t)n >= 4096 - off) break;
        off += (size_t)n;
    }
    closedir(dir);

    if (off + 2 < 4096) { buf[off++] = ']'; buf[off++] = '}'; buf[off] = '\0'; }
    else { buf[4090] = '\0'; off = 4090; }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, buf, off);
    free(buf);
    return err;
}

/* GET /api/file?name=xxx */
static esp_err_t file_download_handler(httpd_req_t *req)
{
    char name[96];
    if (!get_query_value(req, "name", name, sizeof(name)) || !is_safe_filename(name))
        return httpd_resp_send_500(req);

    char path[128];
    snprintf(path, sizeof(path), "/sdcard/%s", name);
    FILE *f = fopen(path, "r");
    if (f == NULL) return httpd_resp_send_404(req);

    httpd_resp_set_type(req, "application/octet-stream");
    char disp[160];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char chunk[1024];
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) { err = ESP_FAIL; break; }
    }
    fclose(f);
    if (err != ESP_OK) return err;
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* POST /api/file/delete?name=xxx */
static esp_err_t file_delete_handler(httpd_req_t *req)
{
    char name[96];
    if (!get_query_value(req, "name", name, sizeof(name)) || !is_safe_filename(name)) {
        return send_json(req, "{\"ok\":false}");
    }
    char path[128];
    snprintf(path, sizeof(path), "/sdcard/%s", name);
    int rc = unlink(path);
    return send_json(req, rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t recorder_start_handler(httpd_req_t *req)
{
    esp_err_t err = recorder_start();
    return send_json(req, err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t recorder_stop_handler(httpd_req_t *req)
{
    esp_err_t err = recorder_stop();
    return send_json(req, err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* GET /api/stream?since=N */
static esp_err_t stream_fetch_handler(httpd_req_t *req)
{
    char since_str[16] = {0};
    uint32_t since = 0;
    if (get_query_value(req, "since", since_str, sizeof(since_str)))
        since = (uint32_t)strtoul(since_str, NULL, 10);

    recorder_live_item_t items[32];
    uint32_t max_seq = since;
    size_t got = recorder_live_fetch(since, items, 32, &max_seq);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "items");
    for (size_t i = 0; i < got; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "seq", items[i].seq);
        cJSON_AddNumberToObject(o, "ts", items[i].ts_ms);
        cJSON_AddStringToObject(o, "line", items[i].line);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(root, "max_seq", max_seq);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, body, strlen(body));
    cJSON_free(body);
    return err;
}

static esp_err_t stream_clear_handler(httpd_req_t *req)
{
    recorder_live_clear();
    return send_json_ok(req);
}

/* POST /api/send {"channel":1,"hex":"01 03"} */
static int hex_nibble(char c, int *ok)
{
    if (c >= '0' && c <= '9') { *ok = 1; return c - '0'; }
    if (c >= 'a' && c <= 'f') { *ok = 1; return 10 + c - 'a'; }
    if (c >= 'A' && c <= 'F') { *ok = 1; return 10 + c - 'A'; }
    *ok = 0; return 0;
}

static esp_err_t send_tx_handler(httpd_req_t *req)
{
    char body[1024];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int got = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    if (total <= 0) return httpd_resp_send_500(req);
    body[total] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) { return send_json(req, "{\"ok\":false}"); }
    cJSON *jhex = cJSON_GetObjectItem(root, "hex");
    if (!cJSON_IsString(jhex) || jhex->valuestring == NULL) {
        cJSON_Delete(root);
        return send_json(req, "{\"ok\":false}");
    }
    cJSON *jchannel = cJSON_GetObjectItem(root, "channel");
    int channel = cJSON_IsNumber(jchannel) ? jchannel->valueint : RECORDER_CHANNEL_1;
    if (channel < RECORDER_CHANNEL_1 || channel > RECORDER_CHANNEL_2) {
        cJSON_Delete(root);
        return send_json(req, "{\"ok\":false,\"error\":\"bad channel\"}");
    }
    const char *hex = jhex->valuestring;
    uint8_t buf[256];
    size_t out = 0;
    int hi = -1;
    for (const char *p = hex; *p && out < sizeof(buf); p++) {
        int v, ok;
        v = hex_nibble(*p, &ok);
        if (!ok) continue;
        if (hi < 0) hi = v;
        else { buf[out++] = (uint8_t)((hi << 4) | v); hi = -1; }
    }
    int sent = user_uart_write_channel((uint8_t)channel, buf, out);
    cJSON_Delete(root);

    char resp[80];
    int n = snprintf(resp, sizeof(resp),
                     "{\"ok\":%s,\"channel\":%d,\"sent\":%d}",
                     sent > 0 ? "true" : "false", channel, sent);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, resp, n);
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    send_json_ok(req);
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "reboot requested via web");
    esp_restart();
    return ESP_OK;
}

/* POST /api/config/stream_gap {"value":100} */
static esp_err_t stream_gap_set_handler(httpd_req_t *req)
{
    char body[64];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int got = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    if (total <= 0) { return send_json(req, "{\"ok\":false}"); }
    body[total] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) { return send_json(req, "{\"ok\":false}"); }
    cJSON *jv = cJSON_GetObjectItem(root, "value");
    if (!cJSON_IsNumber(jv)) { cJSON_Delete(root); return send_json(req, "{\"ok\":false}"); }
    int value = (int)jv->valueint;
    cJSON_Delete(root);

    config_manager_t *mgr = config_manager_instance();
    esp_err_t err = config_manager_set_stream_gap_ms(mgr, value, "/sdcard/config.json");
    if (err != ESP_OK) return send_json(req, "{\"ok\":false}");
    ESP_LOGI(TAG, "stream_gap_ms set to %d", mgr->cfg.recorder.stream_gap_ms);
    return send_json_ok(req);
}

/* POST /api/config/uart {"baudrate":115200,"databits":8,"stopbits":1,"parity":0} */
static esp_err_t uart_config_handler(httpd_req_t *req)
{
    char body[128];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int got = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    if (total <= 0) { return send_json(req, "{\"ok\":false}"); }
    body[total] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) { return send_json(req, "{\"ok\":false}"); }

    cJSON *jbaud = cJSON_GetObjectItem(root, "baudrate");
    if (!cJSON_IsNumber(jbaud) || !config_manager_valid_baudrate(jbaud->valueint)) {
        cJSON_Delete(root);
        return send_json_err(req, "invalid_baudrate");
    }

    int baudrate = jbaud->valueint;
    cJSON *jbits = cJSON_GetObjectItem(root, "databits");
    int databits = cJSON_IsNumber(jbits) ? jbits->valueint : 8;
    cJSON *jstop = cJSON_GetObjectItem(root, "stopbits");
    int stopbits = cJSON_IsNumber(jstop) ? jstop->valueint : 1;
    cJSON *jparity = cJSON_GetObjectItem(root, "parity");
    int parity = cJSON_IsNumber(jparity) ? jparity->valueint : 0;

    config_manager_t *mgr = config_manager_instance();
    device_config_t old_cfg = mgr->cfg;
    cJSON_Delete(root);

    /* 更新两路 UART */
    esp_err_t err = user_uart_set_channel_baudrate(USER_UART_CHANNEL_1, baudrate);
    if (err == ESP_OK) err = user_uart_set_channel_baudrate(USER_UART_CHANNEL_2, baudrate);
    if (err != ESP_OK) {
        user_uart_set_channel_baudrate(USER_UART_CHANNEL_1, old_cfg.uart.baudrate);
        user_uart_set_channel_baudrate(USER_UART_CHANNEL_2, old_cfg.uart.baudrate);
        return send_json_err(req, "uart_apply_failed");
    }

    mgr->cfg.uart.baudrate = baudrate;
    mgr->cfg.uart.databits = databits;
    mgr->cfg.uart.stopbits = stopbits;
    mgr->cfg.uart.parity = parity;

    esp_err_t save_err = config_manager_save(mgr, "/sdcard/config.json");
    if (save_err != ESP_OK) {
        mgr->cfg = old_cfg;
        user_uart_set_channel_baudrate(USER_UART_CHANNEL_1, old_cfg.uart.baudrate);
        user_uart_set_channel_baudrate(USER_UART_CHANNEL_2, old_cfg.uart.baudrate);
        return send_json_err(req, "save_failed");
    }
    return send_json_ok(req);
}

/* POST /api/config/router {"enabled":true} */
static esp_err_t router_config_handler(httpd_req_t *req)
{
    char body[64];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int got = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    if (total <= 0) { return send_json(req, "{\"ok\":false}"); }
    body[total] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) { return send_json(req, "{\"ok\":false}"); }
    cJSON *item = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsBool(item)) { cJSON_Delete(root); return send_json_err(req, "invalid_router_setting"); }
    bool enabled = cJSON_IsTrue(item);
    cJSON_Delete(root);

    esp_err_t err = config_manager_set_router_enabled(config_manager_instance(),
                                                       enabled, "/sdcard/config.json");
    if (err != ESP_OK) return send_json_err(req, "save_failed");
    data_router_set_enabled(enabled);
    return send_json_ok(req);
}

/* POST /api/config/recorder {"auto_start":true} */
static esp_err_t recorder_config_handler(httpd_req_t *req)
{
    char body[64];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int got = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    if (total <= 0) { return send_json(req, "{\"ok\":false}"); }
    body[total] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) { return send_json(req, "{\"ok\":false}"); }
    cJSON *item = cJSON_GetObjectItem(root, "auto_start");
    if (!cJSON_IsBool(item)) { cJSON_Delete(root); return send_json_err(req, "invalid_recorder_setting"); }
    bool enabled = cJSON_IsTrue(item);
    cJSON_Delete(root);

    esp_err_t err = config_manager_set_recorder_auto_start(
        config_manager_instance(), enabled, "/sdcard/config.json");
    if (err != ESP_OK) return send_json_err(req, "save_failed");
    return send_json_ok(req);
}

/* ---------------- 启动 / 停止 ---------------- */

esp_err_t web_server_start(void)
{
    if (s_server != NULL) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WEB_PORT;
    cfg.max_uri_handlers = 24;
    cfg.stack_size = 16384;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",                       .method = HTTP_GET,  .handler = index_handler },
        { .uri = "/app.css",                .method = HTTP_GET,  .handler = css_handler },
        { .uri = "/app.js",                 .method = HTTP_GET,  .handler = js_handler },
        { .uri = "/api/status",             .method = HTTP_GET,  .handler = status_handler },
        { .uri = "/api/wifi",               .method = HTTP_POST, .handler = wifi_set_handler },
        { .uri = "/api/wifi/scan",          .method = HTTP_GET,  .handler = wifi_scan_handler },
        { .uri = "/api/wifi/disconnect",    .method = HTTP_POST, .handler = wifi_disconnect_handler },
        { .uri = "/api/wifi/status",        .method = HTTP_GET,  .handler = wifi_status_handler },
        { .uri = "/api/ap",                 .method = HTTP_POST, .handler = ap_set_handler },
        { .uri = "/api/files",              .method = HTTP_GET,  .handler = files_list_handler },
        { .uri = "/api/file",               .method = HTTP_GET,  .handler = file_download_handler },
        { .uri = "/api/file/delete",        .method = HTTP_POST, .handler = file_delete_handler },
        { .uri = "/api/recorder/start",     .method = HTTP_POST, .handler = recorder_start_handler },
        { .uri = "/api/recorder/stop",      .method = HTTP_POST, .handler = recorder_stop_handler },
        { .uri = "/api/stream",             .method = HTTP_GET,  .handler = stream_fetch_handler },
        { .uri = "/api/stream/clear",       .method = HTTP_POST, .handler = stream_clear_handler },
        { .uri = "/api/send",               .method = HTTP_POST, .handler = send_tx_handler },
        { .uri = "/api/reboot",             .method = HTTP_POST, .handler = reboot_handler },
        { .uri = "/api/config/stream_gap",  .method = HTTP_POST, .handler = stream_gap_set_handler },
        { .uri = "/api/config/uart",        .method = HTTP_POST, .handler = uart_config_handler },
        { .uri = "/api/config/router",      .method = HTTP_POST, .handler = router_config_handler },
        { .uri = "/api/config/recorder",    .method = HTTP_POST, .handler = recorder_config_handler },
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    ESP_LOGI(TAG, "web server started on port %d", WEB_PORT);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_server == NULL) return ESP_OK;
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}
