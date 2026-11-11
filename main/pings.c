#include "pings.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "settings.h"
#include "wifi.h"
#include "deco_nodes.h"
#include "mqtt.h"

static const char *TAG = "pings";
static ping_round_t *s_ring;              // allocated on the first round (the worker is idle then, so no SSH session holds memory)
static int s_head, s_n;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

int pings_count(void) { return s_n; }

bool pings_get(int i, ping_round_t *out)
{
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_ring && i >= 0 && i < s_n) { *out = s_ring[(s_head + PING_ROUNDS - s_n + i) % PING_ROUNDS]; ok = true; }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

static void fill(ping_s_t *s, uint32_t ip, int size)
{
    uint32_t avg = 0, min = 0, lost = 0;
    if (!ip) return;
    s->sent = PING_COUNT_DEV;
    if (!wifi_ping(ip, PING_COUNT_DEV, size, &avg, &min, &lost)) { s->lost = PING_COUNT_DEV; return; }
    s->lost = lost > PING_COUNT_DEV ? PING_COUNT_DEV : lost;
    s->avg = avg > 0xFFFE ? 0xFFFE : avg;
    s->min = min > 0xFFFE ? 0xFFFE : min;
}

void pings_round(void)
{
    if (!g_cfg.ping_every_min || !wifi_is_connected()) return;
    if (!s_ring) s_ring = calloc(PING_ROUNDS, sizeof *s_ring);
    if (!s_ring) { ESP_LOGW(TAG, "no memory for the ping history"); return; }

    ping_round_t r = {0};
    time_t now = time(NULL);
    r.ts = now > 1700000000 ? (uint32_t)now : 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) memcpy(r.bssid, ap.bssid, 6);

    uint32_t ip[PING_SERIES] = {0};
    ip[0] = wifi_gateway_ip();
    deco_node_t n;
    if (deco_node_for_bssid(r.bssid, &n) && n.ip[0]) {
        esp_ip4_addr_t a;
        if (esp_netif_str_to_ip4(n.ip, &a) == ESP_OK) ip[1] = a.addr;
    }
    for (int i = 0; i < PING_MAX_DEV; i++) ip[2 + i] = g_cfg.ping_dev[i].ip;
    int size = g_cfg.ping_size ? g_cfg.ping_size : 1000;
    if (size < 32) size = 32;
    if (size > 1400) size = 1400;

    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < PING_SERIES; i++) fill(&r.s[i], ip[i], size);

    portENTER_CRITICAL(&s_mux);
    s_ring[s_head] = r;
    s_head = (s_head + 1) % PING_ROUNDS;
    if (s_n < PING_ROUNDS) s_n++;
    portEXIT_CRITICAL(&s_mux);

    char line[96]; int k = 0;
    for (int i = 0; i < PING_SERIES && k < (int)sizeof line - 12; i++)
        if (r.s[i].sent) k += snprintf(line + k, sizeof line - k, " %s%u", r.s[i].lost == r.s[i].sent ? "x" : "", r.s[i].lost == r.s[i].sent ? 0 : r.s[i].avg);
    ESP_LOGI(TAG, "round of %d bytes in %d ms, avg ms (x = no reply):%s", size, (int)((esp_timer_get_time() - t0) / 1000), line);
    mqtt_publish_pings(&r);
}
