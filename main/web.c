#include "web.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "mdns.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "settings.h"
#include "history.h"
#include "appstate.h"
#include "mqtt.h"
#include "deco.h"
#include "evlog.h"

static const char *TAG = "web";
static web_callbacks_t s_cb;
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* ---------- auth ---------- */

static bool const_time_eq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned d = la ^ lb;
    for (size_t i = 0; i < la && i < lb; i++) d |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return d == 0;
}

// True if the request carries the right credentials; otherwise sends 401 (or 403 if no password is set).
static bool authorised(httpd_req_t *r)
{
    if (!g_cfg.admin_pass[0]) {
        httpd_resp_set_status(r, "403 Forbidden");
        httpd_resp_sendstr(r, "No admin password is set. Hold the BOOT button for 5 seconds to run setup.");
        return false;
    }
    size_t n = httpd_req_get_hdr_value_len(r, "Authorization");
    if (n > 0 && n < 200) {
        char hdr[210];
        httpd_req_get_hdr_value_str(r, "Authorization", hdr, sizeof hdr);
        if (!strncmp(hdr, "Basic ", 6)) {
            unsigned char dec[150]; size_t olen = 0;
            if (!mbedtls_base64_decode(dec, sizeof dec - 1, &olen, (unsigned char *)hdr + 6, strlen(hdr + 6))) {
                dec[olen] = 0;
                char *colon = strchr((char *)dec, ':');
                if (colon) {
                    *colon = 0;
                    bool ok = const_time_eq((char *)dec, "admin") & const_time_eq(colon + 1, g_cfg.admin_pass);
                    memset(dec, 0, sizeof dec);
                    if (ok) return true;
                }
            }
        }
    }
    httpd_resp_set_status(r, "401 Unauthorized");
    httpd_resp_set_hdr(r, "WWW-Authenticate", "Basic realm=\"Deco Optimizer\"");
    httpd_resp_sendstr(r, "Sign in with user admin");
    return false;
}

/* ---------- helpers ---------- */

static esp_err_t send_json(httpd_req_t *r, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(r, s);
    free(s);
    return e;
}

static cJSON *read_json_body(httpd_req_t *r)
{
    if (r->content_len == 0 || r->content_len > 1200) return NULL;
    char *buf = malloc(r->content_len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < (int)r->content_len) {
        int n = httpd_req_recv(r, buf + got, r->content_len - got);
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[got] = 0;
    cJSON *j = cJSON_Parse(buf);
    memset(buf, 0, got);
    free(buf);
    return j;
}

static void restart_later(void *arg) { vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); }

static void hex_fp(const uint8_t *fp, char *out)
{
    for (int i = 0; i < 20; i++) sprintf(out + i * 3, "%02x%s", fp[i], i < 19 ? ":" : "");
}

/* ---------- handlers ---------- */

static esp_err_t h_index(httpd_req_t *r)
{
    if (!authorised(r)) return ESP_OK;
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_status(httpd_req_t *r)
{
    if (!authorised(r)) return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    char st[40]; app_state_get(st, sizeof st);
    cJSON_AddStringToObject(o, "state", st);
    cJSON_AddBoolToObject(o, "busy", app_busy_get());
    cJSON_AddBoolToObject(o, "optimize", g_cfg.optimize);
    cJSON_AddNumberToObject(o, "next", app_next_get());
    cJSON_AddNumberToObject(o, "sched_en", g_cfg.sched_en);

    cJSON *w = cJSON_AddObjectToObject(o, "wifi");
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        char b[18]; snprintf(b, sizeof b, MACSTR, MAC2STR(ap.bssid));
        cJSON_AddStringToObject(w, "ssid", (char *)ap.ssid);
        cJSON_AddNumberToObject(w, "rssi", ap.rssi);
        cJSON_AddNumberToObject(w, "channel", ap.primary);
        cJSON_AddStringToObject(w, "bssid", b);
    }
    esp_netif_ip_info_t ip;
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK) {
        char s[16]; snprintf(s, sizeof s, IPSTR, IP2STR(&ip.ip));
        cJSON_AddStringToObject(w, "ip", s);
    }
    cJSON *m = cJSON_AddObjectToObject(o, "mqtt");
    cJSON_AddBoolToObject(m, "enabled", g_cfg.mqtt_user[0] != 0);
    cJSON_AddBoolToObject(m, "connected", mqtt_is_connected());
    cJSON *y = cJSON_AddObjectToObject(o, "sys");
    cJSON_AddNumberToObject(y, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(y, "min_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(y, "largest", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(y, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON *h = cJSON_AddArrayToObject(o, "history");
    for (int i = 0; i < history_count(); i++) {
        hist_entry_t e; history_get(i, &e);
        cJSON *x = cJSON_CreateObject();
        cJSON_AddNumberToObject(x, "ts", e.ts);
        cJSON_AddStringToObject(x, "v", e.verdict == DECO_ERROR ? "error" : deco_verdict_str(e.verdict));
        cJSON_AddNumberToObject(x, "dur", e.duration_s);
        cJSON_AddNumberToObject(x, "flags", e.flags);
        cJSON_AddItemToArray(h, x);
    }
    return send_json(r, o);
}

static esp_err_t h_config_get(httpd_req_t *r)
{
    if (!authorised(r)) return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "wifi_ssid", g_cfg.wifi_ssid);
    cJSON_AddStringToObject(o, "deco_host", g_cfg.deco_host);
    cJSON_AddStringToObject(o, "deco_email", g_cfg.deco_email);
    cJSON_AddStringToObject(o, "mqtt_uri", g_cfg.mqtt_uri);
    cJSON_AddStringToObject(o, "mqtt_user", g_cfg.mqtt_user);
    cJSON_AddBoolToObject(o, "sched_en", g_cfg.sched_en);
    cJSON_AddNumberToObject(o, "sched_h", g_cfg.sched_h);
    cJSON_AddNumberToObject(o, "sched_m", g_cfg.sched_m);
    // secrets are never sent back, only whether they are set
    cJSON_AddBoolToObject(o, "wifi_pass_set", g_cfg.wifi_pass[0] != 0);
    cJSON_AddBoolToObject(o, "deco_pass_set", g_cfg.deco_pass[0] != 0);
    cJSON_AddBoolToObject(o, "mqtt_pass_set", g_cfg.mqtt_pass[0] != 0);
    cJSON_AddBoolToObject(o, "hostkey_set", g_cfg.hostkey_set);
    if (g_cfg.hostkey_set) { char fp[61]; hex_fp(g_cfg.hostkey_sha1, fp); cJSON_AddStringToObject(o, "hostkey", fp); }
    return send_json(r, o);
}

