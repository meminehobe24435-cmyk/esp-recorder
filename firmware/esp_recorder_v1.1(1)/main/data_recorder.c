#include "data_recorder.h"

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sd_card.h"

#define TAG "recorder"

#define RECORDER_DIR            "/sdcard"

/* ---------------- 落盘缓冲 / 任务 ---------------- */

/* 16KB 给满载（460800 → ~50KB/s 二进制写入）提供 ~300ms 背压缓冲 */
#define SAVE_RINGBUF_SIZE       16384

#define SAVE_THRESHOLD_BYTES    2048
#define SAVE_TIMEOUT_MS         1000

#define SAVE_TASK_STACK         4096
#define SAVE_TASK_PRIORITY      5

#define SYNC_TASK_STACK         2048
#define SYNC_TASK_PRIORITY      1

#define FILE_NAME_SIZE          96

#define LIVE_BYTES_FLUSH        RECORDER_LIVE_BYTES_FLUSH
#define LIVE_TIME_FLUSH_MS      RECORDER_LIVE_TIME_FLUSH_MS

typedef struct {
    uint8_t *buf;
    size_t   size;
    size_t   head;
    size_t   tail;
} ringbuf_t;

static bool     s_running = false;
static FILE    *s_file = NULL;
static char     s_filename[FILE_NAME_SIZE] = {0};

static SemaphoreHandle_t s_mutex = NULL;
static SemaphoreHandle_t s_data_sem = NULL;
static ringbuf_t         s_save_rb = {0};
static uint64_t s_rx_total = 0;          /* 落盘成功写入的字节数（纯 payload） */
static uint32_t s_drop_count = 0;        /* 背压溢出丢的字节数 */
static TickType_t s_last_write_tick = 0;

/* fsync 失败计数；连续 3 次失败 → 关闭文件、停录制 */
static uint32_t s_fsync_fail_count = 0;

/* ---------------- Live 环形缓冲（Web 实时显示） ---------------- */

static recorder_live_item_t s_live[RECORDER_LIVE_CAPACITY] = {0};
static size_t s_live_head = 0;
static size_t s_live_count = 0;
static uint32_t s_live_seq = 0;
static SemaphoreHandle_t s_live_mutex = NULL;

/* live 节流缓冲 */
static uint8_t  s_live_buf[64];
static size_t   s_live_buf_len = 0;
static uint8_t  s_live_buf_dir = 0;
static uint32_t s_live_last_push_ms = 0;

/* ---------------- 环形缓冲操作 ---------------- */

static inline size_t ringbuf_count(const ringbuf_t *rb)
{
    return (rb->head + rb->size - rb->tail) % rb->size;
}

static inline size_t ringbuf_free(const ringbuf_t *rb)
{
    return (rb->tail + rb->size - rb->head - 1) % rb->size;
}

