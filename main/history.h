// Run history, kept in flash (NVS) so it survives reboots. Newest entry is index 0.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "deco.h"

typedef struct {
    uint32_t ts;          // unix time, 0 if the clock was not set yet
    uint16_t duration_s;
    uint8_t verdict;      // deco_verdict_t
    uint8_t flags;        // bit0: optimize requested, bit1: scheduled run
} hist_entry_t;

void history_init(void);
void history_add(const hist_entry_t *e);
int history_count(void);
bool history_get(int idx_newest_first, hist_entry_t *out);
