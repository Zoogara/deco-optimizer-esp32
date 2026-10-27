// Wi-Fi station for a multi-node Deco mesh. The join/measure/avoid logic is adapted from the
// gcal_display_esp32s3 project, where the same failure was diagnosed on this network: a node stays
// associated (no disconnect event) but stops passing traffic ("no route to host", unpingable).
#include "wifi.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "ping/ping_sock.h"
#include "settings.h"
#include "evlog.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_eg;
#define GOT_IP BIT0
static int s_retries;
static bool s_started;

/* Pinned to one node by wifi_check_link(): reconnects go to that BSSID only. Released after
 * UNPIN_AFTER_FAILS failed reconnects, so a pinned node that disappears cannot strand the device. */
static bool s_pinned;
#define UNPIN_AFTER_FAILS 3

static void unpin(void)
{
    if (!s_pinned) return;
    wifi_config_t c;
    if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK) { c.sta.bssid_set = false; esp_wifi_set_config(WIFI_IF_STA, &c); }
    s_pinned = false;
    ESP_LOGI(TAG, "no longer pinned to a mesh node");
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_eg, GOT_IP);
        if (s_pinned && s_retries + 1 >= UNPIN_AFTER_FAILS) unpin();
        s_retries++;
        ESP_LOGW(TAG, "dropped, reconnecting (%d)", s_retries);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retries = 0;
        xEventGroupSetBits(s_eg, GOT_IP);
        if (!g_cfg.wifi_verified) { g_cfg.wifi_verified = true; settings_save(); }   // credentials proved good
        const ip_event_got_ip_t *got = data;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            ESP_LOGI(TAG, "got ip " IPSTR ", node " MACSTR " ch %d rssi %d", IP2STR(&got->ip_info.ip), MAC2STR(ap.bssid), ap.primary, ap.rssi);
    }
}

void wifi_start(void)
{
    s_eg = xEventGroupCreate();
    if (!settings_wifi_configured()) { ESP_LOGW(TAG, "no Wi-Fi configured"); return; }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, g_cfg.wifi_ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, g_cfg.wifi_pass, sizeof wc.sta.password);
    // ESP-IDF defaults to fast scan (first node heard); scan every channel and take the strongest instead.
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    // The settings module is the only place secrets live; do not let the Wi-Fi driver persist its own copy.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Mains powered: no modem sleep. On the gcal display, power save caused missed beacons and dropped transfers.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    s_started = true;
}

bool wifi_wait_connected(uint32_t timeout_ms)
{
    return xEventGroupWaitBits(s_eg, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & GOT_IP;
}

bool wifi_is_connected(void) { return s_eg && (xEventGroupGetBits(s_eg) & GOT_IP); }

void wifi_kick(void)
{
    if (!s_started) return;
    // The disconnect handler reconnects, and the reconnect rescans all channels, so this may well
    // land on a different node from the one that stopped passing traffic.
    evlog_event("wifi: forced reconnect");
    unpin();
    xEventGroupClearBits(s_eg, GOT_IP);
    esp_wifi_disconnect();
}

/* ---------- link quality: find a mesh node that actually works ---------- */

#define PING_COUNT     5
#define BAD_AVG_MS     150
#define BAD_LOST       2              /* this many of PING_COUNT lost = bad */
#define AVOID_US       (60LL * 60 * 1000000)
#define AVOID_SLOTS    4
#define REJOIN_WAIT_MS 20000

typedef struct { uint8_t bssid[6]; int64_t until_us; } avoid_t;
static avoid_t s_avoid[AVOID_SLOTS];

typedef struct { SemaphoreHandle_t done; uint32_t replies, total_ms; } ping_result_t;

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    ping_result_t *r = args; uint32_t gap = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &gap, sizeof gap);
    r->replies++; r->total_ms += gap;
}
static void on_ping_end(esp_ping_handle_t hdl, void *args) { xSemaphoreGive(((ping_result_t *)args)->done); }

static bool ping_gateway(uint32_t *avg_ms, uint32_t *lost)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.gw.addr == 0) return false;
    ping_result_t r = {.done = xSemaphoreCreateBinary()};
    if (!r.done) return false;
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    pc.count = PING_COUNT; pc.interval_ms = 200; pc.timeout_ms = 1000; pc.data_size = 32;
    pc.target_addr.type = IPADDR_TYPE_V4;
    pc.target_addr.u_addr.ip4.addr = ip.gw.addr;
    esp_ping_callbacks_t cbs = {.cb_args = &r, .on_ping_success = on_ping_success, .on_ping_end = on_ping_end};
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&pc, &cbs, &ping) != ESP_OK) { vSemaphoreDelete(r.done); return false; }
    esp_ping_start(ping);
    xSemaphoreTake(r.done, pdMS_TO_TICKS(PING_COUNT * 1500 + 2000));
    esp_ping_stop(ping);
    esp_ping_delete_session(ping);
    vSemaphoreDelete(r.done);
    *lost = PING_COUNT - r.replies;
    *avg_ms = r.replies ? r.total_ms / r.replies : 0;
    return true;
}

