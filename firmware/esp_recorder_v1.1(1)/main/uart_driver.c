#include "uart_driver.h"

#include "driver/uart.h"
#include "esp_log.h"

#define TAG "uart_driver"

static user_uart_tx_cb_t s_tx_cb = NULL;

void user_uart_set_tx_callback(user_uart_tx_cb_t cb)
{
    s_tx_cb = cb;
}

esp_err_t user_uart_init(int baud_rate)
{
    /* 幂等：已安装时只更新参数与引脚，避免在 TinyUSB/SD 已就绪后
     * 误删驱动破坏运行时上下文（导致 UART0 RX 失能）。 */
    if (!uart_is_driver_installed(USER_UART_NUM)) {
        uart_config_t uart_config = {
            .baud_rate = baud_rate,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };

        esp_err_t ret = uart_param_config(USER_UART_NUM, &uart_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "param_config failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = uart_set_pin(USER_UART_NUM, USER_UART_TX_PIN, USER_UART_RX_PIN,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "set_pin failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = uart_driver_install(USER_UART_NUM, USER_UART_BUF_SIZE, USER_UART_BUF_SIZE, 0, NULL, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "driver_install failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "UART%d installed @ %d baud, TX=GPIO%d RX=GPIO%d",
                 USER_UART_NUM, baud_rate, USER_UART_TX_PIN, USER_UART_RX_PIN);
    } else {
        /* 已安装：仅同步 baudrate 与引脚映射（不重装驱动） */
        esp_err_t ret = uart_set_baudrate(USER_UART_NUM, baud_rate);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "UART%d baudrate set to %d", USER_UART_NUM, baud_rate);
        } else {
            ESP_LOGE(TAG, "set_baudrate failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t user_uart_set_baudrate(int baud_rate)
{
    esp_err_t ret = uart_set_baudrate(USER_UART_NUM, baud_rate);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "UART%d baudrate set to %d", USER_UART_NUM, baud_rate);
    } else {
        ESP_LOGE(TAG, "set_baudrate failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

int user_uart_write(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return 0;
    }
    int sent = uart_write_bytes(USER_UART_NUM, data, len);
    if (sent > 0 && s_tx_cb != NULL) {
        s_tx_cb(data, (size_t)sent);
    }
    return sent;
}

int user_uart_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    if (buf == NULL || len == 0) {
        return 0;
    }
    return uart_read_bytes(USER_UART_NUM, buf, len, pdMS_TO_TICKS(timeout_ms));
}
