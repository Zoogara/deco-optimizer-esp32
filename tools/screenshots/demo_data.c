/* Invented data for the screenshots: nothing here comes from a real network, device or person. */
#include <string.h>
#include <time.h>
#include "history.h"
#include "deco.h"

void screensaver_init(void) {}   /* the host renderer has no idle timer */

const char *deco_verdict_str(deco_verdict_t v)
{
    switch (v) {
    case DECO_OPTIMAL: return "optimal";
    case DECO_OPTIMIZABLE: return "optimizable";
    case DECO_OPTIMIZED: return "optimized";
    default: return "error";
    }
}

/* Fixed, made-up dates in March 2026 (UTC; render.c sets TZ=UTC so the output is reproducible). */
uint32_t demo_ts(int day, int hour, int min)
{
    struct tm tm = {.tm_year = 126, .tm_mon = 2, .tm_mday = day, .tm_hour = hour, .tm_min = min};
    return (uint32_t)timegm(&tm);
}

static hist_entry_t s_hist[9];
static int s_count;

void demo_fill_history(void)
{
    /* flags: bit0 optimise requested, bit1 scheduled, bit2 startup */
    hist_entry_t h[] = {
        {demo_ts(14, 3, 0),   12, DECO_OPTIMAL,     2},
        {demo_ts(13, 3, 0),   11, DECO_OPTIMAL,     2},
        {demo_ts(12, 18, 42), 34, DECO_OPTIMIZED,   1},
        {demo_ts(12, 18, 40), 13, DECO_OPTIMIZABLE, 0},
        {demo_ts(12, 3, 0),    0, DECO_ERROR,       2},
        {demo_ts(11, 3, 0),   12, DECO_OPTIMAL,     2},
        {demo_ts(10, 3, 0),   12, DECO_OPTIMAL,     2},
        {demo_ts(9, 3, 0),    11, DECO_OPTIMAL,     2},
    };
    s_count = sizeof h / sizeof h[0];
    memcpy(s_hist, h, sizeof h);
}

int history_count(void) { return s_count; }
bool history_get(int i, hist_entry_t *out)
{
    if (i < 0 || i >= s_count) return false;
    *out = s_hist[i];
    return true;
}
