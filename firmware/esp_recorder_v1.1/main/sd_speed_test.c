#include "sd_speed_test.h"
#include "sd_card.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "sdmmc_cmd.h"

#define TAG          "sd_speed"
#define TEST_PATH    "/sdcard/sd_speed_test.tmp"
#define TEST_BUF_MAX (64 * 1024)
#define TEST_SEED    0xA55AA55Au

static uint8_t *s_buf;

/* 简单 LCG 生成确定性的伪随机字节，用来填测试 buffer 并校验读回数据 */
static void fill_pattern(uint8_t *buf, size_t size, uint32_t s)
{
    for (size_t i = 0; i < size; i++) {
        s = s * 1103515245u + 12345u;
        buf[i] = (uint8_t)(s >> 16);
    }
}

static bool verify_pattern(const uint8_t *buf, size_t size, uint32_t s)
{
    for (size_t i = 0; i < size; i++) {
        s = s * 1103515245u + 12345u;
        if (buf[i] != (uint8_t)(s >> 16)) {
            return false;
        }
    }
    return true;
}

static void format_rate(double kbs, char *out, size_t out_size)
{
    if (kbs >= 1024.0) {
        snprintf(out, out_size, "%.2f MB/s", kbs / 1024.0);
    } else {
        snprintf(out, out_size, "%.1f KB/s", kbs);
    }
}

esp_err_t sd_speed_test_once(size_t block_size, size_t total_bytes,
                             sd_speed_result_t *result)
{
    if (!result || block_size == 0 || block_size > TEST_BUF_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (total_bytes < block_size) {
        total_bytes = block_size;
    }
    if (!sd_card_get_state()->mounted) {
        ESP_LOGE(TAG, "SD not mounted (USB connected?)");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_buf) {
        /* 优先放 PSRAM（ESP32-S3 一般有 8MB），失败再回落到普通 heap */
        s_buf = heap_caps_malloc(TEST_BUF_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_buf) {
            s_buf = malloc(TEST_BUF_MAX);
        }
        if (!s_buf) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(result, 0, sizeof(*result));
    result->block_size = block_size;

    size_t n_blocks = total_bytes / block_size;
    if (n_blocks == 0) n_blocks = 1;
    size_t actual = n_blocks * block_size;
    result->total_bytes = actual;

    fill_pattern(s_buf, block_size, TEST_SEED);
    remove(TEST_PATH);

    /* ---- 写入 ---- */
    int64_t t0 = esp_timer_get_time();
    FILE *f = fopen(TEST_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen wb failed");
        return ESP_FAIL;
    }
    for (size_t i = 0; i < n_blocks; i++) {
        if (fwrite(s_buf, 1, block_size, f) != block_size) {
            ESP_LOGE(TAG, "fwrite failed at block %u/%u",
                     (unsigned)i, (unsigned)n_blocks);
            fclose(f);
            remove(TEST_PATH);
            return ESP_FAIL;
        }
    }
    fclose(f);   /* 这里触发 FATFS flush + 真正落盘 */
    int64_t t1 = esp_timer_get_time();

    result->write_ms  = (uint32_t)((t1 - t0) / 1000);
    result->write_kbs = (double)actual / 1024.0 / ((double)(t1 - t0) / 1e6);

    /* ---- 读取 + 校验 ---- */
    t0 = esp_timer_get_time();
    f = fopen(TEST_PATH, "rb");
    if (!f) {
        ESP_LOGE(TAG, "fopen rb failed");
        remove(TEST_PATH);
        return ESP_FAIL;
    }
    bool verify_ok = true;
    for (size_t i = 0; i < n_blocks; i++) {
        if (fread(s_buf, 1, block_size, f) != block_size) {
            ESP_LOGE(TAG, "fread failed at block %u/%u",
                     (unsigned)i, (unsigned)n_blocks);
            fclose(f);
            remove(TEST_PATH);
            return ESP_FAIL;
        }
        /* 只校验首块避免拖慢测试；数据模式本身是确定的 */
        if (i == 0) {
            verify_ok = verify_pattern(s_buf, block_size, TEST_SEED);
        }
    }
    fclose(f);
    t1 = esp_timer_get_time();

    result->read_ms   = (uint32_t)((t1 - t0) / 1000);
    result->read_kbs  = (double)actual / 1024.0 / ((double)(t1 - t0) / 1e6);
    result->verify_ok = verify_ok;

    remove(TEST_PATH);
    return ESP_OK;
}

esp_err_t sd_speed_test_run_all(void)
{
    ESP_LOGI(TAG, "============= SD card speed test =============");
    const sd_card_state_t *st = sd_card_get_state();
    if (!st || !st->mounted) {
        ESP_LOGE(TAG, "SD not mounted, abort");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "mount: %s", st->mount_point);

    sdmmc_card_t *card = sd_card_get_handle();
    if (card) {
        ESP_LOGI(TAG, "card: %s, sector=%u B, size=%llu MB",
                 card->cid.name, (unsigned)card->csd.sector_size,
                 ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
    }
    ESP_LOGI(TAG, "------------------------------------------------");
    ESP_LOGI(TAG, "%-10s %-10s %-12s %-12s %-6s",
             "Block", "Total", "Write", "Read", "Verify");

    static const struct { size_t b; size_t t; } cfgs[] = {
        { 1  * 1024, 1  * 1024 * 1024 },
        { 4  * 1024, 4  * 1024 * 1024 },
        { 16 * 1024, 4  * 1024 * 1024 },
        { 64 * 1024, 4  * 1024 * 1024 },
    };
    const int n = sizeof(cfgs) / sizeof(cfgs[0]);

    for (int i = 0; i < n; i++) {
        sd_speed_result_t r;
        esp_err_t err = sd_speed_test_once(cfgs[i].b, cfgs[i].t, &r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "block=%u test failed: %s",
                     (unsigned)cfgs[i].b, esp_err_to_name(err));
            continue;
        }
        char wb[16], rb[16];
        format_rate(r.write_kbs, wb, sizeof(wb));
        format_rate(r.read_kbs, rb, sizeof(rb));
        ESP_LOGI(TAG, "%-10u %-10u %-12s %-12s %s",
                 (unsigned)r.block_size, (unsigned)r.total_bytes,
                 wb, rb, r.verify_ok ? "OK" : "FAIL");
    }
    ESP_LOGI(TAG, "================================================");
    return ESP_OK;
}
