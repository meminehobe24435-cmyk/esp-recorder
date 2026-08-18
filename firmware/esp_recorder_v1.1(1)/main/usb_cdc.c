#include "usb_cdc.h"

#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "esp_log.h"
#include "driver/gpio.h"

#define TAG "usb_cdc"

#define USB_CDC_TX_BUF_SIZE 512
#define USB_CDC_RX_BUF_SIZE 512
#define EPNUM_MSC       1
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
static usb_cdc_rx_cb_t s_rx_cb = NULL;

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL
};

enum {
    EDPT_CTRL_OUT = 0x00,
    EDPT_CTRL_IN  = 0x80,

    EDPT_MSC_OUT  = 0x01,
    EDPT_MSC_IN   = 0x81,
};

static uint8_t const desc_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, TUD_OPT_HIGH_SPEED ? 512 : 64),
};

static tusb_desc_device_t descriptor_config = {
    .bLength = sizeof(descriptor_config),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, // This is Espressif VID. This needs to be changed according to Users / Customers
    .idProduct = 0x4002,
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

static const char *s_string_desc[] = {
    (const char[]) {0x09, 0x04}, /* Langid */
    "Espressif",                 /* Manufacturer */
    "ESP Recorder",              /* Product */
    "123456",                    /* Serial */
    "ESP Recorder CDC",          /* CDC Interface */
    "ESP Recorder MSC",          /* MSC Interface */
};

static void usb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void) itf;
    (void) event;

    uint8_t buf[USB_CDC_RX_BUF_SIZE];
    size_t rx_size = 0;

    while (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, sizeof(buf), &rx_size) == ESP_OK
           && rx_size > 0) {
        if (s_rx_cb != NULL) {
            s_rx_cb(buf, rx_size);
        }
    }
}

esp_err_t usb_cdc_init(usb_cdc_rx_cb_t rx_cb)
{
    s_rx_cb = rx_cb;
    tinyusb_config_t tusb_cfg = {
        .device_descriptor = &descriptor_config,
        .string_descriptor = s_string_desc,
        .string_descriptor_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]),
        .external_phy = false,
        .configuration_descriptor = desc_configuration,
        .self_powered = true,
        .vbus_monitor_io = GPIO_NUM_48
    };

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    tinyusb_config_cdcacm_t cdc_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = USB_CDC_RX_BUF_SIZE,
        .callback_rx = &usb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };

    ret = tusb_cdc_acm_init(&cdc_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tusb_cdc_acm_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "TinyUSB CDC init done");
    return ESP_OK;
}

int usb_cdc_write(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return 0;
    }

    size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data, len);
    if (queued > 0) {
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    }
    return (int) queued;
}

int usb_cdc_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    if (buf == NULL || len == 0) {
        return 0;
    }

    size_t rx_size = 0;
    esp_err_t ret = tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, len, &rx_size);
    if (ret != ESP_OK) {
        return -1;
    }
    return (int) rx_size;
}
