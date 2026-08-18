#include "uart_driver.h"

#include "freertos/FreeRTOS.h"
#include "driver/uart.h"
#include "esp_log.h"

#define TAG "uart_driver"

static user_uart_tx_cb_t s_tx_cb = NULL;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_channel_ready[USER_UART_CHANNEL_COUNT];
static uint64_t s_rx_bytes[USER_UART_CHANNEL_COUNT];
static uint64_t s_tx_bytes[USER_UART_CHANNEL_COUNT];

typedef struct {
    uart_port_t port;
    int tx_pin;
    int rx_pin;
} user_uart_hw_t;

static const user_uart_hw_t s_uart_hw[USER_UART_CHANNEL_COUNT] = {
    {USER_UART1_NUM, USER_UART1_TX_PIN, USER_UART1_RX_PIN},
    {USER_UART2_NUM, USER_UART2_TX_PIN, USER_UART2_RX_PIN},
};

static const user_uart_hw_t *user_uart_get_hw(uint8_t channel)
{
    if (channel < USER_UART_CHANNEL_1 || channel > USER_UART_CHANNEL_2) {
        return NULL;
    }
    return &s_uart_hw[channel - 1];
}

static size_t user_uart_channel_index(uint8_t channel)
{
    return (size_t)(channel - USER_UART_CHANNEL_1);
}

void user_uart_set_tx_callback(user_uart_tx_cb_t cb)
{
    s_tx_cb = cb;
}

