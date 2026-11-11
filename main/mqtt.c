#include "mqtt.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "deco.h"
#include "settings.h"
#include "pings.h"

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_client;
static mqtt_callbacks_t s_cb;
static bool s_connected;
static char s_id[24];          // deco_optimizer_<mac tail>
static char s_base[48];        // deco_optimizer/<mac tail>

// Last published values, re-sent after every (re)connect.
static char s_state[24] = "unknown";
static char s_attr[240] = "{}";
static char s_mode[4] = "OFF";
static char s_next[24] = "";
static char s_last[24] = "";

static void pub(const char *sub, const char *payload, bool retain)
{
    if (!s_client || !s_connected) return;
    char topic[96];
    snprintf(topic, sizeof topic, "%s/%s", s_base, sub);
    esp_mqtt_client_enqueue(s_client, topic, payload, 0, 1, retain, true);
}

static void iso_utc(uint32_t ts, char *out, size_t n)
{
    time_t t = ts; struct tm tm; gmtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

// object_id and unique_id must be [a-z0-9_-]; the display name is separate.
static void discovery(const char *component, const char *object, const char *name, const char *extra)
{
    char topic[112], payload[640];
    snprintf(topic, sizeof topic, "homeassistant/%s/%s/%s/config", component, s_id, object);
    snprintf(payload, sizeof payload,
             "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"availability_topic\":\"%s/availability\","
             "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"Deco Optimizer\",\"model\":\"ESP32-2424S012\","
             "\"manufacturer\":\"DIY\"},%s}",
             name, s_id, object, s_base, s_id, extra);
    esp_mqtt_client_enqueue(s_client, topic, payload, 0, 1, true, true);
}

static void publish_discovery(void)
{
    char e[300];
    snprintf(e, sizeof e, "\"state_topic\":\"%s/state\",\"json_attributes_topic\":\"%s/attributes\",\"icon\":\"mdi:wifi-cog\"", s_base, s_base);
    discovery("sensor", "network_optimisation", "Network optimisation", e);
    snprintf(e, sizeof e, "\"state_topic\":\"%s/last_run\",\"device_class\":\"timestamp\"", s_base);
    discovery("sensor", "last_scan", "Last scan", e);
    snprintf(e, sizeof e, "\"state_topic\":\"%s/next_run\",\"device_class\":\"timestamp\"", s_base);
    discovery("sensor", "next_scan", "Next scheduled scan", e);
    snprintf(e, sizeof e, "\"state_topic\":\"%s/mode\",\"command_topic\":\"%s/mode/set\",\"icon\":\"mdi:tune\"", s_base, s_base);
    discovery("switch", "optimise_mode", "Optimise mode", e);
    snprintf(e, sizeof e, "\"command_topic\":\"%s/run/set\",\"payload_press\":\"PRESS\",\"icon\":\"mdi:play\"", s_base);
    discovery("button", "run_scan", "Run scan now", e);
}

// One sensor per ping target (gateway, connected node, each device in the settings): the state is the average round-trip
// time in ms, with min and loss as attributes. A device's sensor is identified by its NAME, not its slot or address, so
// Home Assistant keeps one history for "Garage cam" whatever address it has later. An unnamed device falls back to its slot.
static void raw_slug(const char *label, char *out, size_t n)       // "Garage cam" -> "garage_cam"
{
    size_t k = 0;
    for (const char *c = label; *c && k < n - 1; c++) {
        char ch = (*c >= 'A' && *c <= 'Z') ? *c + 32 : *c;
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) out[k++] = ch;
        else if (k && out[k - 1] != '_') out[k++] = '_';
    }
    while (k && out[k - 1] == '_') k--;
    out[k] = 0;
}

static void dev_slug(const ping_dev_t *d, int i, char *out, size_t n)
{
    char s[20], t[20];
    raw_slug(d[i].label, s, sizeof s);
    bool dup = false;
    for (int j = 0; j < i && !dup; j++) if (d[j].ip) { raw_slug(d[j].label, t, sizeof t); dup = !strcmp(s, t); }
    if (!s[0] || dup) snprintf(out, n, "d%d", i + 1); else snprintf(out, n, "%s", s);
}