static size_t ringbuf_write(ringbuf_t *rb, const uint8_t *data, size_t len)
{
    size_t fr = ringbuf_free(rb);
    if (len > fr) {
        len = fr;
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
    rb->head = rb->tail = 0;
}

/* ---------------- 时间戳 ----------------
 * 落盘路径不再使用时间戳，但 live 显示仍需要 ISO 字符串与 ms tick。 */

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

/* ---------------- 文件操作 ---------------- */

static void recorder_close_file(void)
{
    if (s_file == NULL) {
        return;
    }
    fflush(s_file);
    int fd = fileno(s_file);
    if (fd >= 0) {
        fsync(fd);
    }
    fclose(s_file);
    s_file = NULL;
    ESP_LOGI(TAG, "file closed: %s, rx=%" PRIu64 " drop=%" PRIu32,
             s_filename, s_rx_total, s_drop_count);
}

/* 启动时调用：按当前时间戳生成文件名，做 stat 重名检查 + _N 后缀，
 * 写进 s_filename，**但不 fopen**。第一个字节到达时才实际打开。 */
static void recorder_compute_pending_filename(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_info;
    localtime_r(&tv.tv_sec, &tm_info);

    for (int suffix = 0; suffix < 100; suffix++) {
        if (suffix == 0) {
            snprintf(s_filename, sizeof(s_filename),
                     "%s/REC_%04d%02d%02d_%02d%02d%02d.bin",
                     RECORDER_DIR,
                     tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                     tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        } else {
            snprintf(s_filename, sizeof(s_filename),
                     "%s/REC_%04d%02d%02d_%02d%02d%02d_%d.bin",
                     RECORDER_DIR,
                     tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                     tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
                     suffix);
        }
        struct stat st;
        if (stat(s_filename, &st) != 0) {
            break;
        }
    }
}

/* 把 save_rb 里的数据写到文件。**不调 fsync**，主写路径必须不能阻塞。 */
static void recorder_flush_save_rb(void)
{
    if (!s_running) {
        return;
    }

    const sd_card_state_t *state = sd_card_get_state();
    if (!state->mounted) {
        recorder_close_file();
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_file == NULL) {
        xSemaphoreGive(s_mutex);
        return;
    }

    size_t count = ringbuf_count(&s_save_rb);
    if (count == 0) {
        xSemaphoreGive(s_mutex);
        return;
    }

    uint8_t tmp[1024];
    size_t written_total = 0;
    while ((count = ringbuf_count(&s_save_rb)) > 0) {
        size_t to_read = (count > sizeof(tmp)) ? sizeof(tmp) : count;
        ringbuf_read(&s_save_rb, tmp, to_read);
        size_t written = fwrite(tmp, 1, to_read, s_file);
        if (written != to_read) {
            ESP_LOGE(TAG, "fwrite failed: %d/%d", (int)written, (int)to_read);
            s_drop_count += (uint32_t)(to_read - written);
            break;
        }
        written_total += written;
    }

    fflush(s_file);   /* 不调 fsync */
    s_last_write_tick = xTaskGetTickCount();
    xSemaphoreGive(s_mutex);

    (void)written_total;
}

/* 把字节流压入 save_rb。**空间不足时直接丢弃**，不再自旋。 */
static void recorder_push_bin(const uint8_t *data, size_t len)
{
    if (len == 0) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t fr = ringbuf_free(&s_save_rb);
    if (fr >= len) {
        ringbuf_write(&s_save_rb, data, len);
        s_rx_total += len;
    } else {
        /* 丢当前批；按"尽力写入"语义，把能塞的部分塞进去，剩下的算丢 */
        if (fr > 0) {
            ringbuf_write(&s_save_rb, data, fr);
            s_rx_total += fr;
        }
        s_drop_count += (uint32_t)(len - fr);
        /* 唤醒 IO task 尽快 flush 出空间 */
    }
    xSemaphoreGive(s_mutex);

    xSemaphoreGive(s_data_sem);
}

/* ---------------- Live 显示 ---------------- */

/* 把累积缓冲里的字节格式化成 "ts,dir,HEX\n" 写进 live ring */
static void recorder_live_emit(uint32_t now_ms)
{
    if (s_live_buf_len == 0) {
        s_live_last_push_ms = now_ms;
        return;
    }

    char ts[32];
    format_iso_timestamp(ts, sizeof(ts));
    const char *dir_str = (s_live_buf_dir == 0) ? "RX" : "TX";

    xSemaphoreTake(s_live_mutex, portMAX_DELAY);

    recorder_live_item_t *slot = &s_live[s_live_head];
    slot->seq = ++s_live_seq;
    slot->ts_ms = now_ms;
    int n = snprintf(slot->line, RECORDER_LIVE_LINE_MAX, "%s,%s,", ts, dir_str);
    if (n < 0 || (size_t)n >= RECORDER_LIVE_LINE_MAX) {
        slot->line[0] = '\0';
        s_live_head = (s_live_head + 1) % RECORDER_LIVE_CAPACITY;
        if (s_live_count < RECORDER_LIVE_CAPACITY) s_live_count++;
        xSemaphoreGive(s_live_mutex);
        s_live_last_push_ms = now_ms;
        s_live_buf_len = 0;
        return;
    }
    size_t pos = (size_t)n;
    for (size_t i = 0; i < s_live_buf_len; i++) {
        if (pos + 3 >= RECORDER_LIVE_LINE_MAX) break;
        pos += (size_t)snprintf(slot->line + pos, RECORDER_LIVE_LINE_MAX - pos, "%02X", s_live_buf[i]);
    }
    if (pos + 1 < RECORDER_LIVE_LINE_MAX) {
        slot->line[pos++] = '\n';
        slot->line[pos] = '\0';
    } else {
        slot->line[RECORDER_LIVE_LINE_MAX - 1] = '\0';
    }

    s_live_head = (s_live_head + 1) % RECORDER_LIVE_CAPACITY;
    if (s_live_count < RECORDER_LIVE_CAPACITY) {
        s_live_count++;
    }
    xSemaphoreGive(s_live_mutex);

    s_live_buf_len = 0;
    s_live_last_push_ms = now_ms;
}

/* live 写入入口（双闸门：长度 + 时间） */
static void recorder_live_push(uint8_t dir, const uint8_t *data, size_t len)
{
    if (s_live_mutex == NULL || len == 0) return;

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * (1000 / configTICK_RATE_HZ));

    /* 首字节：如果当前 dir 与累积 dir 不同，先 flush 一行再切 */
    if (s_live_buf_len > 0 && s_live_buf_dir != dir) {
        recorder_live_emit(now_ms);
    }
    s_live_buf_dir = dir;

    /* 累积字节到 s_live_buf，超过 64B 截断 */
    size_t remain = sizeof(s_live_buf) - s_live_buf_len;
    size_t copy = (len > remain) ? remain : len;
    memcpy(s_live_buf + s_live_buf_len, data, copy);
    s_live_buf_len += copy;
    /* 超过的部分直接丢弃 —— live 不要求精确字节，仅预览 */

    /* 双闸门判定 */
    bool time_due = (now_ms - s_live_last_push_ms) >= LIVE_TIME_FLUSH_MS;
    bool len_due  = s_live_buf_len >= LIVE_BYTES_FLUSH;
    if (time_due || len_due) {
        recorder_live_emit(now_ms);
    }
}

/* ---------------- 落盘写入入口 ---------------- */

/* 把 raw 字节推进 save_rb。首次字节时 fopen 文件。
 * 整个链路不写任何 header、帧头、时间戳——文件就是纯裸字节流。 */
static void recorder_write_raw(const uint8_t *data, size_t len)
{
    if (!s_running || len == 0) return;

    /* 首次字节：原子地检查并打开文件 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_file == NULL) {
        s_file = fopen(s_filename, "w");
        if (s_file == NULL) {
            s_drop_count += (uint32_t)len;
            xSemaphoreGive(s_mutex);
            xSemaphoreGive(s_data_sem);   /* 仍然唤醒 IO task 处理后续 */
            return;
        }
        s_last_write_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "file opened: %s", s_filename);
    }
    xSemaphoreGive(s_mutex);

    /* 字节直接进 save_rb（不切片、不带任何 meta） */
    recorder_push_bin(data, len);
}

