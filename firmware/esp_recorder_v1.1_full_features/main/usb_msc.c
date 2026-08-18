#include "usb_msc.h"

#include "tusb.h"
#include "sd_card.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "usb_msc"

#if CFG_TUD_MSC

#define MSC_IO_RETRY_MAX    3
#define MSC_IO_RETRY_DELAY_MS 10

static sdmmc_card_t *msc_get_card(void)
{
    return sd_card_get_handle();
}

/* 容量信息由 SD CSD 提供；标准 SD 卡扇区大小为 512 字节。 */
static uint32_t msc_sector_size(void)
{
    sdmmc_card_t *card = msc_get_card();
    if (card == NULL || card->csd.sector_size == 0) {
        return 512;
    }
    return card->csd.sector_size;
}

static uint32_t msc_sector_count(void)
{
    sdmmc_card_t *card = msc_get_card();
    if (card == NULL) {
        return 0;
    }
    return card->csd.capacity;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4])
{
    (void) lun;

    const char vid[] = "Espressif";
    const char pid[] = "ESP Recorder MSC";
    const char rev[] = "1.0";

    memcpy(vendor_id, vid, strlen(vid));
    memcpy(product_id, pid, strlen(pid));
    memcpy(product_rev, rev, strlen(rev));
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void) lun;
    return msc_get_card() != NULL;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void) lun;

    *block_count = msc_sector_count();
    *block_size = (uint16_t) msc_sector_size();
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                           bool start, bool load_eject)
{
    (void) lun;
    (void) power_condition;
    (void) start;
    (void) load_eject;
    return true;
}

static esp_err_t sdmmc_read_sectors_retry(sdmmc_card_t *card, void *dst,
                                          size_t start_sector, size_t sector_count)
{
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < MSC_IO_RETRY_MAX; i++) {
        ret = sdmmc_read_sectors(card, dst, start_sector, sector_count);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "read retry %d/%d: %s", i + 1, MSC_IO_RETRY_MAX, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(MSC_IO_RETRY_DELAY_MS));
    }
    return ret;
}

static esp_err_t sdmmc_write_sectors_retry(sdmmc_card_t *card, const void *src,
                                           size_t start_sector, size_t sector_count)
{
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < MSC_IO_RETRY_MAX; i++) {
        ret = sdmmc_write_sectors(card, src, start_sector, sector_count);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "write retry %d/%d: %s", i + 1, MSC_IO_RETRY_MAX, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(MSC_IO_RETRY_DELAY_MS));
    }
    return ret;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    (void) lun;

    sdmmc_card_t *card = msc_get_card();
    if (card == NULL) {
        return -1;
    }

    uint32_t sector_size = msc_sector_size();
    if (offset != 0 || (bufsize % sector_size) != 0) {
        ESP_LOGE(TAG, "unsupported read params: lba=%lu offset=%lu bufsize=%lu",
                 (unsigned long) lba, (unsigned long) offset, (unsigned long) bufsize);
        return -1;
    }

    uint32_t sector_count = bufsize / sector_size;
    esp_err_t ret = sdmmc_read_sectors_retry(card, buffer, lba, sector_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_read_sectors failed: %s", esp_err_to_name(ret));
        return -1;
    }

    return (int32_t) bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize)
{
    (void) lun;

    sdmmc_card_t *card = msc_get_card();
    if (card == NULL) {
        return -1;
    }

    uint32_t sector_size = msc_sector_size();
    if (offset != 0 || (bufsize % sector_size) != 0) {
        ESP_LOGE(TAG, "unsupported write params: lba=%lu offset=%lu bufsize=%lu",
                 (unsigned long) lba, (unsigned long) offset, (unsigned long) bufsize);
        return -1;
    }

    uint32_t sector_count = bufsize / sector_size;
    esp_err_t ret = sdmmc_write_sectors_retry(card, buffer, lba, sector_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_write_sectors failed: %s", esp_err_to_name(ret));
        return -1;
    }

    return (int32_t) bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void *buffer, uint16_t bufsize)
{
    (void) lun;
    (void) buffer;
    (void) bufsize;

    int32_t resplen = 0;

    switch (scsi_cmd[0]) {
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        resplen = -1;
        break;
    }

    return resplen;
}

#endif /* CFG_TUD_MSC */

esp_err_t usb_msc_init(void)
{
    /* TinyUSB MSC 回调已静态链接；此函数仅用于确保本文件被包含进构建。 */
    ESP_LOGI(TAG, "USB MSC callbacks registered");
    return ESP_OK;
}