// Copies a JSON string field into dst if present (and, for secrets, non-empty). Returns true if changed.
static bool take_str(cJSON *j, const char *key, char *dst, size_t cap, bool secret, size_t minlen)
{
    cJSON *v = cJSON_GetObjectItem(j, key);
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= cap) return false;
    if (secret && !v->valuestring[0]) return false;
    if (strlen(v->valuestring) < minlen) return false;
    if (!strcmp(dst, v->valuestring)) return false;
    strlcpy(dst, v->valuestring, cap);
    return true;
}

static esp_err_t h_config_post(httpd_req_t *r)
{
    if (!authorised(r)) return ESP_OK;
    cJSON *j = read_json_body(r);
    if (!j) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    bool restart = false, changed = false;

    cJSON *ssid = cJSON_GetObjectItem(j, "wifi_ssid");
    if (cJSON_IsString(ssid) && !ssid->valuestring[0]) {          // an empty SSID would force setup mode on boot
        cJSON_Delete(j); httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Wi-Fi network cannot be empty"); return ESP_FAIL;
    }
    if (take_str(j, "wifi_ssid", g_cfg.wifi_ssid, sizeof g_cfg.wifi_ssid, false, 0)) { restart = changed = true; g_cfg.wifi_verified = false; }
    if (take_str(j, "wifi_pass", g_cfg.wifi_pass, sizeof g_cfg.wifi_pass, true, 0)) { restart = changed = true; g_cfg.wifi_verified = false; }
    if (take_str(j, "deco_host", g_cfg.deco_host, sizeof g_cfg.deco_host, false, 0)) { changed = true; g_cfg.hostkey_set = false; }
    changed |= take_str(j, "deco_email", g_cfg.deco_email, sizeof g_cfg.deco_email, false, 0);
    changed |= take_str(j, "deco_pass", g_cfg.deco_pass, sizeof g_cfg.deco_pass, true, 0);
    if (take_str(j, "mqtt_uri", g_cfg.mqtt_uri, sizeof g_cfg.mqtt_uri, false, 0)) restart = changed = true;
    if (take_str(j, "mqtt_user", g_cfg.mqtt_user, sizeof g_cfg.mqtt_user, false, 0)) restart = changed = true;
    if (take_str(j, "mqtt_pass", g_cfg.mqtt_pass, sizeof g_cfg.mqtt_pass, true, 0)) restart = changed = true;
    changed |= take_str(j, "admin_pass", g_cfg.admin_pass, sizeof g_cfg.admin_pass, true, 8);
    cJSON *v;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "sched_en"))) { g_cfg.sched_en = cJSON_IsTrue(v); changed = true; }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "sched_h")) && v->valueint >= 0 && v->valueint < 24) { g_cfg.sched_h = v->valueint; changed = true; }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "sched_m")) && v->valueint >= 0 && v->valueint < 60) { g_cfg.sched_m = v->valueint; changed = true; }
    bool want_restart = cJSON_IsTrue(cJSON_GetObjectItem(j, "restart"));
    cJSON_Delete(j);

    if (changed && !settings_save()) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed"); return ESP_FAIL; }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "restart_needed", restart && !want_restart);
    esp_err_t e = send_json(r, o);
    if (want_restart || (restart && want_restart)) xTaskCreate(restart_later, "restart", 2048, NULL, 5, NULL);
    return e;
}