/* ---------------- 任务 ---------------- */

static void recorder_io_task(void *arg)
{
    (void)arg;
    while (1) {
        BaseType_t notified = xSemaphoreTake(s_data_sem, pdMS_TO_TICKS(SAVE_TIMEOUT_MS));
        if (notified == pdTRUE) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            size_t save_count = ringbuf_count(&s_save_rb);
            xSemaphoreGive(s_mutex);
            if (save_count >= SAVE_THRESHOLD_BYTES) {
                recorder_flush_save_rb();
            }
        } else {
            /* 超时：flush 剩余数据，但**不关文件**（长会话保活） */
            recorder_flush_save_rb();
        }
    }
}

/* 独立低优 fsync 任务：每秒一次 flush + fsync，不阻塞 IO task */
static void recorder_sync_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_file != NULL) {
            fflush(s_file);
            int fd = fileno(s_file);
            int rc = (fd >= 0) ? fsync(fd) : -1;
            if (rc != 0) {
                s_fsync_fail_count++;
                ESP_LOGE(TAG, "fsync failed (count=%" PRIu32 ")", s_fsync_fail_count);
                if (s_fsync_fail_count >= 3) {
                    ESP_LOGE(TAG, "fsync 3 consecutive failures, closing file");
                    fclose(s_file);
                    s_file = NULL;
                    s_running = false;
                }
            } else {
                s_fsync_fail_count = 0;
            }
        }
        xSemaphoreGive(s_mutex);
    }
}

/* ---------------- 公共接口 ---------------- */

