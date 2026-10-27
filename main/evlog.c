#include "evlog.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "evlog";

/* ---------- RAM ring: everything the firmware logs ---------- */

static char s_ring[EVLOG_RAM_SIZE];
static size_t s_head, s_len;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;

static void ring_put(const char *p, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < n; i++) { s_ring[s_head] = p[i]; s_head = (s_head + 1) % EVLOG_RAM_SIZE; }
    s_len = s_len + n > EVLOG_RAM_SIZE ? EVLOG_RAM_SIZE : s_len + n;
    portEXIT_CRITICAL(&s_mux);
}

// Small stack use on purpose: logging happens from tasks with little stack (the Wi-Fi event task has about 2 KB).
static int log_hook(const char *fmt, va_list ap)
{
    char line[128];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof line, fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        if (n >= (int)sizeof line) { n = sizeof line - 1; line[n - 1] = '\n'; }
        size_t m = 0;
        for (int i = 0; i < n; i++) {                 // drop ANSI colour codes
            if (line[i] == 0x1b && i + 1 < n && line[i + 1] == '[') { i += 2; while (i < n && line[i] != 'm') i++; continue; }
            line[m++] = line[i];
        }
        ring_put(line, m);
    }
    return s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
}

void evlog_init(void) { s_prev = esp_log_set_vprintf(log_hook); }

size_t evlog_ram_copy(char *out, size_t cap)
{
    if (cap < EVLOG_RAM_SIZE + 1) { if (cap) out[0] = 0; return 0; }
    portENTER_CRITICAL(&s_mux);
    size_t len = s_len, start = (s_head + EVLOG_RAM_SIZE - len) % EVLOG_RAM_SIZE;
    size_t first = EVLOG_RAM_SIZE - start < len ? EVLOG_RAM_SIZE - start : len;
    memcpy(out, s_ring + start, first);
    memcpy(out + first, s_ring, len - first);
    portEXIT_CRITICAL(&s_mux);
    out[len] = 0;
    if (len == EVLOG_RAM_SIZE) {                       // wrapped: the first line is probably cut, so skip it
        char *nl = strchr(out, '\n');
        if (nl) { size_t skip = nl + 1 - out; memmove(out, nl + 1, len - skip + 1); len -= skip; }
    }
    return len;
}

/* ---------- persistent events (encrypted NVS) ---------- */

#define MAX_WRITES_PER_HOUR 30
static SemaphoreHandle_t s_lock;
static uint32_t s_write_up[MAX_WRITES_PER_HOUR];       // uptime (s) of the most recent writes
static int s_write_idx;
static unsigned s_suppressed;

static bool rate_ok(uint32_t up)
{
    uint32_t oldest = s_write_up[s_write_idx];
    if (oldest && up - oldest < 3600) return false;    // 30 writes already within the last hour
    s_write_up[s_write_idx] = up ? up : 1;
    s_write_idx = (s_write_idx + 1) % MAX_WRITES_PER_HOUR;
    return true;
}

void evlog_event(const char *fmt, ...)
{
    char text[EVLOG_EVENT_TEXT];
    va_list ap; va_start(ap, fmt); vsnprintf(text, sizeof text, fmt, ap); va_end(ap);
    ESP_LOGI(TAG, "event: %s", text);                  // also lands in the RAM log

    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) return;
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    if (!rate_ok(up)) {                                // protects the flash if something loops
        s_suppressed++;
        xSemaphoreGive(s_lock);
        return;
    }
    time_t now = time(NULL);
    char rec[EVLOG_EVENT_TEXT + 24];
    snprintf(rec, sizeof rec, "%u|%u|%s", (unsigned)(now > 1700000000 ? now : 0), (unsigned)up, text);
    nvs_handle_t h;
    if (nvs_open("evlog", NVS_READWRITE, &h) == ESP_OK) {
        uint32_t n = 0;
        nvs_get_u32(h, "n", &n);
        char key[8]; snprintf(key, sizeof key, "e%02u", (unsigned)(n % EVLOG_EVENT_SLOTS));
        if (nvs_set_str(h, key, rec) == ESP_OK && nvs_set_u32(h, "n", n + 1) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_lock);
}

int evlog_events_read(evlog_event_t *out, int max)
{
    nvs_handle_t h;
    if (nvs_open("evlog", NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t n = 0;
    nvs_get_u32(h, "n", &n);
    int have = n < EVLOG_EVENT_SLOTS ? (int)n : EVLOG_EVENT_SLOTS, count = 0;
    for (int back = 0; back < have && count < max; back++) {
        char key[8], rec[EVLOG_EVENT_TEXT + 24];
        size_t len = sizeof rec;
        snprintf(key, sizeof key, "e%02u", (unsigned)((n - 1 - back) % EVLOG_EVENT_SLOTS));
        if (nvs_get_str(h, key, rec, &len) != ESP_OK) continue;
        unsigned ts = 0, up = 0; int off = 0;
        if (sscanf(rec, "%u|%u|%n", &ts, &up, &off) < 2 || off == 0) continue;
        out[count].ts = ts; out[count].uptime_s = up;
        strlcpy(out[count].text, rec + off, sizeof out[count].text);
        count++;
    }
    nvs_close(h);
    return count;
}
