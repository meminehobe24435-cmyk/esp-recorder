#ifndef UART_DRIVER_H
#define UART_DRIVER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* 需求：DATA-01 / DATA-02 / WEB-07 — 用户传感器串口 RX/TX 与波特率配置 */

#define USER_UART_NUM       UART_NUM_0
#define USER_UART_TX_PIN    43
#define USER_UART_RX_PIN    44
#define USER_UART_BUF_SIZE  2048

esp_err_t user_uart_init(int baud_rate);
esp_err_t user_uart_set_baudrate(int baud_rate);
int user_uart_write(const uint8_t *data, size_t len);
int user_uart_read(uint8_t *buf, size_t len, uint32_t timeout_ms);

/* 注册 TX 完成回调（recorder 用：把"对外发出的字节"也写进 live 显示 / CSV）。 */
typedef esp_err_t (*user_uart_tx_cb_t)(const uint8_t *data, size_t len);
void user_uart_set_tx_callback(user_uart_tx_cb_t cb);

#endif
