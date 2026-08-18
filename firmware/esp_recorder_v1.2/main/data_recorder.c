#include "data_recorder.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_card.h"

#define TAG "recorder"

#define RECORDER_DIR            "/sdcard"
#define SAVE_RINGBUF_SIZE       16384
#define SAVE_THRESHOLD_BYTES    2048
#define SAVE_TIMEOUT_MS         1000
#define SAVE_TASK_STACK         4096
#define SAVE_TASK_PRIORITY      5
#define SYNC_TASK_STACK         3072
#define SYNC_TASK_PRIORITY      1
#define FILE_NAME_SIZE          96
#define LIVE_CHUNK_BYTES        48

typedef struct {
    uint8_t *buf;
    size_t size;
    size_t head;
    size_t tail;
} ringbuf_t;

typedef struct {
    FILE *file;
    char filename[FILE_NAME_SIZE];
    ringbuf_t save_rb;
    uint64_t rx_total;
    uint32_t drop_count;
    uint32_t fsync_fail_count;
    bool failed;
} recorder_channel_state_t;

static bool s_running = false;
static SemaphoreHandle_t s_mutex = NULL;
static SemaphoreHandle_t s_data_sem = NULL;
static recorder_channel_state_t s_channels[RECORDER_CHANNEL_COUNT] = {0};

static recorder_live_item_t s_live[RECORDER_LIVE_CAPACITY] = {0};
static size_t s_live_head = 0;
static size_t s_live_count = 0;
static uint32_t s_live_seq = 0;
static SemaphoreHandle_t s_live_mutex = NULL;

static bool recorder_channel_valid(uint8_t channel)
{
    return channel >= RECORDER_CHANNEL_1 && channel <= RECORDER_CHANNEL_2;
}

static recorder_channel_state_t *recorder_channel_get(uint8_t channel)
{
    if (!recorder_channel_valid(channel)) {
        return NULL;
    }
    return &s_channels[channel - 1];
}

static size_t ringbuf_count(const ringbuf_t *rb)
{
    return (rb->head + rb->size - rb->tail) % rb->size;
}

static size_t ringbuf_free(const ringbuf_t *rb)
{
    return (rb->tail + rb->size - rb->head - 1) % rb->size;
}

static size_t ringbuf_write(ringbuf_t *rb, const uint8_t *data, size_t len)
{
    size_t free_bytes = ringbuf_free(rb);
    if (len > free_bytes) {
        len = free_bytes;
    }
    for (size_t i = 0; i < len; i++) {
        rb->buf[rb->head] = data[i];
        rb->head = (rb->head + 1) % rb->size;
    }
    return len;
}

static size_t ringbuf_read(ringbuf_t *rb, uint8_t *out, size_t len)
{
    size_t count = ringbuf_count(rb);
    if (len > count) {
        len = count;
    }
    for (size_t i = 0; i < len; i++) {
        out[i] = rb->buf[rb->tail];
        rb->tail = (rb->tail + 1) % rb->size;
    }
    return len;
}

static void ringbuf_reset(ringbuf_t *rb)
{
    rb->head = 0;
    rb->tail = 0;
}

static void format_iso_timestamp(char *out, size_t out_size)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_info;
    localtime_r(&tv.tv_sec, &tm_info);
    snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02d.%03ld",
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
             (long)(tv.tv_usec / 1000));
}

/* 调用者必须持有 s_mutex。 */
static void recorder_close_channel_locked(uint8_t channel)
{
    recorder_channel_state_t *state = recorder_channel_get(channel);
    if (state == NULL || state->file == NULL) {
        return;
    }

    fflush(state->file);
    int fd = fileno(state->file);
    if (fd >= 0) {
        fsync(fd);
    }
    fclose(state->file);
    state->file = NULL;
    ESP_LOGI(TAG, "CH%u file closed: %s, rx=%" PRIu64 " drop=%" PRIu32,
             channel, state->filename, state->rx_total, state->drop_count);
}

