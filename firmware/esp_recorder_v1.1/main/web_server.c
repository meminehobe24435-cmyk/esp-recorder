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
#include "cJSON.h"

#include "wifi_config.h"
#include "uart_driver.h"
#include "data_recorder.h"
#include "data_router.h"
#include "config_manager.h"
#include "sd_card.h"

#define TAG "web_server"
#define WEB_PORT 80

static httpd_handle_t s_server = NULL;

/* ---------------- 工具 ---------------- */

/* 从 query string 中取 key（例如 ?name=REC_x.bin） */
static bool get_query_value(httpd_req_t *req, const char *key,
                            char *out, size_t out_size)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0) {
        return false;
    }
    char *qbuf = (char *)malloc(qlen + 1);
    if (qbuf == NULL) {
        return false;
    }
    if (httpd_req_get_url_query_str(req, qbuf, qlen + 1) != ESP_OK) {
        free(qbuf);
        return false;
    }
    esp_err_t err = httpd_query_key_value(qbuf, key, out, out_size);
    free(qbuf);
    return err == ESP_OK;
}

/* 防止路径穿越：拒绝包含 / \ .. 的文件名 */
static bool is_safe_filename(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            return false;
        }
    }
    if (strstr(name, "..") != NULL) {
        return false;
    }
    return true;
}

/* ---------------- Handlers ---------------- */

static esp_err_t index_handler(httpd_req_t *req)
{
    /* index.html 通过 CMake EMBED_FILES 链接进固件；
     * _binary_index_html_start/_end 由 ld 段 .esp.bin_files.index.html 暴露 */
    extern const uint8_t _binary_index_html_start[];
    extern const uint8_t _binary_index_html_end[];
    const size_t len = (size_t)(_binary_index_html_end - _binary_index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)_binary_index_html_start, len);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    const wifi_state_t *ws = wifi_get_state();
    const sd_card_state_t *sd = sd_card_get_state();
    const device_config_t *cfg = config_manager_get(config_manager_instance());
    data_router_stats_t route_stats;
    data_router_get_stats(&route_stats);

    /* 用 cJSON 构造，避免 snprintf %s 拼接时遇到 SSID 含 " \ 控制字符导致 JSON 损坏。
     * cJSON_Print 自动按 UTF-8 输出（客户端 charset=utf-8 即可正确解码）。 */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ap", ws->ap_started);
    /* 显示用户保存的 SSID；未配置则回落到 - */
    cJSON_AddStringToObject(root, "ap_ssid",
        cfg->wifi.ap_ssid[0] ? cfg->wifi.ap_ssid : "-");
    cJSON_AddStringToObject(root, "ap_ip",
        cfg->wifi.ap_ip[0] ? cfg->wifi.ap_ip : "-");
    cJSON_AddBoolToObject(root, "sta_connected", ws->sta_connected);
    /* STA 显示用 config 里保存的目标 SSID，而不是 ws->sta_ssid（运行时可能未更新） */
    cJSON_AddStringToObject(root, "sta_ssid",
        cfg->wifi.sta_ssid[0] ? cfg->wifi.sta_ssid : "-");
    cJSON_AddBoolToObject(root, "sd", sd->mounted);
    cJSON_AddBoolToObject(root, "recording", recorder_is_running());
    cJSON_AddBoolToObject(root, "file_open", recorder_is_file_open());
    cJSON_AddNumberToObject(root, "stream_gap_ms", cfg->recorder.stream_gap_ms);
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

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, body, strlen(body));
    cJSON_free(body);
    return err;
}

/* 把当前 STA 凭证写入 config.json 并触发重连。
 * 其它字段（ap_pass、baudrate、auto_start 等）保留原值。 */