esp_err_t user_uart_init_channel(uint8_t channel, int baud_rate)
{
    const user_uart_hw_t *hw = user_uart_get_hw(channel);
    if (hw == NULL || baud_rate <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!uart_is_driver_installed(hw->port)) {
        uart_config_t uart_config = {
            .baud_rate = baud_rate,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };

        esp_err_t ret = uart_param_config(hw->port, &uart_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "CH%u param_config failed: %s", channel, esp_err_to_name(ret));
            return ret;
        }

        ret = uart_set_pin(hw->port, hw->tx_pin, hw->rx_pin,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "CH%u set_pin failed: %s", channel, esp_err_to_name(ret));
            return ret;
        }

        ret = uart_driver_install(hw->port, USER_UART_BUF_SIZE, USER_UART_BUF_SIZE, 0, NULL, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "CH%u driver_install failed: %s", channel, esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "CH%u UART%d installed @ %d baud, TX=GPIO%d RX=GPIO%d",
                 channel, hw->port, baud_rate, hw->tx_pin, hw->rx_pin);
    } else {
        esp_err_t ret = uart_set_baudrate(hw->port, baud_rate);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "CH%u set_baudrate failed: %s", channel, esp_err_to_name(ret));
            return ret;
        }
        ret = uart_set_pin(hw->port, hw->tx_pin, hw->rx_pin,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "CH%u set_pin failed: %s", channel, esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "CH%u UART%d updated @ %d baud, TX=GPIO%d RX=GPIO%d",
                 channel, hw->port, baud_rate, hw->tx_pin, hw->rx_pin);
    }
    portENTER_CRITICAL(&s_state_lock);
    s_channel_ready[user_uart_channel_index(channel)] = true;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t user_uart_init(int baud_rate)
{
    for (uint8_t channel = USER_UART_CHANNEL_1; channel <= USER_UART_CHANNEL_2; channel++) {
        esp_err_t ret = user_uart_init_channel(channel, baud_rate);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t user_uart_set_channel_baudrate(uint8_t channel, int baud_rate)
{
    const user_uart_hw_t *hw = user_uart_get_hw(channel);
    if (hw == NULL || baud_rate <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = uart_set_baudrate(hw->port, baud_rate);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "CH%u UART%d baudrate set to %d", channel, hw->port, baud_rate);
    } else {
        ESP_LOGE(TAG, "CH%u set_baudrate failed: %s", channel, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t user_uart_set_baudrate(int baud_rate)
{
    for (uint8_t channel = USER_UART_CHANNEL_1; channel <= USER_UART_CHANNEL_2; channel++) {
        esp_err_t ret = user_uart_set_channel_baudrate(channel, baud_rate);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

/* 将配置中的数字映射到 ESP-IDF 枚举值。 */
static uart_word_length_t map_data_bits(int bits)
{
    switch (bits) {
    case 5:  return UART_DATA_5_BITS;
    case 6:  return UART_DATA_6_BITS;
    case 7:  return UART_DATA_7_BITS;
    default: return UART_DATA_8_BITS;
    }
}

static uart_stop_bits_t map_stop_bits(int bits)
{
    return (bits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

static uart_parity_t map_parity(int parity)
{
    if (parity == 1) return UART_PARITY_ODD;
    if (parity == 2) return UART_PARITY_EVEN;
    return UART_PARITY_DISABLE;
}

esp_err_t user_uart_configure_channel(uint8_t channel, int baud_rate,
                                       int data_bits, int stop_bits, int parity)
{
    const user_uart_hw_t *hw = user_uart_get_hw(channel);
    if (hw == NULL || baud_rate <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!uart_is_driver_installed(hw->port)) {
        return user_uart_init_channel(channel, baud_rate);
    }

    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = map_data_bits(data_bits),
        .parity    = map_parity(parity),
        .stop_bits = map_stop_bits(stop_bits),
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(hw->port, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CH%u param_config failed: %s", channel, esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(hw->port, hw->tx_pin, hw->rx_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CH%u set_pin failed: %s", channel, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "CH%u configured: %d baud, %d%c%d",
             channel, baud_rate, data_bits,
             parity == 0 ? 'N' : (parity == 1 ? 'O' : 'E'), stop_bits);
    return ESP_OK;
}

int user_uart_write_channel(uint8_t channel, const uint8_t *data, size_t len)
{
    const user_uart_hw_t *hw = user_uart_get_hw(channel);
    if (hw == NULL || data == NULL || len == 0 ||
        !user_uart_channel_is_ready(channel)) {
        return 0;
    }
    int sent = uart_write_bytes(hw->port, data, len);
    if (sent > 0) {
        portENTER_CRITICAL(&s_state_lock);
        s_tx_bytes[user_uart_channel_index(channel)] += (uint64_t)sent;
        portEXIT_CRITICAL(&s_state_lock);

        if (s_tx_cb != NULL) {
            s_tx_cb(channel, data, (size_t)sent);
        }
    }
    return sent;
}

int user_uart_read_channel(uint8_t channel, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    const user_uart_hw_t *hw = user_uart_get_hw(channel);
    if (hw == NULL || buf == NULL || len == 0 ||
        !user_uart_channel_is_ready(channel)) {
        return 0;
    }
    int received = uart_read_bytes(hw->port, buf, len, pdMS_TO_TICKS(timeout_ms));
    if (received > 0) {
        portENTER_CRITICAL(&s_state_lock);
        s_rx_bytes[user_uart_channel_index(channel)] += (uint64_t)received;
        portEXIT_CRITICAL(&s_state_lock);
    }
    return received;
}

bool user_uart_channel_is_ready(uint8_t channel)
{
    if (user_uart_get_hw(channel) == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_state_lock);
    bool ready = s_channel_ready[user_uart_channel_index(channel)];
    portEXIT_CRITICAL(&s_state_lock);
    return ready;
}

uint64_t user_uart_get_rx_bytes(uint8_t channel)
{
    if (user_uart_get_hw(channel) == NULL) {
        return 0;
    }

    portENTER_CRITICAL(&s_state_lock);
    uint64_t bytes = s_rx_bytes[user_uart_channel_index(channel)];
    portEXIT_CRITICAL(&s_state_lock);
    return bytes;
}

uint64_t user_uart_get_tx_bytes(uint8_t channel)
{
    if (user_uart_get_hw(channel) == NULL) {
        return 0;
    }

    portENTER_CRITICAL(&s_state_lock);
    uint64_t bytes = s_tx_bytes[user_uart_channel_index(channel)];
    portEXIT_CRITICAL(&s_state_lock);
    return bytes;
}

int user_uart_write(const uint8_t *data, size_t len)
{
    return user_uart_write_channel(USER_UART_CHANNEL_1, data, len);
}

int user_uart_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    return user_uart_read_channel(USER_UART_CHANNEL_1, buf, len, timeout_ms);
}