/* 调用者必须持有 s_mutex。 */
static esp_err_t recorder_compute_filename_locked(uint8_t channel)
{
    recorder_channel_state_t *state = recorder_channel_get(channel);
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_info;
    localtime_r(&tv.tv_sec, &tm_info);

    for (int suffix = 0; suffix < 10000; suffix++) {
        if (suffix == 0) {
            snprintf(state->filename, sizeof(state->filename),
                     "%s/REC_CH%u_%04d%02d%02d_%02d%02d%02d.bin",
                     RECORDER_DIR, channel,
                     tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                     tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        } else {
            snprintf(state->filename, sizeof(state->filename),
                     "%s/REC_CH%u_%04d%02d%02d_%02d%02d%02d_%d.bin",
                     RECORDER_DIR, channel,
                     tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                     tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, suffix);
        }
        struct stat st;
        if (stat(state->filename, &st) != 0) {
            return ESP_OK;
        }
    }
    state->filename[0] = '\0';
    return ESP_ERR_NOT_FOUND;
}

/* 调用者必须持有 s_mutex。 */
static void recorder_flush_channel_locked(uint8_t channel)
{
    recorder_channel_state_t *state = recorder_channel_get(channel);
    if (state == NULL || state->file == NULL) {
        return;
    }

    uint8_t tmp[1024];
    size_t count;
    while ((count = ringbuf_count(&state->save_rb)) > 0) {
        size_t to_read = count > sizeof(tmp) ? sizeof(tmp) : count;
        ringbuf_read(&state->save_rb, tmp, to_read);
        size_t written = fwrite(tmp, 1, to_read, state->file);
        if (written != to_read) {
            state->drop_count += (uint32_t)(to_read - written);
            state->failed = true;
            ESP_LOGE(TAG, "CH%u fwrite failed: %u/%u",
                     channel, (unsigned)written, (unsigned)to_read);
            break;
        }
    }
    fflush(state->file);
}

/* 调用者必须持有 s_mutex。 */
static void recorder_flush_all_locked(void)
{
    const sd_card_state_t *sd = sd_card_get_state();
    if (!sd->mounted) {
        for (uint8_t channel = RECORDER_CHANNEL_1;
             channel <= RECORDER_CHANNEL_2; channel++) {
            recorder_channel_state_t *state = recorder_channel_get(channel);
            state->drop_count += (uint32_t)ringbuf_count(&state->save_rb);
            ringbuf_reset(&state->save_rb);
            recorder_close_channel_locked(channel);
        }
        return;
    }

    for (uint8_t channel = RECORDER_CHANNEL_1;
         channel <= RECORDER_CHANNEL_2; channel++) {
        recorder_flush_channel_locked(channel);
    }
}

static void recorder_live_push(uint8_t channel, uint8_t direction,
                               const uint8_t *data, size_t len)
{
    if (!recorder_channel_valid(channel) || data == NULL || len == 0 ||
        s_live_mutex == NULL) {
        return;
    }

    uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    char timestamp[32];
    format_iso_timestamp(timestamp, sizeof(timestamp));
    const char *dir_str = direction == 0 ? "RX" : "TX";

    for (size_t offset = 0; offset < len; offset += LIVE_CHUNK_BYTES) {
        size_t chunk = len - offset;
        if (chunk > LIVE_CHUNK_BYTES) {
            chunk = LIVE_CHUNK_BYTES;
        }

        xSemaphoreTake(s_live_mutex, portMAX_DELAY);
        recorder_live_item_t *slot = &s_live[s_live_head];
        slot->seq = ++s_live_seq;
        slot->ts_ms = now_ms;
        int n = snprintf(slot->line, sizeof(slot->line), "%s,CH%u,%s,",
                         timestamp, channel, dir_str);
        size_t pos = (n > 0) ? (size_t)n : 0;
        for (size_t i = 0; i < chunk && pos + 3 < sizeof(slot->line); i++) {
            int added = snprintf(slot->line + pos, sizeof(slot->line) - pos,
                                 "%02X", data[offset + i]);
            if (added <= 0) {
                break;
            }
            pos += (size_t)added;
        }
        if (pos + 1 < sizeof(slot->line)) {
            slot->line[pos++] = '\n';
            slot->line[pos] = '\0';
        } else {
            slot->line[sizeof(slot->line) - 1] = '\0';
        }

        s_live_head = (s_live_head + 1) % RECORDER_LIVE_CAPACITY;
        if (s_live_count < RECORDER_LIVE_CAPACITY) {
            s_live_count++;
        }
        xSemaphoreGive(s_live_mutex);
    }
}

static void recorder_write_raw(uint8_t channel, const uint8_t *data, size_t len)
{
    recorder_channel_state_t *state = recorder_channel_get(channel);
    if (state == NULL || data == NULL || len == 0 || s_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_running || state->failed) {
        xSemaphoreGive(s_mutex);
        return;
    }

    if (state->file == NULL) {
        state->file = fopen(state->filename, "wb");
        if (state->file == NULL) {
            state->drop_count += (uint32_t)len;
            ESP_LOGE(TAG, "CH%u can not open %s", channel, state->filename);
            xSemaphoreGive(s_mutex);
            return;
        }
        ESP_LOGI(TAG, "CH%u file opened: %s", channel, state->filename);
    }

    size_t written = ringbuf_write(&state->save_rb, data, len);
    state->rx_total += written;
    if (written < len) {
        state->drop_count += (uint32_t)(len - written);
    }
    xSemaphoreGive(s_mutex);
    xSemaphoreGive(s_data_sem);
}

static void recorder_io_task(void *arg)
{
    (void)arg;
    while (1) {
        BaseType_t notified =
            xSemaphoreTake(s_data_sem, pdMS_TO_TICKS(SAVE_TIMEOUT_MS));

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool should_flush = notified == pdFALSE;
        if (!should_flush) {
            for (uint8_t channel = RECORDER_CHANNEL_1;
                 channel <= RECORDER_CHANNEL_2; channel++) {
                if (ringbuf_count(&recorder_channel_get(channel)->save_rb) >=
                    SAVE_THRESHOLD_BYTES) {
                    should_flush = true;
                    break;
                }
            }
        }
        if (should_flush) {
            recorder_flush_all_locked();
        }
        xSemaphoreGive(s_mutex);
    }
}

static void recorder_sync_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        recorder_flush_all_locked();
        for (uint8_t channel = RECORDER_CHANNEL_1;
             channel <= RECORDER_CHANNEL_2; channel++) {
            recorder_channel_state_t *state = recorder_channel_get(channel);
            if (state->file == NULL) {
                continue;
            }

            int fd = fileno(state->file);
            int rc = fd >= 0 ? fsync(fd) : -1;
            if (rc == 0) {
                state->fsync_fail_count = 0;
                continue;
            }

            state->fsync_fail_count++;
            ESP_LOGE(TAG, "CH%u fsync failed (%" PRIu32 "/3)",
                     channel, state->fsync_fail_count);
            if (state->fsync_fail_count >= 3) {
                state->failed = true;
                recorder_close_channel_locked(channel);
            }
        }
        xSemaphoreGive(s_mutex);
    }
}

