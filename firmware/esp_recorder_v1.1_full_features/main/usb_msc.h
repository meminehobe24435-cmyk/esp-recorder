#ifndef USB_MSC_H
#define USB_MSC_H

#include "esp_err.h"

/* 初始化 USB MSC 所需的回调已在 TinyUSB 栈中静态注册。
 * 此函数仅作占位，保证模块被链接到最终镜像。 */
esp_err_t usb_msc_init(void);

#endif