static esp_err_t wifi_set_handler(httpd_req_t *req)
{
    char buf[256];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int got = httpd_req_recv(req, buf + total, sizeof(buf) - 1 - total);
        if (got <= 0) {
            break;
        }
        total += got;
    }
    if (total <= 0) {
        return httpd_resp_send_500(req);
    }
    buf[total] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
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

    /* 加载现有配置（不存在则用默认值），只覆盖 STA 字段 */
    config_manager_t *mgr = config_manager_instance();
    if (config_manager_load(mgr, "/sdcard/config.json") != ESP_OK) {
        config_manager_reset_defaults(mgr);
    }

    strncpy(mgr->cfg.wifi.sta_ssid, ssid, sizeof(mgr->cfg.wifi.sta_ssid) - 1);
    mgr->cfg.wifi.sta_ssid[sizeof(mgr->cfg.wifi.sta_ssid) - 1] = '\0';
    strncpy(mgr->cfg.wifi.sta_pass, pass, sizeof(mgr->cfg.wifi.sta_pass) - 1);
    mgr->cfg.wifi.sta_pass[sizeof(mgr->cfg.wifi.sta_pass) - 1] = '\0';
    mgr->cfg.wifi.enable_sta = (ssid[0] != '\0');
    mgr->cfg.wifi.sta_authmode = auth;

    esp_err_t save_err = config_manager_save(mgr, "/sdcard/config.json");
    cJSON_Delete(root);

    if (save_err != ESP_OK) {
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"ok\":false}", 11);
    }

    /* 应用：连接新 STA 或断开 */
    if (ssid[0] != '\0') {
        wifi_connect_sta(ssid, pass, mgr->cfg.wifi.sta_authmode);
    } else {
        wifi_disconnect_sta();
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, "{\"ok\":true}", 10);
}

/* 修改 AP 配置（SSID/密码/IP），写回 config.json 后用 wifi_apply_ap 重启热点。 */
static bool valid_ipv4(const char *s)
{
    if (s == NULL || *s == '\0') return true; /* 空 = 不改 */
    int dots = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '.') { dots++; continue; }
        if (*p < '0' || *p > '9') return false;
    }
    if (dots != 3) return false;
    int a, b, c, d;
    if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
    if (a == 127 || (a >= 224 && a <= 239)) return false; /* loopback / multicast */
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
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"ok\":false}", 11);
    }

    const char *ssid = cJSON_GetObjectItem(root, "ssid") ? cJSON_GetObjectItem(root, "ssid")->valuestring : NULL;
    const char *pass = cJSON_GetObjectItem(root, "pass") ? cJSON_GetObjectItem(root, "pass")->valuestring : NULL;
    const char *ip   = cJSON_GetObjectItem(root, "ip")   ? cJSON_GetObjectItem(root, "ip")->valuestring   : NULL;

    if ((ssid && strlen(ssid) > 31) || (pass && strlen(pass) > 63)) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"ok\":false}", 11);
    }
    if (!valid_ipv4(ip)) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"ok\":false}", 11);
    }

    config_manager_t *mgr = config_manager_instance();
    if (config_manager_load(mgr, "/sdcard/config.json") != ESP_OK) {
        config_manager_reset_defaults(mgr);
    }
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
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"ok\":false}", 11);
    }

    /* 应用：让 AP 用新 SSID/密码/IP 立刻重启 */
    wifi_apply_ap(mgr->cfg.wifi.ap_ssid, mgr->cfg.wifi.ap_pass, mgr->cfg.wifi.ap_ip);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, "{\"ok\":true}", 10);
}

static esp_err_t files_list_handler(httpd_req_t *req)
{
    DIR *dir = opendir("/sdcard");
    if (dir == NULL) {
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"files\":[]}", 11);
    }

    /* 直接堆上拼 JSON；记录文件数量有限，简单做法即可 */
    char *buf = (char *)malloc(4096);
    if (buf == NULL) {
        closedir(dir);
        return httpd_resp_send_500(req);
    }
    size_t off = 0;
    off += (size_t)snprintf(buf + off, 4096 - off, "{\"files\":[");
    bool first = true;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "REC_", 4) != 0) {
            continue;
        }
        if (!strstr(ent->d_name, ".bin")) {
            continue;
        }
        if (!is_safe_filename(ent->d_name)) {
            continue;
        }

        char path[300];
        snprintf(path, sizeof(path), "/sdcard/%s", ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }

        if (!first) {
            buf[off++] = ',';
        }
        first = false;
        int n = snprintf(buf + off, 4096 - off,
                         "{\"name\":\"%s\",\"size\":%ld}",
                         ent->d_name, (long)st.st_size);
        if (n < 0 || (size_t)n >= 4096 - off) {
            break;
        }
        off += (size_t)n;
    }
    closedir(dir);

    if (off + 2 < 4096) {
        buf[off++] = ']';
        buf[off++] = '}';
        buf[off] = '\0';
    } else {
        buf[4090] = '\0';
        off = 4090;
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, buf, off);
    free(buf);
    return err;
}

static esp_err_t file_download_handler(httpd_req_t *req)
{
    char name[96];
    if (!get_query_value(req, "name", name, sizeof(name)) || !is_safe_filename(name)) {
        return httpd_resp_send_500(req);
    }

    char path[128];
    snprintf(path, sizeof(path), "/sdcard/%s", name);

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return httpd_resp_send_404(req);
    }

    httpd_resp_set_type(req, "application/octet-stream");
    /* 触发浏览器另存为 */
    char disp[160];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char chunk[1024];
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }
    fclose(f);
    if (err != ESP_OK) {
        return err;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t recorder_start_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = recorder_start();
    return httpd_resp_send(req, err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false}",
                           err == ESP_OK ? 10 : 11);
}

