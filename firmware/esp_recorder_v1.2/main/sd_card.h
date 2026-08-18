#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdbool.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"

/* 需求：DATA-03 / DATA-04 / NF-03 — SD 卡 FATFS 记录与读取 */

typedef struct {
    bool mounted;
    bool usb_owned;
    char mount_point[16];
} sd_card_state_t;

esp_err_t sd_card_init(void);
esp_err_t sd_card_deinit(void);

/* 用于 USB MSC 与 FATFS 之间的安全切换：
 * 卸载/重新挂载文件系统，但保持 SPI 总线和 sdmmc_card_t 句柄有效。 */
esp_err_t sd_card_unmount_fs(void);
esp_err_t sd_card_remount_fs(void);
sdmmc_card_t *sd_card_get_handle(void);

const sd_card_state_t *sd_card_get_state(void);

#endif
