#include "console_cmd.h"
#include "sd_speed_test.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_console.h"
#include "linenoise/linenoise.h"

#define TAG "console"
/* UART1 (TX=17, RX=18, 115200) 由 sdkconfig 的
 * CONFIG_ESP_CONSOLE_UART_CUSTOM 配置，REPL API 会自动应用。 */

static void format_rate(double kbs, char *out, size_t out_size)
{
    if (kbs >= 1024.0) {
        snprintf(out, out_size, "%.2f MB/s", kbs / 1024.0);
    } else {
        snprintf(out, out_size, "%.1f KB/s", kbs);
    }
}

static int cmd_sd_test(int argc, char **argv)
{
    if (argc == 1) {
        return sd_speed_test_run_all() == ESP_OK ? 0 : 1;
    }
    if (argc == 3) {
        char *end;
        long block = strtol(argv[1], &end, 10);
        if (*end != '\0' || block <= 0 || block > (64 * 1024)) {
            printf("bad block size: %s\n", argv[1]);
            return 1;
        }
        long total = strtol(argv[2], &end, 10);
        if (*end != '\0' || total <= 0) {
            printf("bad total bytes: %s\n", argv[2]);
            return 1;
        }
        sd_speed_result_t r;
        esp_err_t err = sd_speed_test_once((size_t)block, (size_t)total, &r);
        if (err != ESP_OK) {
            printf("test failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        char wb[16], rb[16];
        format_rate(r.write_kbs, wb, sizeof(wb));
        format_rate(r.read_kbs, rb, sizeof(rb));
        printf("block=%u total=%u write=%s read=%s verify=%s\n",
               (unsigned)r.block_size, (unsigned)r.total_bytes,
               wb, rb, r.verify_ok ? "OK" : "FAIL");
        return 0;
    }
    printf("usage:\n"
           "  sd_test                  standard sweep (1/4/16/64 KB)\n"
           "  sd_test <block> <bytes>  custom, e.g. sd_test 4096 1048576\n");
    return 1;
}

static void register_sd_test(void)
{
    const esp_console_cmd_t cmd = {
        .command = "sd_test",
        .help    = "SD card FATFS speed test",
        .hint    = "[block_size total_bytes]",
        .func    = &cmd_sd_test,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

esp_err_t console_start(void)
{
    /* esp_console_new_repl_uart 自己创建 UART driver 任务和 linenoise 循环，
     * 我只负责注册命令、启动 REPL。 */
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "esp> ";

    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    esp_console_repl_t *repl = NULL;
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &repl_cfg, &repl));

    register_sd_test();

    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    ESP_LOGI(TAG, "console ready on UART1, try: sd_test");
    return ESP_OK;
}
