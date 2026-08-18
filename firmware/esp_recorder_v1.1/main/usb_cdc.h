#ifndef USB_CDC_H
#define USB_CDC_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* 需求：3.2 / 6.4 — USB CDC 虚拟串口作为 AP 配网备选与调试通道。
 * 通过 TinyUSB CDC-ACM 实现，与 USB MSC 组成复合设备。
 */

typedef void (*usb_cdc_rx_cb_t)(const uint8_t *data, size_t len);

esp_err_t usb_cdc_init(usb_cdc_rx_cb_t rx_cb);
int usb_cdc_write(const uint8_t *data, size_t len);
int usb_cdc_read(uint8_t *buf, size_t len, uint32_t timeout_ms);

#endif