static bool avoided(const uint8_t *bssid, int64_t now)
{
    for (int i = 0; i < AVOID_SLOTS; i++)
        if (s_avoid[i].until_us > now && !memcmp(s_avoid[i].bssid, bssid, 6)) return true;
    return false;
}

static void avoid(const uint8_t *bssid, int64_t now)
{
    int slot = 0;
    for (int i = 0; i < AVOID_SLOTS; i++) {
        if (s_avoid[i].until_us <= now || !memcmp(s_avoid[i].bssid, bssid, 6)) { slot = i; break; }
        if (s_avoid[i].until_us < s_avoid[slot].until_us) slot = i;   /* else reuse the one expiring soonest */
    }
    memcpy(s_avoid[slot].bssid, bssid, 6);
    s_avoid[slot].until_us = now + AVOID_US;
}

/* Scans for the SSID and pins to the strongest node that is not avoided. True if it switched. */
static bool switch_node(void)
{
    wifi_config_t c;
    if (esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK) return false;
    wifi_scan_config_t sc = {.ssid = c.sta.ssid, .show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) { ESP_LOGW(TAG, "scan failed"); return false; }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return false;
    if (n > 12) n = 12;
    wifi_ap_record_t *aps = malloc(n * sizeof *aps);
    if (!aps) { esp_wifi_clear_ap_list(); return false; }
    esp_wifi_scan_get_ap_records(&n, aps);
    int64_t now = esp_timer_get_time();
    int best = -1;
    for (int i = 0; i < n; i++) {
        bool bad = avoided(aps[i].bssid, now);
        ESP_LOGI(TAG, "  node " MACSTR " ch %d rssi %d%s", MAC2STR(aps[i].bssid), aps[i].primary, aps[i].rssi, bad ? " (avoided)" : "");
        if (!bad && (best < 0 || aps[i].rssi > aps[best].rssi)) best = i;
    }
    bool switched = false;
    if (best >= 0) {
        evlog_event("wifi: moving to node " MACSTR " (rssi %d)", MAC2STR(aps[best].bssid), aps[best].rssi);
        memcpy(c.sta.bssid, aps[best].bssid, 6);
        c.sta.bssid_set = true;
        esp_wifi_set_config(WIFI_IF_STA, &c);
        s_pinned = true;
        s_retries = 0;
        // Clear here too: esp_wifi_disconnect() returns before the event arrives, so a caller waiting
        // for the new connection would otherwise see the old connection's bit and return at once.
        xEventGroupClearBits(s_eg, GOT_IP);
        esp_wifi_disconnect();   /* the event handler reconnects, to that node now */
        switched = true;
    } else {
        ESP_LOGW(TAG, "no other node for the SSID - staying put");
    }
    free(aps);
    return switched;
}

bool wifi_check_link(void)
{
    if (!s_started || !wifi_is_connected()) return true;   /* the normal reconnect logic handles that */
    uint32_t avg, lost;
    if (!ping_gateway(&avg, &lost)) return true;
    wifi_ap_record_t ap;
    bool have_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    bool bad = lost >= BAD_LOST || avg > BAD_AVG_MS;
    ESP_LOGI(TAG, "link check: node " MACSTR " rssi %d - %u/%d replies, avg %u ms%s",
             MAC2STR(have_ap ? ap.bssid : (uint8_t[6]){0}), have_ap ? ap.rssi : 0,
             (unsigned)(PING_COUNT - lost), PING_COUNT, (unsigned)avg, bad ? " - POOR" : "");
    if (!bad || !have_ap) return true;

    avoid(ap.bssid, esp_timer_get_time());
    if (!switch_node()) return false;
    if (!(xEventGroupWaitBits(s_eg, GOT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(REJOIN_WAIT_MS)) & GOT_IP)) {
        ESP_LOGW(TAG, "not reconnected after %d s", REJOIN_WAIT_MS / 1000);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));   // straight after a rejoin most pings go unanswered on any node
    if (ping_gateway(&avg, &lost)) ESP_LOGI(TAG, "new node: %u/%d replies, avg %u ms", (unsigned)(PING_COUNT - lost), PING_COUNT, (unsigned)avg);
    return true;
}
