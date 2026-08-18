#ifndef UART_DRIVER_H
#define UART_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/uart.h"

/* 两路独立用户串口。调试控制台使用 UART1，不占用这两个通道。 */
#define USER_UART_CHANNEL_COUNT 2

#define USER_UART_CHANNEL_1 1
#define USER_UART_CHANNEL_2 2

#define USER_UART1_NUM       UART_NUM_0
#define USER_UART1_TX_PIN    43
#define USER_UART1_RX_PIN    44

#define USER_UART2_NUM       UART_NUM_2
#define USER_UART2_TX_PIN    17
#define USER_UART2_RX_PIN    18

#define USER_UART_BUF_SIZE   2048

/* 初始化两路串口，当前两路共用同一个波特率配置。 */
esp_err_t user_uart_init(int baud_rate);
esp_err_t user_uart_init_channel(uint8_t channel, int baud_rate);
esp_err_t user_uart_set_baudrate(int baud_rate);
esp_err_t user_uart_set_channel_baudrate(uint8_t channel, int baud_rate);

/* 运行时同步更新通道的完整 UART 参数（波特率、数据位、停止位、校验位）。
 * 不会重新安装驱动，只调用 uart_param_config + uart_set_pin 热更新。 */
esp_err_t user_uart_configure_channel(uint8_t channel, int baud_rate,
                                      int data_bits, int stop_bits, int parity);

int user_uart_write(const uint8_t *data, size_t len);
int user_uart_read(uint8_t *buf, size_t len, uint32_t timeout_ms);
int user_uart_write_channel(uint8_t channel, const uint8_t *data, size_t len);
int user_uart_read_channel(uint8_t channel, uint8_t *buf, size_t len, uint32_t timeout_ms);

/* Raw UART state and byte counters are independent of SD recording state. */
bool user_uart_channel_is_ready(uint8_t channel);
uint64_t user_uart_get_rx_bytes(uint8_t channel);
uint64_t user_uart_get_tx_bytes(uint8_t channel);

/* 注册 TX 完成回调，用于把已发送数据同步到 Web 实时显示。 */
typedef esp_err_t (*user_uart_tx_cb_t)(uint8_t channel, const uint8_t *data, size_t len);
void user_uart_set_tx_callback(user_uart_tx_cb_t cb);

#endif
