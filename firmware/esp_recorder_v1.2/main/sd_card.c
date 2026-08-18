#include "sd_card.h"

#include <string.h>
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "diskio_sdmmc.h"
#include "diskio_impl.h"
#include "ff.h"

#define TAG "sd_card"

/* 硬件定义：README 2.1 — SD 卡走 SPI */
#define SD_MOUNT_POINT      "/sdcard"
#define SD_HOST             SPI2_HOST
#define SD_PIN_CS           10
#define SD_PIN_MOSI         38
#define SD_PIN_MISO         40
#define SD_PIN_SCK          39
#define SD_MAX_TRANSFER_SZ  4000

static sd_card_state_t s_state = {0};
static sdmmc_card_t *s_card = NULL;
static bool s_spi_initialized = false;

esp_err_t sd_card_init(void)
{
    if (s_state.mounted) {
        return ESP_OK;
    }

    esp_err_t ret;
    if (!s_spi_initialized) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = SD_PIN_MOSI,
            .miso_io_num = SD_PIN_MISO,
            .sclk_io_num = SD_PIN_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = SD_MAX_TRANSFER_SZ,
        };

        ret = spi_bus_initialize(SD_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_spi_initialized = true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    host.max_freq_khz = 20000;  // 20MHz

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = SD_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config,
                                  &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdspi_mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state.mounted = true;
    s_state.usb_owned = false;
    strncpy(s_state.mount_point, SD_MOUNT_POINT, sizeof(s_state.mount_point) - 1);
    ESP_LOGI(TAG, "SD mounted at %s, size: %llu MB",
             SD_MOUNT_POINT, ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));
    return ESP_OK;
}

static void sd_card_release_host(void)
{
    if (s_card == NULL) {
        return;
    }

    if (s_card->host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
        s_card->host.deinit_p(s_card->host.slot);
    } else {
        s_card->host.deinit();
    }
    free(s_card);
    s_card = NULL;
    s_state.mounted = false;
}

esp_err_t sd_card_deinit(void)
{
    if (s_state.mounted && s_card != NULL) {
        esp_err_t ret = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "unmount failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_state.mounted = false;
    } else {
        sd_card_release_host();
    }

    s_card = NULL;
    s_state.usb_owned = false;

    if (s_spi_initialized) {
        spi_bus_free(SD_HOST);
        s_spi_initialized = false;
    }

    ESP_LOGI(TAG, "SD deinited");
    return ESP_OK;
}

esp_err_t sd_card_unmount_fs(void)
{
    if (!s_state.mounted) {
        return ESP_OK;
    }

    if (s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 只卸载 FATFS/VFS，保留 sdmmc_card_t 和 SPI 设备，供 USB MSC 继续访问块设备。 */
    BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
    if (pdrv != FF_DRV_NOT_USED) {
        char drv[3] = {(char)('0' + pdrv), ':', 0};
        f_mount(0, drv, 0);
        ff_diskio_unregister(pdrv);
    }

    esp_err_t ret = esp_vfs_fat_unregister_path(SD_MOUNT_POINT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "vfs unregister failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state.mounted = false;
    s_state.usb_owned = true;
    ESP_LOGI(TAG, "SD FATFS unmounted, block device still available for USB MSC");
    return ESP_OK;
}

esp_err_t sd_card_remount_fs(void)
{
    if (s_state.mounted) {
        return ESP_OK;
    }

    /* USB 已断开：释放 USB MSC 占用的 SPI 设备/card，再重新完整初始化 FATFS。 */
    sd_card_release_host();
    sd_card_deinit();
    esp_err_t ret = sd_card_init();
    if (ret == ESP_OK) {
        s_state.usb_owned = false;
    }
    return ret;
}

sdmmc_card_t *sd_card_get_handle(void)
{
    return s_card;
}

const sd_card_state_t *sd_card_get_state(void)
{
    return &s_state;
}
