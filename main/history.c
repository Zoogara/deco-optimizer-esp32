#include "history.h"
#include <string.h>
#include "nvs.h"
#include "config.h"

#define NS "hist"

static hist_entry_t s_buf[HISTORY_MAX];   // s_buf[0] is the newest
static int s_count;

void history_init(void)
{
    nvs_handle_t h;
    s_count = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof s_buf;
    if (nvs_get_blob(h, "log", s_buf, &len) == ESP_OK) s_count = len / sizeof(hist_entry_t);
    nvs_close(h);
}

void history_add(const hist_entry_t *e)
{
    memmove(&s_buf[1], &s_buf[0], (HISTORY_MAX - 1) * sizeof(hist_entry_t));
    s_buf[0] = *e;
    if (s_count < HISTORY_MAX) s_count++;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "log", s_buf, s_count * sizeof(hist_entry_t));
        nvs_commit(h);
        nvs_close(h);
    }
}

int history_count(void) { return s_count; }

bool history_get(int i, hist_entry_t *out)
{
    if (i < 0 || i >= s_count) return false;
    *out = s_buf[i];
    return true;
}
