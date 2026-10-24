// Deco network-optimization client (TMP App-V2 over an SSH direct-tcpip tunnel). See PROTOCOL.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DECO_OPTIMAL,       // scan found nothing to improve
    DECO_OPTIMIZABLE,   // report mode: scan found improvements, none applied
    DECO_OPTIMIZED,     // optimize ran and the Deco now reports optimal
    DECO_ERROR,
} deco_verdict_t;

typedef struct {
    deco_verdict_t verdict;
    bool optimize_requested;
    uint32_t duration_s;
    char detail[40];     // short human-readable note, e.g. the error text
} deco_result_t;

typedef void (*deco_progress_cb)(const char *msg);

// Blocking; run from a task with >= 16 KB stack. Wi-Fi must already be started.
void deco_run(bool optimize, deco_result_t *out, deco_progress_cb progress);
const char *deco_verdict_str(deco_verdict_t v);