esp_err_t recorder_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;

    s_data_sem = xSemaphoreCreateBinary();
    if (s_data_sem == NULL) return ESP_ERR_NO_MEM;

    s_live_mutex = xSemaphoreCreateMutex();
    if (s_live_mutex == NULL) return ESP_ERR_NO_MEM;

    s_save_rb.buf = (uint8_t *)malloc(SAVE_RINGBUF_SIZE);
    if (s_save_rb.buf == NULL) return ESP_ERR_NO_MEM;
    s_save_rb.size = SAVE_RINGBUF_SIZE;
    ringbuf_reset(&s_save_rb);

    if (xTaskCreate(recorder_io_task, "rec_io", SAVE_TASK_STACK,
                    NULL, SAVE_TASK_PRIORITY, NULL) != pdPASS) {
        free(s_save_rb.buf);
        s_save_rb.buf = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(recorder_sync_task, "rec_sync", SYNC_TASK_STACK,
                    NULL, SYNC_TASK_PRIORITY, NULL) != pdPASS) {
        /* sync task 创建失败不影响落盘，只是 fsync 退化为不调用（依赖文件系统 close 兜底） */
        ESP_LOGE(TAG, "sync task create failed");
    }

    s_live_last_push_ms = (uint32_t)(xTaskGetTickCount() * (1000 / configTICK_RATE_HZ));

    ESP_LOGI(TAG, "recorder init (bin), save_rb=%d threshold=%d timeout=%dms sync=1s",
             (int)SAVE_RINGBUF_SIZE, (int)SAVE_THRESHOLD_BYTES, (int)SAVE_TIMEOUT_MS);
    return ESP_OK;
}

bool recorder_is_running(void)
{
    return s_running;
}

uint32_t recorder_get_drop_count(void)
{
    return s_drop_count;
}

uint64_t recorder_get_total_bytes(void)
{
    return s_rx_total;
}

bool recorder_is_file_open(void)
{
    return s_file != NULL;
}

esp_err_t recorder_force_sync(void)
{
    recorder_flush_save_rb();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_file != NULL) {
        fflush(s_file);
        int fd = fileno(s_file);
        if (fd >= 0) fsync(fd);
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t recorder_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    const sd_card_state_t *state = sd_card_get_state();
    if (!state->mounted) {
        ESP_LOGE(TAG, "SD card not mounted, can not start recording");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ringbuf_reset(&s_save_rb);
    s_rx_total = 0;
    s_drop_count = 0;
    s_fsync_fail_count = 0;
    s_file = NULL;
    recorder_compute_pending_filename();         /* 算文件名，不 fopen */
    s_running = true;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "recording started, pending file: %s", s_filename);
    return ESP_OK;
}

esp_err_t recorder_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_running = false;
    xSemaphoreGive(s_mutex);

    /* 最后一次 flush + fsync + close */
    recorder_force_sync();
    recorder_close_file();

    if (s_drop_count > 0) {
        ESP_LOGW(TAG, "recording stopped, dropped %lu bytes during run", (unsigned long)s_drop_count);
    }
    ESP_LOGI(TAG, "recording stopped, total bytes %" PRIu64, s_rx_total);
    return ESP_OK;
}

esp_err_t recorder_write_rx(const uint8_t *data, size_t len)
{
    recorder_live_push(0, data, len);  /* 0 = RX */
    recorder_write_raw(data, len);
    return ESP_OK;
}

esp_err_t recorder_write_tx(const uint8_t *data, size_t len)
{
    /* TX 只进 live 显示，不落盘 */
    recorder_live_push(1, data, len);  /* 1 = TX */
    return ESP_OK;
}

size_t recorder_live_fetch(uint32_t since_seq,
                          recorder_live_item_t *out, size_t max_count,
                          uint32_t *out_max_seq)
{
    if (s_live_mutex == NULL || out == NULL || max_count == 0) {
        if (out_max_seq) *out_max_seq = s_live_seq;
        return 0;
    }
    size_t got = 0;
    uint32_t max_seq = since_seq;
    xSemaphoreTake(s_live_mutex, portMAX_DELAY);
    size_t start = (s_live_count < RECORDER_LIVE_CAPACITY) ? 0 : s_live_head;
    for (size_t i = 0; i < s_live_count && got < max_count; i++) {
        size_t idx = (start + i) % RECORDER_LIVE_CAPACITY;
        recorder_live_item_t *it = &s_live[idx];
        if (it->seq <= since_seq) continue;
        out[got++] = *it;
        if (it->seq > max_seq) max_seq = it->seq;
    }
    xSemaphoreGive(s_live_mutex);
    if (out_max_seq) *out_max_seq = max_seq;
    return got;
}

void recorder_live_clear(void)
{
    if (s_live_mutex == NULL) return;
    xSemaphoreTake(s_live_mutex, portMAX_DELAY);
    s_live_head = 0;
    s_live_count = 0;
    xSemaphoreGive(s_live_mutex);
    s_live_buf_len = 0;
    s_live_last_push_ms = (uint32_t)(xTaskGetTickCount() * (1000 / configTICK_RATE_HZ));
}