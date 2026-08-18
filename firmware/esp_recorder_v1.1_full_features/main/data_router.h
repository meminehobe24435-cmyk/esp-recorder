#ifndef DATA_ROUTER_H
#define DATA_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool enabled;
    uint64_t ch1_to_ch2_bytes;
    uint64_t ch2_to_ch1_bytes;
    uint64_t ch1_to_ch2_drop_bytes;
    uint64_t ch2_to_ch1_drop_bytes;
} data_router_stats_t;

esp_err_t data_router_init(bool enabled);
void data_router_set_enabled(bool enabled);
int data_router_forward(uint8_t source_channel, const uint8_t *data, size_t len);
void data_router_get_stats(data_router_stats_t *stats);

#endif
