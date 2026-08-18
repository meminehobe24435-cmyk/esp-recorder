#include "data_router.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#include "uart_driver.h"

#define TAG "data_router"

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static data_router_stats_t s_stats;

esp_err_t data_router_init(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.enabled = true;
    portEXIT_CRITICAL(&s_stats_lock);

    ESP_LOGI(TAG, "routing enabled: CH1 RX -> CH2 TX, CH2 RX -> CH1 TX");
    return ESP_OK;
}

int data_router_forward(uint8_t source_channel, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return 0;
    }

    uint8_t target_channel;
    if (source_channel == USER_UART_CHANNEL_1) {
        target_channel = USER_UART_CHANNEL_2;
    } else if (source_channel == USER_UART_CHANNEL_2) {
        target_channel = USER_UART_CHANNEL_1;
    } else {
        return -1;
    }

    portENTER_CRITICAL(&s_stats_lock);
    bool enabled = s_stats.enabled;
    portEXIT_CRITICAL(&s_stats_lock);
    if (!enabled) {
        return 0;
    }

    int sent = user_uart_write_channel(target_channel, data, len);
    size_t sent_bytes = sent > 0 ? (size_t)sent : 0;
    size_t drop_bytes = sent_bytes < len ? len - sent_bytes : 0;

    portENTER_CRITICAL(&s_stats_lock);
    if (source_channel == USER_UART_CHANNEL_1) {
        s_stats.ch1_to_ch2_bytes += sent_bytes;
        s_stats.ch1_to_ch2_drop_bytes += drop_bytes;
    } else {
        s_stats.ch2_to_ch1_bytes += sent_bytes;
        s_stats.ch2_to_ch1_drop_bytes += drop_bytes;
    }
    portEXIT_CRITICAL(&s_stats_lock);

    if (drop_bytes > 0) {
        ESP_LOGW(TAG, "CH%u -> CH%u routed %u/%u bytes",
                 source_channel, target_channel,
                 (unsigned)sent_bytes, (unsigned)len);
    }
    return (int)sent_bytes;
}

void data_router_get_stats(data_router_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_stats_lock);
    *stats = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);
}
