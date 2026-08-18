#ifndef DATA_RECORDER_H
#define DATA_RECORDER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* 实时数据流（live stream）：web 端用 /api/stream?since=N 拉取增量。
 *
 * 落盘路径（v2：纯裸字节流）：
 *   - 启动时按时间戳生成文件名（REC_yyyyMMdd_HHmmss[_N].bin），不 fopen
 *   - 第一个 RX 字节到达时才 fopen(filename, "w")
 *   - 后续 RX 字节直接 fwrite 进文件，**无任何文件头、帧头、时间戳**
 *   - save_rb（16KB）解耦生产者/消费者
 *   - 独立 sync task 每秒 fflush+fsync，主写路径不阻塞
 *   - TX 字节**不落盘**，只进 live 显示
 */

#define RECORDER_LIVE_LINE_MAX 160   /* 单条 live 行字节上限（time,dir,hex\n\0） */
#define RECORDER_LIVE_CAPACITY 256   /* live 环形缓冲能容纳的行数 */
#define RECORDER_LIVE_BYTES_FLUSH 32 /* live 节流长度闸门：累计 ≥ N 字节才刷一行 */
#define RECORDER_LIVE_TIME_FLUSH_MS 200 /* live 节流时间闸门：距上次刷 ≥ N ms 才再刷 */

typedef struct {
    uint32_t seq;        /* 单调递增序号（since 查询时使用） */
    uint32_t ts_ms;      /* 本条推入时刻的 pdTICKS_TO_MS 值；前端算 "等待数据..." 用 */
    char     line[RECORDER_LIVE_LINE_MAX];
} recorder_live_item_t;

esp_err_t recorder_init(void);
bool recorder_is_running(void);
esp_err_t recorder_start(void);
esp_err_t recorder_stop(void);

/* 强制 flush + fsync（USB unmount 前等落盘） */
esp_err_t recorder_force_sync(void);

/* 落盘统计 / 状态查询（status_handler 暴露给前端） */
uint32_t recorder_get_drop_count(void);
uint64_t recorder_get_total_bytes(void);
bool     recorder_is_file_open(void);

esp_err_t recorder_write_rx(const uint8_t *data, size_t len);
esp_err_t recorder_write_tx(const uint8_t *data, size_t len);

/* 拉取 live 数据：返回 since_seq 之后新增的条目，out 最多能容纳 max_count 条。
 * 返回实际写入的条数；out_max_seq 输出最大序号（用于下次 since）。 */
size_t recorder_live_fetch(uint32_t since_seq,
                          recorder_live_item_t *out, size_t max_count,
                          uint32_t *out_max_seq);

/* 清空 live 缓冲（用于 Web 端"清空显示"按钮）。 */
void recorder_live_clear(void);

#endif