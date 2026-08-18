#ifndef DATA_RECORDER_H
#define DATA_RECORDER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* 两路实时数据流：Web 端用 /api/stream?since=N 拉取增量。
 *
 * 落盘路径：
 *   - 两路分别生成 REC_CH1_*.bin 和 REC_CH2_*.bin
 *   - 第一个 RX 字节到达时才打开已确认唯一的记录文件
 *   - 每个文件只保存对应通道的 RX 原始字节，无文件头和时间戳
 *   - 两路各有 16KB 缓冲，TX 只进入实时显示
 */

#define RECORDER_CHANNEL_COUNT 2
#define RECORDER_CHANNEL_1     1
#define RECORDER_CHANNEL_2     2

#define RECORDER_LIVE_LINE_MAX 160   /* 单条 live 行：time,channel,dir,hex */
#define RECORDER_LIVE_CAPACITY 256   /* live 环形缓冲能容纳的行数 */

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
uint32_t recorder_get_channel_drop_count(uint8_t channel);
uint64_t recorder_get_channel_total_bytes(uint8_t channel);
bool     recorder_is_channel_file_open(uint8_t channel);

esp_err_t recorder_write_rx(const uint8_t *data, size_t len);
esp_err_t recorder_write_tx(const uint8_t *data, size_t len);
esp_err_t recorder_write_rx_channel(uint8_t channel,
                                    const uint8_t *data, size_t len);
esp_err_t recorder_write_tx_channel(uint8_t channel,
                                    const uint8_t *data, size_t len);

/* 拉取 live 数据：返回 since_seq 之后新增的条目，out 最多能容纳 max_count 条。
 * 返回实际写入的条数；out_max_seq 输出最大序号（用于下次 since）。 */
size_t recorder_live_fetch(uint32_t since_seq,
                          recorder_live_item_t *out, size_t max_count,
                          uint32_t *out_max_seq);

/* 清空 live 缓冲（用于 Web 端"清空显示"按钮）。 */
void recorder_live_clear(void);

#endif
