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
#include "evlog.h"

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

// Outage tracking: a target counts as unreachable after 2 rounds in a row with no reply at all (one lost
// round is only noted in the detailed log). Down and up are written to the persistent Events log.
#define DOWN_AFTER 2
static uint8_t s_miss[PING_SERIES];        // consecutive rounds with no reply
static uint32_t s_down_ts[PING_SERIES];    // time of the first lost round of the current outage
static bool s_quiet[PING_SERIES];          // outage was reported as part of a gateway outage
static bool s_started;

static void series_name(int i, char *out, size_t n)
{
    if (i == 0) snprintf(out, n, "gateway");
    else if (i == 1) snprintf(out, n, "connected node");
    else snprintf(out, n, "%s", g_cfg.ping_dev[i - 2].label[0] ? g_cfg.ping_dev[i - 2].label : "device");
}

static void fmt_dur(uint32_t s, char *out, size_t n)
{
    if (s < 3600) snprintf(out, n, "%u min", (unsigned)((s + 30) / 60));
    else snprintf(out, n, "%u h %u min", (unsigned)(s / 3600), (unsigned)((s % 3600 + 30) / 60));
}

static void track(const ping_round_t *r, const uint32_t *ip)
{
    char nm[40], ctx[64] = "", dur[24];
    bool gw_down = false, gw_up = false;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        char lab[40]; deco_node_label(r->bssid, lab, sizeof lab);
        snprintf(ctx, sizeof ctx, " (on %s, %d dBm)", lab, ap.rssi);
    }
    for (int i = 0; i < PING_SERIES; i++) {            // gateway first so it can speak for the rest
        int j = i;
        if (!ip[j] || !r->s[j].sent) { s_miss[j] = 0; s_quiet[j] = false; continue; }
        bool lost = r->s[j].lost == r->s[j].sent;
        series_name(j, nm, sizeof nm);
        if (lost) {
            if (s_miss[j] == 0) s_down_ts[j] = r->ts;
            if (s_miss[j] < 250) s_miss[j]++;
            if (s_miss[j] < DOWN_AFTER) ESP_LOGW(TAG, "%s: no reply this round", nm);
            else if (s_miss[j] == DOWN_AFTER) {
                if (j == 0) { gw_down = true; evlog_event("ping: gateway unreachable for %d rounds, so the Wi-Fi or LAN link is down%s", DOWN_AFTER, ctx); }
                else if (gw_down || s_miss[0] >= DOWN_AFTER) { s_quiet[j] = true; ESP_LOGW(TAG, "%s: unreachable too (gateway is down)", nm); }
                else evlog_event("ping: %s unreachable for %d rounds in a row%s", nm, DOWN_AFTER, ctx);
            }
        } else {
            if (s_miss[j] >= DOWN_AFTER && !(s_quiet[j] && gw_up)) {
                fmt_dur(r->ts && s_down_ts[j] ? r->ts - s_down_ts[j] : (uint32_t)s_miss[j] * g_cfg.ping_every_min * 60, dur, sizeof dur);
                evlog_event("ping: %s reachable again after about %s%s", nm, dur, j == 0 ? ctx : "");
                if (j == 0) gw_up = true;
            }
            s_miss[j] = 0; s_quiet[j] = false;
        }
    }
}

void pings_round(void)
{
    if (!g_cfg.ping_every_min) return;
    if (!wifi_is_connected()) { ESP_LOGW(TAG, "ping round skipped: Wi-Fi is not connected"); return; }
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
    if (!s_started) { s_started = true; evlog_event("ping: monitoring started, history begins now (it is kept in RAM and starts empty after a restart)"); }
    track(&r, ip);
    mqtt_publish_pings(&r);
}