static esp_err_t recorder_stop_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = recorder_stop();
    return httpd_resp_send(req, err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false}",
                           err == ESP_OK ? 10 : 11);
}

/* GET /api/stream?since=N → JSON {"items":[...], "max_seq":M} */
static esp_err_t stream_fetch_handler(httpd_req_t *req)
{
    char since_str[16] = {0};
    uint32_t since = 0;
    if (get_query_value(req, "since", since_str, sizeof(since_str))) {
        since = (uint32_t)strtoul(since_str, NULL, 10);
    }

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
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, "{\"ok\":true}", 10);
}

/* POST /api/send {"channel":1,"hex":"01 03"} -> 写入指定用户串口。 */
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
    if (root == NULL) {
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"ok\":false}", 11);
    }
    cJSON *jhex = cJSON_GetObjectItem(root, "hex");
    if (!cJSON_IsString(jhex) || jhex->valuestring == NULL) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"ok\":false}", 11);
    }
    cJSON *jchannel = cJSON_GetObjectItem(root, "channel");
    int channel = cJSON_IsNumber(jchannel) ? jchannel->valueint : RECORDER_CHANNEL_1;
    if (channel < RECORDER_CHANNEL_1 || channel > RECORDER_CHANNEL_2) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad channel\"}",
                               HTTPD_RESP_USE_STRLEN);
    }
    const char *hex = jhex->valuestring;
    uint8_t buf[256];
    size_t out = 0;
    int hi = -1;
    for (const char *p = hex; *p && out < sizeof(buf); p++) {
        int v, ok;
        (void)hi;
        v = hex_nibble(*p, &ok);
        if (!ok) continue; /* 跳过空格、':'、'-' 等分隔符 */
        if (hi < 0) {
            hi = v;
        } else {
            buf[out++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    int sent = user_uart_write_channel((uint8_t)channel, buf, out);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    char resp[80];
    int n = snprintf(resp, sizeof(resp),
                     "{\"ok\":%s,\"channel\":%d,\"sent\":%d}",
                     sent > 0 ? "true" : "false", channel, sent);
    return httpd_resp_send(req, resp, n);
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, "{\"ok\":true}", 10);
    /* 给响应一点时间飞出去再重启 */
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "reboot requested via web");
    esp_restart();
    return err;
}

/* POST /api/config/stream_gap {"value": 100} */
static esp_err_t stream_gap_set_handler(httpd_req_t *req)
{
    char body[64];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int got = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    if (total <= 0) {
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"ok\":false}", 11);
    }
    body[total] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"ok\":false}", 11);
    }
    cJSON *jv = cJSON_GetObjectItem(root, "value");
    if (!cJSON_IsNumber(jv)) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"ok\":false}", 11);
    }
    int value = (int)jv->valueint;
    cJSON_Delete(root);

    config_manager_t *mgr = config_manager_instance();
    esp_err_t err = config_manager_set_stream_gap_ms(mgr, value, "/sdcard/config.json");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    if (err != ESP_OK) {
        return httpd_resp_send(req, "{\"ok\":false}", 11);
    }
    ESP_LOGI(TAG, "stream_gap_ms set to %d (clamped from %d)",
             mgr->cfg.recorder.stream_gap_ms, value);
    return httpd_resp_send(req, "{\"ok\":true}", 10);
}

static esp_err_t file_delete_handler(httpd_req_t *req)
{
    char name[96];
    if (!get_query_value(req, "name", name, sizeof(name)) || !is_safe_filename(name)) {
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"ok\":false}", 11);
    }
    char path[128];
    snprintf(path, sizeof(path), "/sdcard/%s", name);
    /* 路径已被 is_safe_filename 校验过；unlink 返回 0 即成功 */
    int rc = unlink(path);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}",
                           rc == 0 ? 10 : 11);
}

/* ---------------- 启动 / 停止 ---------------- */

esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WEB_PORT;
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 16384;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",                       .method = HTTP_GET,  .handler = index_handler },
        { .uri = "/api/status",             .method = HTTP_GET,  .handler = status_handler },
        { .uri = "/api/wifi",               .method = HTTP_POST, .handler = wifi_set_handler },
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
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    ESP_LOGI(TAG, "web server started on port %d", WEB_PORT);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}
