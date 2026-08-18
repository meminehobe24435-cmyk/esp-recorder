#include "wifi_config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "lwip/inet.h"

#define TAG "wifi_config"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define STA_MAX_RETRY 3
#define STA_CONNECT_TIMEOUT_MS 30000
#define STA_RETRY_INTERVAL_MS 5000

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static wifi_state_t s_state = {0};
static int s_retry_count = 0;

static char *wifi_ap_default_ssid(char *out, size_t out_size)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_size, "%s%02X%02X",
             AP_SSID_PREFIX, mac[4], mac[5]);
    return out;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_state.sta_connected = false;
        if (s_retry_count < STA_MAX_RETRY)
        {
            s_retry_count++;
            ESP_LOGI(TAG, "STA retry %d/%d", s_retry_count, STA_MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(STA_RETRY_INTERVAL_MS));
            esp_wifi_connect();
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "STA failed after %d retries, staying in AP+STA mode", STA_MAX_RETRY);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        s_retry_count = 0;
        s_state.sta_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "STA got ip: " IPSTR, IP2STR(&event->ip_info.ip));
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START)
    {
        s_state.ap_started = true;
        ESP_LOGI(TAG, "AP started");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP)
    {
        s_state.ap_started = false;
        ESP_LOGI(TAG, "AP stopped");
    }
}

esp_err_t wifi_config_init(void)
{
    if (s_wifi_event_group != NULL)
    {
        return ESP_OK;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    s_state.mode = REC_WIFI_MODE_AP_STA;
    ESP_LOGI(TAG, "WiFi init done");
    return ESP_OK;
}

esp_err_t wifi_start_ap(void)
{
    /* 兼容旧调用：boot 阶段按默认 IP/SSID/密码启动。
     * 真正想用 config 里的 AP 参数应调 wifi_apply_ap() */
    return wifi_apply_ap(NULL, NULL, NULL);
}

esp_err_t wifi_apply_ap(const char *ap_ssid, const char *ap_pass, const char *ap_ip)
{
    if (s_ap_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* IP：用入参或默认；空串回落到默认 */
    const char *ip_str = (ap_ip != NULL && ap_ip[0] != '\0') ? ap_ip : AP_IP;

    esp_netif_ip_info_t ip_info;
    ip_info.ip.addr      = ipaddr_addr(ip_str);
    ip_info.gw.addr      = ipaddr_addr(ip_str);    /* 网关 = 自己 */
    ip_info.netmask.addr = ipaddr_addr(AP_NETMASK);

    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip_info);
    esp_netif_dhcps_start(s_ap_netif);

    /* SSID：入参优先，否则按 MAC 默认 */
    char default_ssid[32];
    wifi_ap_default_ssid(default_ssid, sizeof(default_ssid));
    const char *ssid_str = (ap_ssid != NULL && ap_ssid[0] != '\0') ? ap_ssid : default_ssid;

    /* 密码：空串视为开放 */
    const char *pass_str = (ap_pass != NULL) ? ap_pass : AP_DEFAULT_PASS;
    bool is_open = (pass_str[0] == '\0');

    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = 0,
            .channel = 1,
            .max_connection = 4,
            .authmode = is_open ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK,
            .password = {0},
            .ssid = {0},
        },
    };
    strncpy((char *)ap_config.ap.ssid, ssid_str, sizeof(ap_config.ap.ssid) - 1);
    if (!is_open) {
        strncpy((char *)ap_config.ap.password, pass_str, sizeof(ap_config.ap.password) - 1);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    if (!s_state.ap_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        /* wifi_start 之后 AP 才会真正起来；esp_wifi_set_config 在 start 前/后都允许 */
    } else {
        /* 已经在跑：esp_wifi_set_config 即可让 AP 用新参数 */
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    ESP_LOGI(TAG, "AP applied: SSID=%s PASS=%s IP=%s auth=%s",
             ssid_str, is_open ? "<open>" : pass_str, ip_str,
             is_open ? "OPEN" : "WPA2_PSK");
    return ESP_OK;
}

esp_err_t wifi_connect_sta(const char *ssid, const char *pass, int authmode)
{
    if (ssid == NULL || strlen(ssid) == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* OPEN 网络不允许填密码；密码为空时强制按 OPEN 连接，避免 WPA 协商失败 */
    if (authmode == WIFI_AUTH_OPEN || (pass != NULL && pass[0] == '\0')) {
        authmode = WIFI_AUTH_OPEN;
        pass = "";
    }

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    if (pass != NULL)
    {
        strncpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password) - 1);
    }
    sta_config.sta.threshold.authmode = (wifi_auth_mode_t)authmode;

    strncpy(s_state.sta_ssid, ssid, sizeof(s_state.sta_ssid) - 1);
    if (pass != NULL)
    {
        strncpy(s_state.sta_pass, pass, sizeof(s_state.sta_pass) - 1);
    }

    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    /* IDF 限制：STA 在连接/扫描过程中不允许 esp_wifi_set_config(WIFI_IF_STA)。
     * 稳妥顺序：先把 STA disconnect（允许失败），再 set_config；如仍被拒，把整个 wifi stop
     * 后再 set_config 再 start。失败时返回错误而不是 ESP_ERROR_CHECK abort。 */
    esp_wifi_disconnect();

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err == ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "STA busy, stopping wifi to apply new STA config");
        esp_wifi_stop();
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        if (err == ESP_OK) {
            esp_wifi_start();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA) failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_state.ap_started)
    {
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    ESP_LOGI(TAG, "STA connecting to %s, authmode=%d", ssid, authmode);
    return ESP_OK;
}

esp_err_t wifi_disconnect_sta(void)
{
    s_state.sta_connected = false;
    memset(s_state.sta_ssid, 0, sizeof(s_state.sta_ssid));
    memset(s_state.sta_pass, 0, sizeof(s_state.sta_pass));
    return esp_wifi_disconnect();
}

const wifi_state_t *wifi_get_state(void)
{
    return &s_state;
}

esp_err_t wifi_get_sta_ip(char *out, size_t out_size)
{
    if (s_sta_netif == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(out, out_size, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

int wifi_get_sta_rssi(void)
{
    if (!s_state.sta_connected) {
        return 0;
    }
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}