static void remove_ping_sensor(const char *slug)
{
    char topic[112];
    snprintf(topic, sizeof topic, "homeassistant/sensor/%s/ping_%s/config", s_id, slug);
    esp_mqtt_client_enqueue(s_client, topic, "", 0, 1, true, true);
}

static void publish_ping_discovery(const ping_dev_t *old)
{
    static const char *const key[PING_SERIES] = {"gw", "node", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
    char slug[PING_SERIES][20];
    strcpy(slug[0], "gateway"); strcpy(slug[1], "node");
    for (int i = 0; i < PING_MAX_DEV; i++) dev_slug(g_cfg.ping_dev, i, slug[2 + i], sizeof slug[0]);
    if (old) {                                          // sensors of devices that were renamed or removed
        for (int i = 0; i < PING_MAX_DEV; i++) {
            if (!old[i].ip) continue;
            char os[20];
            dev_slug(old, i, os, sizeof os);
            bool kept = false;
            for (int k = 0; k < PING_MAX_DEV; k++) if (g_cfg.ping_every_min && g_cfg.ping_dev[k].ip && !strcmp(slug[2 + k], os)) kept = true;
            if (!kept) remove_ping_sensor(os);
        }
    }
    for (int i = 0; i < PING_SERIES; i++) {
        bool on = g_cfg.ping_every_min && (i < 2 || g_cfg.ping_dev[i - 2].ip);
        if (!on) {
            if (i < 2) remove_ping_sensor(slug[i]);
            else { char fb[8]; snprintf(fb, sizeof fb, "d%d", i - 1); remove_ping_sensor(fb); }
            continue;
        }
        char obj[32], name[40], e[420];
        snprintf(obj, sizeof obj, "ping_%s", slug[i]);
        if (i == 0) strcpy(name, "Ping gateway");
        else if (i == 1) strcpy(name, "Ping connected node");
        else if (g_cfg.ping_dev[i - 2].label[0]) snprintf(name, sizeof name, "Ping %s", g_cfg.ping_dev[i - 2].label);
        else snprintf(name, sizeof name, "Ping device %d", i - 1);
        snprintf(e, sizeof e,
                 "\"state_topic\":\"%s/ping\",\"value_template\":\"{{ value_json.%s.avg }}\",\"unit_of_measurement\":\"ms\","
                 "\"state_class\":\"measurement\",\"json_attributes_topic\":\"%s/ping\",\"json_attributes_template\":\"{{ value_json.%s | tojson }}\","
                 "\"icon\":\"mdi:lan-pending\"", s_base, key[i], s_base, key[i]);
        discovery("sensor", obj, name, e);
    }
}

void mqtt_ping_discovery_refresh(const ping_dev_t *old) { if (s_client && s_connected) publish_ping_discovery(old); }

void mqtt_publish_pings(const ping_round_t *r)
{
    static const char *const key[PING_SERIES] = {"gw", "node", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
    char b[640]; int n = snprintf(b, sizeof b, "{");
    for (int i = 0; i < PING_SERIES && n < (int)sizeof b - 80; i++) {
        const ping_s_t *s = &r->s[i];
        if (!s->sent) continue;
        bool none = s->lost >= s->sent;
        if (none) n += snprintf(b + n, sizeof b - n, "%s\"%s\":{\"avg\":null,\"min\":null,\"loss\":100}", n > 1 ? "," : "", key[i]);
        else n += snprintf(b + n, sizeof b - n, "%s\"%s\":{\"avg\":%u,\"min\":%u,\"loss\":%u}", n > 1 ? "," : "", key[i], s->avg, s->min, s->lost * 100 / s->sent);
    }
    snprintf(b + n, sizeof b - n, "}");
    pub("ping", b, false);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected");
        pub("availability", "online", true);
        publish_discovery();
        publish_ping_discovery(NULL);
        pub("state", s_state, true);
        pub("attributes", s_attr, true);
        pub("mode", s_mode, true);
        if (s_next[0]) pub("next_run", s_next, true);
        if (s_last[0]) pub("last_run", s_last, true);
        {
            char t[96];
            snprintf(t, sizeof t, "%s/mode/set", s_base); esp_mqtt_client_subscribe(s_client, t, 1);
            snprintf(t, sizeof t, "%s/run/set", s_base);  esp_mqtt_client_subscribe(s_client, t, 1);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_DATA: {
        char topic[96] = "", msg[16] = "";
        snprintf(topic, sizeof topic, "%.*s", ev->topic_len, ev->topic);
        snprintf(msg, sizeof msg, "%.*s", ev->data_len, ev->data);
        if (strstr(topic, "/mode/set") && s_cb.on_mode) s_cb.on_mode(!strcmp(msg, "ON"));
        else if (strstr(topic, "/run/set") && s_cb.on_run) s_cb.on_run();
        break;
    }
    default: break;
    }
}

void mqtt_start(const mqtt_callbacks_t *cb, bool optimize_initial)
{
    s_cb = *cb;
    strcpy(s_mode, optimize_initial ? "ON" : "OFF");
    if (!g_cfg.mqtt_user[0]) { ESP_LOGW(TAG, "MQTT_USER empty, MQTT disabled"); return; }
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_id, sizeof s_id, "deco_optimizer_%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_base, sizeof s_base, "deco_optimizer/%02x%02x%02x", mac[3], mac[4], mac[5]);
    char will[96]; snprintf(will, sizeof will, "%s/availability", s_base);
    esp_mqtt_client_config_t c = {
        .broker.address.uri = g_cfg.mqtt_uri,
        .credentials.username = g_cfg.mqtt_user,
        .credentials.authentication.password = g_cfg.mqtt_pass,
        .credentials.client_id = s_id,
        .session.last_will = {.topic = will, .msg = "offline", .qos = 1, .retain = 1},
        .session.keepalive = 60,
        .buffer.size = 1024,
        .task.stack_size = 5120,
    };
    s_client = esp_mqtt_client_init(&c);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_mqtt_client_start(s_client);
}

bool mqtt_is_connected(void) { return s_connected; }

void mqtt_publish_busy(const char *progress)
{
    // multi-line UI strings become one-line states
    char b[sizeof s_state]; int j = 0;
    for (const char *p = progress; *p && j < (int)sizeof b - 1; p++) b[j++] = *p == '\n' ? ' ' : *p;
    b[j] = 0;
    snprintf(s_state, sizeof s_state, "%s", b);
    pub("state", s_state, true);
}

void mqtt_publish_result(const hist_entry_t *e, const char *detail)
{
    char when[24] = "";
    if (e->ts) iso_utc(e->ts, when, sizeof when);
    snprintf(s_state, sizeof s_state, "%s", e->verdict == DECO_ERROR ? "error" : deco_verdict_str(e->verdict));
    snprintf(s_attr, sizeof s_attr,
             "{\"duration_s\":%u,\"mode\":\"%s\",\"trigger\":\"%s\",\"detail\":\"%s\"}",
             e->duration_s, (e->flags & 1) ? "optimise" : "report", (e->flags & 4) ? "startup" : (e->flags & 2) ? "schedule" : "manual", detail ? detail : "");
    pub("state", s_state, true);
    pub("attributes", s_attr, true);
    if (when[0]) { snprintf(s_last, sizeof s_last, "%s", when); pub("last_run", s_last, true); }
}

void mqtt_publish_mode(bool optimize)
{
    strcpy(s_mode, optimize ? "ON" : "OFF");
    pub("mode", s_mode, true);
}

void mqtt_publish_next(uint32_t ts)
{
    if (ts) iso_utc(ts, s_next, sizeof s_next); else s_next[0] = 0;
    if (s_next[0]) pub("next_run", s_next, true);
}
