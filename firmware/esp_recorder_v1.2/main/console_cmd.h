#ifndef CONSOLE_CMD_H
#define CONSOLE_CMD_H

#include "esp_err.h"

/* 在 GPIO17/18 (UART1) 上挂一个 REPL。
 * 通过 ESP_CONSOLE_UART_CUSTOM（sdkconfig 已配）走 UART1，
 * 不和 USB CDC / user_uart (GPIO16) 冲突。
 *
 * 注册命令：
 *   sd_test                — 跑一组标准块大小读写测试
 *   sd_test <size> <bytes> — 自定义：block_size total_bytes（字节）
 *   help                   — linenoise 内置
 */
esp_err_t console_start(void);

#endif
