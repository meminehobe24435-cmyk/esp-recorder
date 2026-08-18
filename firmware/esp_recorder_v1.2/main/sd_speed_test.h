#ifndef SD_SPEED_TEST_H
#define SD_SPEED_TEST_H

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* 需求：用于评估 SD 卡 SPI 模式下的 FATFS 顺序读写吞吐。
 * 写入和读取都通过 FATFS (fwrite/fread) 完成，反映项目实际使用场景。
 * 文件会落到 /sdcard/sd_speed_test.tmp，测试结束后删除。
 */

typedef struct {
    size_t   block_size;   /* 单次 fwrite/fread 的字节数 */
    size_t   total_bytes;  /* 本次测试实际传输字节数（向下取整到 block_size） */
    uint32_t write_ms;     /* 写入耗时（含 fclose 触发 flush） */
    uint32_t read_ms;      /* 读取耗时 */
    double   write_kbs;    /* 写入速率 KB/s (1 KB = 1024 B) */
    double   read_kbs;     /* 读取速率 KB/s */
    bool     verify_ok;    /* 读回首块校验是否通过 */
} sd_speed_result_t;

/**
 * @brief 单次顺序写入+读取+校验测试
 * @param block_size  单次 I/O 字节数 (1 ~ 65536)
 * @param total_bytes 目标传输字节数 (>= block_size，会向下取整)
 * @param result      输出结果
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 表示 SD 未挂载
 */
esp_err_t sd_speed_test_once(size_t block_size, size_t total_bytes,
                             sd_speed_result_t *result);

/**
 * @brief 运行一组标准块大小测试 (1KB/4KB/16KB/64KB)，结果通过 ESP_LOG 输出
 * @return ESP_OK 全部跑完（个别子项失败不影响后续）
 */
esp_err_t sd_speed_test_run_all(void);

#endif
