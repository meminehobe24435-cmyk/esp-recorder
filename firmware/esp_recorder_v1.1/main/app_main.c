#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "tusb.h"

#include "sd_card.h"
#include "uart_driver.h"
#include "usb_cdc.h"
#include "usb_msc.h"
#include "wifi_config.h"
#include "data_recorder.h"
#include "data_router.h"
#include "config_manager.h"
#include "web_server.h"
#include "console_cmd.h"

#define TAG "main"

#define DEFAULT_BAUD_RATE 115200

static void usb_cdc_rx_handler(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
}

/* USB 复合设备挂载成功后，FATFS 必须与 SD 卡块设备互斥访问：
 * 卸载 FATFS，把 SD 卡完全交给 USB 主机。 */
void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "USB mounted, releasing SD card from FATFS");
    recorder_stop();
    sd_card_unmount_fs();
}

/* USB 断开后，重新挂载 FATFS，恢复 ESP32 本地文件访问。 */
void tud_umount_cb(void)
{
    ESP_LOGI(TAG, "USB unmounted, reclaiming SD card for FATFS");
    if (sd_card_remount_fs() == ESP_OK)
    {
        const device_config_t *cfg = config_manager_get(config_manager_instance());
        if (cfg->recorder.auto_start)
        {
            recorder_start();
        }
    }
}

static void uart_rx_task(void *arg)
{
    uint8_t channel = (uint8_t)(uintptr_t)arg;
    uint8_t buf[256];

    while (1)
    {
        int len = user_uart_read_channel(channel, buf, sizeof(buf), 100);
        if (len > 0)
        {
            ESP_LOGD(TAG, "CH%u rx %d bytes", channel, len);
            recorder_write_rx_channel(channel, buf, len);
            data_router_forward(channel, buf, (size_t)len);
        }
    }
}
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
void app_main(void)
{
    gpio_config_t uart_select = {
        .pin_bit_mask = 1 << GPIO_NUM_16,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};

    gpio_config(&uart_select);
    gpio_set_level(GPIO_NUM_16, 0); // 拉低用于开启串口（接收使能，boot 期间保持）

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(sd_card_init());

    ESP_ERROR_CHECK(usb_cdc_init(usb_cdc_rx_handler));
    ESP_ERROR_CHECK(usb_msc_init());
    /* 加载配置 */
    config_manager_t *cfg_mgr = config_manager_instance();
    esp_err_t cfg_err = config_manager_load(cfg_mgr, "/sdcard/config.json");
    if (cfg_err != ESP_OK)
    {
        ESP_LOGW(TAG, "config.json not found, creating default config file");
        config_manager_reset_defaults(cfg_mgr);
        config_manager_save(cfg_mgr, "/sdcard/config.json");
    }
    const device_config_t *cfg = config_manager_get(cfg_mgr);

    ESP_ERROR_CHECK(recorder_init());

    /* Initialize channels independently so one failed interface does not block the other. */
    bool uart_channel_ready[USER_UART_CHANNEL_COUNT] = {false};
    for (uint8_t channel = USER_UART_CHANNEL_1;
         channel <= USER_UART_CHANNEL_2;
         channel++)
    {
        esp_err_t uart_ret = user_uart_init_channel(channel, cfg->uart.baudrate);
        if (uart_ret == ESP_OK)
        {
            uart_channel_ready[channel - USER_UART_CHANNEL_1] = true;
        }
        else
        {
            ESP_LOGE(TAG, "CH%u initialization failed: %s",
                     channel, esp_err_to_name(uart_ret));
        }
    }
    user_uart_set_tx_callback(recorder_write_tx_channel);
    ESP_ERROR_CHECK(data_router_init());

    ESP_ERROR_CHECK(wifi_config_init());
    ESP_ERROR_CHECK(wifi_apply_ap(cfg->wifi.ap_ssid, cfg->wifi.ap_pass, cfg->wifi.ap_ip));
    /* 启动 AP 模式本地 Web（手机/电脑连接热点后访问 192.168.4.1） */
    ESP_ERROR_CHECK(web_server_start());
    /* 用 config.json 中保存的 STA 信息尝试连接外部 WiFi */
    if (cfg->wifi.enable_sta && cfg->wifi.sta_ssid[0] != '\0')
    {
        wifi_connect_sta(cfg->wifi.sta_ssid, cfg->wifi.sta_pass, cfg->wifi.sta_authmode);
    }

    if (cfg->recorder.auto_start)
    {
        if (recorder_start() != ESP_OK)
        {
            ESP_LOGW(TAG, "auto start recording failed");
        }
    }
    else
    {
        ESP_LOGI(TAG, "auto_start disabled, waiting for web/MQTT command");
    }

    static const char *uart_task_names[USER_UART_CHANNEL_COUNT] = {
        "uart_rx_ch1",
        "uart_rx_ch2",
    };
    for (uint8_t channel = USER_UART_CHANNEL_1;
         channel <= USER_UART_CHANNEL_2;
         channel++)
    {
        size_t index = channel - USER_UART_CHANNEL_1;
        if (!uart_channel_ready[index])
        {
            continue;
        }

        BaseType_t task_ret = xTaskCreate(
            uart_rx_task, uart_task_names[index], 4096,
            (void *)(uintptr_t)channel, 5, NULL);
        if (task_ret == pdPASS)
        {
            ESP_LOGI(TAG, "CH%u RX task started", channel);
        }
        else
        {
            ESP_LOGE(TAG, "CH%u RX task creation failed", channel);
        }
    }
    console_start();

    ESP_LOGI(TAG, "esp_recorder init complete");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