esp_err_t recorder_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    s_data_sem = xSemaphoreCreateBinary();
    s_live_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL || s_data_sem == NULL || s_live_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t channel = RECORDER_CHANNEL_1;
         channel <= RECORDER_CHANNEL_2; channel++) {
        recorder_channel_state_t *state = recorder_channel_get(channel);
        state->save_rb.buf = malloc(SAVE_RINGBUF_SIZE);
        if (state->save_rb.buf == NULL) {
            for (uint8_t cleanup = RECORDER_CHANNEL_1; cleanup < channel; cleanup++) {
                free(recorder_channel_get(cleanup)->save_rb.buf);
                recorder_channel_get(cleanup)->save_rb.buf = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
        state->save_rb.size = SAVE_RINGBUF_SIZE;
        ringbuf_reset(&state->save_rb);
    }

    if (xTaskCreate(recorder_io_task, "rec_io", SAVE_TASK_STACK,
                    NULL, SAVE_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(recorder_sync_task, "rec_sync", SYNC_TASK_STACK,
                    NULL, SYNC_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "sync task create failed");
    }

    ESP_LOGI(TAG, "dual recorder ready, channels=%d, buffer=%d bytes/channel",
             RECORDER_CHANNEL_COUNT, SAVE_RINGBUF_SIZE);
    return ESP_OK;
}

bool recorder_is_running(void)
{
    if (s_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool running = s_running;
    xSemaphoreGive(s_mutex);
    return running;
}

uint32_t recorder_get_channel_drop_count(uint8_t channel)
{
    recorder_channel_state_t *state = recorder_channel_get(channel);
    if (state == NULL || s_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t value = state->drop_count;
    xSemaphoreGive(s_mutex);
    return value;
}

uint64_t recorder_get_channel_total_bytes(uint8_t channel)
{
    recorder_channel_state_t *state = recorder_channel_get(channel);
    if (state == NULL || s_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint64_t value = state->rx_total;
    xSemaphoreGive(s_mutex);
    return value;
}

bool recorder_is_channel_file_open(uint8_t channel)
{
    recorder_channel_state_t *state = recorder_channel_get(channel);
    if (state == NULL || s_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool open = state->file != NULL;
    xSemaphoreGive(s_mutex);
    return open;
}

const char *recorder_get_filename(uint8_t channel)
{
    recorder_channel_state_t *state = recorder_channel_get(channel);
    if (state == NULL || state->file == NULL) {
        return NULL;
    }
    /* 只返回文件名部分（去掉路径前缀 "/sdcard/"） */
    const char *prefix = "/sdcard/";
    const char *name = state->filename;
    if (strncmp(name, prefix, strlen(prefix)) == 0) {
        name += strlen(prefix);
    }
    return name;
}

uint32_t recorder_get_drop_count(void)
{
    return recorder_get_channel_drop_count(RECORDER_CHANNEL_1) +
           recorder_get_channel_drop_count(RECORDER_CHANNEL_2);
}

uint64_t recorder_get_total_bytes(void)
{
    return recorder_get_channel_total_bytes(RECORDER_CHANNEL_1) +
           recorder_get_channel_total_bytes(RECORDER_CHANNEL_2);
}

bool recorder_is_file_open(void)
{
    return recorder_is_channel_file_open(RECORDER_CHANNEL_1) ||
           recorder_is_channel_file_open(RECORDER_CHANNEL_2);
}

esp_err_t recorder_force_sync(void)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    recorder_flush_all_locked();
    esp_err_t result = ESP_OK;
    for (uint8_t channel = RECORDER_CHANNEL_1;
         channel <= RECORDER_CHANNEL_2; channel++) {
        recorder_channel_state_t *state = recorder_channel_get(channel);
        if (state->file != NULL) {
            int fd = fileno(state->file);
            if (fflush(state->file) != 0 || fd < 0 || fsync(fd) != 0) {
                state->fsync_fail_count++;
                result = ESP_FAIL;
            }
        }
    }
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t recorder_start(void)
{
    const sd_card_state_t *sd = sd_card_get_state();
    if (!sd->mounted) {
        ESP_LOGE(TAG, "SD card not mounted, can not start recording");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_running) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    for (uint8_t channel = RECORDER_CHANNEL_1;
         channel <= RECORDER_CHANNEL_2; channel++) {
        recorder_channel_state_t *state = recorder_channel_get(channel);
        recorder_close_channel_locked(channel);
        ringbuf_reset(&state->save_rb);
        state->rx_total = 0;
        state->drop_count = 0;
        state->fsync_fail_count = 0;
        state->failed = false;
        esp_err_t filename_err = recorder_compute_filename_locked(channel);
        if (filename_err != ESP_OK) {
            ESP_LOGE(TAG, "CH%u has no available recording filename", channel);
            for (uint8_t cleanup = RECORDER_CHANNEL_1;
                 cleanup <= RECORDER_CHANNEL_2; cleanup++) {
                recorder_channel_get(cleanup)->filename[0] = '\0';
            }
            xSemaphoreGive(s_mutex);
            return filename_err;
        }
        ESP_LOGI(TAG, "CH%u pending file: %s", channel, state->filename);
    }
    s_running = true;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t recorder_stop(void)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_running) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    s_running = false;
    recorder_flush_all_locked();
    for (uint8_t channel = RECORDER_CHANNEL_1;
         channel <= RECORDER_CHANNEL_2; channel++) {
        recorder_close_channel_locked(channel);
    }
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "dual recording stopped");
    return ESP_OK;
}

esp_err_t recorder_write_rx_channel(uint8_t channel,
                                    const uint8_t *data, size_t len)
{
    if (!recorder_channel_valid(channel)) {
        return ESP_ERR_INVALID_ARG;
    }
    recorder_live_push(channel, 0, data, len);
    recorder_write_raw(channel, data, len);
    return ESP_OK;
}

esp_err_t recorder_write_tx_channel(uint8_t channel,
                                    const uint8_t *data, size_t len)
{
    if (!recorder_channel_valid(channel)) {
        return ESP_ERR_INVALID_ARG;
    }
    recorder_live_push(channel, 1, data, len);
    return ESP_OK;
}

esp_err_t recorder_write_rx(const uint8_t *data, size_t len)
{
    return recorder_write_rx_channel(RECORDER_CHANNEL_1, data, len);
}

esp_err_t recorder_write_tx(const uint8_t *data, size_t len)
{
    return recorder_write_tx_channel(RECORDER_CHANNEL_1, data, len);
}

size_t recorder_live_fetch(uint32_t since_seq,
                           recorder_live_item_t *out, size_t max_count,
                           uint32_t *out_max_seq)
{
    if (out_max_seq != NULL) {
        *out_max_seq = s_live_seq;
    }
    if (s_live_mutex == NULL || out == NULL || max_count == 0) {
        return 0;
    }

    xSemaphoreTake(s_live_mutex, portMAX_DELAY);
    size_t got = 0;
    size_t start = s_live_count < RECORDER_LIVE_CAPACITY ? 0 : s_live_head;
    for (size_t i = 0; i < s_live_count && got < max_count; i++) {
        size_t index = (start + i) % RECORDER_LIVE_CAPACITY;
        if (s_live[index].seq > since_seq) {
            out[got++] = s_live[index];
        }
    }
    if (out_max_seq != NULL) {
        *out_max_seq = s_live_seq;
    }
    xSemaphoreGive(s_live_mutex);
    return got;
}

void recorder_live_clear(void)
{
    if (s_live_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_live_mutex, portMAX_DELAY);
    s_live_head = 0;
    s_live_count = 0;
    xSemaphoreGive(s_live_mutex);
}