static esp_err_t h_run(httpd_req_t *r)
{
    if (!authorised(r)) return ESP_OK;
    if (s_cb.on_run) s_cb.on_run();
    cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", true);
    return send_json(r, o);
}

static esp_err_t h_mode(httpd_req_t *r)
{
    if (!authorised(r)) return ESP_OK;
    cJSON *j = read_json_body(r);
    cJSON *v = j ? cJSON_GetObjectItem(j, "optimize") : NULL;
    if (!cJSON_IsBool(v)) { cJSON_Delete(j); httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "need optimize:bool"); return ESP_FAIL; }
    if (s_cb.on_mode) s_cb.on_mode(cJSON_IsTrue(v));
    cJSON_Delete(j);
    cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", true);
    return send_json(r, o);
}

static esp_err_t h_repin(httpd_req_t *r)
{
    if (!authorised(r)) return ESP_OK;
    g_cfg.hostkey_set = false;            // the next connection pins whatever key the Deco presents
    settings_save();
    cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", true);
    return send_json(r, o);
}

// Persistent events (boot reasons, run outcomes with failure reasons, ...), newest first.
static esp_err_t h_events(httpd_req_t *r)
{
    if (!authorised(r)) return ESP_OK;
    evlog_event_t *ev = malloc(EVLOG_EVENT_SLOTS * sizeof *ev);
    if (!ev) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    int n = evlog_events_read(ev, EVLOG_EVENT_SLOTS);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ts", ev[i].ts);
        cJSON_AddNumberToObject(o, "up", ev[i].uptime_s);
        cJSON_AddStringToObject(o, "text", ev[i].text);
        cJSON_AddItemToArray(arr, o);
    }
    free(ev);
    return send_json(r, arr);
}

// Everything the firmware logged recently (RAM ring buffer; empty after a restart).
static esp_err_t h_log(httpd_req_t *r)
{
    if (!authorised(r)) return ESP_OK;
    char *buf = malloc(EVLOG_RAM_SIZE + 1);
    if (!buf) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    size_t n = evlog_ram_copy(buf, EVLOG_RAM_SIZE + 1);
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(r, buf, n);
    free(buf);
    return e;
}

static void erase_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    settings_erase_all();
    esp_restart();
}

// Wipes everything. Requires the literal confirmation string so it cannot be triggered by a stray request.
static esp_err_t h_erase(httpd_req_t *r)
{
    if (!authorised(r)) return ESP_OK;
    cJSON *j = read_json_body(r);
    cJSON *c = j ? cJSON_GetObjectItem(j, "confirm") : NULL;
    bool ok = cJSON_IsString(c) && !strcmp(c->valuestring, "ERASE");
    cJSON_Delete(j);
    if (!ok) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "confirmation missing"); return ESP_FAIL; }
    ESP_LOGW(TAG, "ERASE ALL requested from the web page");
    xTaskCreate(erase_task, "erase", 3072, NULL, 5, NULL);
    cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", true);
    return send_json(r, o);
}

static esp_err_t h_reboot(httpd_req_t *r)
{
    if (!authorised(r)) return ESP_OK;
    xTaskCreate(restart_later, "restart", 2048, NULL, 5, NULL);
    cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", true);
    return send_json(r, o);
}

void web_start(const web_callbacks_t *cb)
{
    s_cb = *cb;
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("deco-optimizer");
        mdns_instance_name_set("Deco Optimizer");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 7168;
    hc.max_open_sockets = 3;      // sockets are shared with SSH, MQTT and ping
    hc.lru_purge_enable = true;
    hc.max_uri_handlers = 12;
    httpd_handle_t srv;
    if (httpd_start(&srv, &hc) != ESP_OK) { ESP_LOGE(TAG, "http server failed to start"); return; }
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/config", .method = HTTP_GET, .handler = h_config_get},
        {.uri = "/api/config", .method = HTTP_POST, .handler = h_config_post},
        {.uri = "/api/run", .method = HTTP_POST, .handler = h_run},
        {.uri = "/api/mode", .method = HTTP_POST, .handler = h_mode},
        {.uri = "/api/repin", .method = HTTP_POST, .handler = h_repin},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot},
        {.uri = "/api/erase", .method = HTTP_POST, .handler = h_erase},
        {.uri = "/api/events", .method = HTTP_GET, .handler = h_events},
        {.uri = "/api/log", .method = HTTP_GET, .handler = h_log},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(srv, &uris[i]);
    ESP_LOGI(TAG, "web page up: http://deco-optimizer.local/");
}
