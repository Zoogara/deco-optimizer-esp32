#include "provision.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "lwip/sockets.h"
#include "settings.h"
#include "ui.h"
#include "display.h"

static const char *TAG = "setup";

// A headless device has no screen to show a random setup password on, so the build must supply one.
#if CONFIG_DECO_HEADLESS
_Static_assert(sizeof(CONFIG_DECO_SETUP_AP_PASSWORD) >= 9, "headless builds need DECO_SETUP_AP_PASSWORD (8-63 characters)");
#endif
_Static_assert(sizeof(CONFIG_DECO_SETUP_AP_PASSWORD) <= 64, "DECO_SETUP_AP_PASSWORD is longer than 63 characters");
#define AP_IP "192.168.4.1"
#define MAX_NETS 15

static char s_nets[MAX_NETS][33];
static int s_net_count;

static const char *PAGE_HEAD =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Deco Optimizer setup</title><style>body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}"
    "main{max-width:420px;margin:0 auto;padding:20px}h1{font-size:1.3em}label{display:block;margin:14px 0 4px;color:#aaa;font-size:.9em}"
    "input{width:100%;box-sizing:border-box;padding:10px;font-size:1em;border-radius:6px;border:1px solid #444;background:#222;color:#eee}"
    "button{margin-top:22px;width:100%;padding:12px;font-size:1.05em;border:0;border-radius:6px;background:#2d7ff9;color:#fff}"
    ".e{color:#ff7070}.s{color:#6c6}small{color:#888}</style></head><body><main>";

static void send_page(httpd_req_t *r, const char *body)
{
    httpd_resp_set_type(r, "text/html");
    httpd_resp_sendstr_chunk(r, PAGE_HEAD);
    httpd_resp_sendstr_chunk(r, body);
    httpd_resp_sendstr_chunk(r, "</main></body></html>");
    httpd_resp_sendstr_chunk(r, NULL);
}

static esp_err_t root_get(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    httpd_resp_sendstr_chunk(r, PAGE_HEAD);
    httpd_resp_sendstr_chunk(r,
        "<h1>Deco Optimizer setup</h1><p><small>Enter the Wi-Fi network the Deco mesh provides, and choose a password "
        "for this device's web page. All other settings are entered there afterwards.</small></p>"
        "<form method=post action=/save><label>Wi-Fi network</label><input name=ssid list=nets maxlength=32 required "
        "autocapitalize=none autocomplete=off><datalist id=nets>");
    for (int i = 0; i < s_net_count; i++) {
        httpd_resp_sendstr_chunk(r, "<option value=\"");
        httpd_resp_sendstr_chunk(r, s_nets[i]);
        httpd_resp_sendstr_chunk(r, "\">");
    }
    httpd_resp_sendstr_chunk(r,
        "</datalist><label>Wi-Fi password</label><input name=pw type=password maxlength=64 autocomplete=off>"
        "<label>New admin password for this device <small>(8+ characters)</small></label>"
        "<input name=apw type=password minlength=8 maxlength=39 required autocomplete=off>"
        "<button>Save and restart</button></form>");
    httpd_resp_sendstr_chunk(r, "</main></body></html>");
    httpd_resp_sendstr_chunk(r, NULL);
    return ESP_OK;
}

static void url_decode(char *dst, const char *src, size_t cap)
{
    size_t n = 0;
    while (*src && n + 1 < cap) {
        if (*src == '+') { dst[n++] = ' '; src++; }
        else if (*src == '%' && src[1] && src[2]) { char h[3] = {src[1], src[2], 0}; dst[n++] = (char)strtol(h, NULL, 16); src += 3; }
        else dst[n++] = *src++;
    }
    dst[n] = 0;
}

static bool form_field(const char *body, const char *key, char *out, size_t cap)
{
    char pat[16]; snprintf(pat, sizeof pat, "%s=", key);
    for (const char *p = body; p; ) {
        if (!strncmp(p, pat, strlen(pat))) {
            const char *v = p + strlen(pat), *end = strchr(v, '&');
            size_t n = end ? (size_t)(end - v) : strlen(v);
            char raw[200]; if (n >= sizeof raw) n = sizeof raw - 1;
            memcpy(raw, v, n); raw[n] = 0;
            url_decode(out, raw, cap);
            return true;
        }
        p = strchr(p, '&'); if (p) p++;
    }
    out[0] = 0;
    return false;
}

static void restart_task(void *arg) { vTaskDelay(pdMS_TO_TICKS(2500)); esp_restart(); }

static esp_err_t save_post(httpd_req_t *r)
{
    char body[420];
    if (r->content_len == 0 || r->content_len >= sizeof body) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad size"); return ESP_FAIL; }
    int got = 0;
    while (got < (int)r->content_len) {
        int n = httpd_req_recv(r, body + got, r->content_len - got);
        if (n <= 0) return ESP_FAIL;
        got += n;
    }
    body[got] = 0;
    char ssid[33], pw[65], apw[40];
    form_field(body, "ssid", ssid, sizeof ssid);
    form_field(body, "pw", pw, sizeof pw);
    form_field(body, "apw", apw, sizeof apw);
    if (!ssid[0]) { send_page(r, "<h1 class=e>Wi-Fi network is required</h1><p><a href=/>Back</a></p>"); return ESP_OK; }
    if (strlen(apw) < 8) { send_page(r, "<h1 class=e>Admin password must be at least 8 characters</h1><p><a href=/>Back</a></p>"); return ESP_OK; }
    if (pw[0] && strlen(pw) < 8) { send_page(r, "<h1 class=e>Wi-Fi password must be at least 8 characters</h1><p><a href=/>Back</a></p>"); return ESP_OK; }

    strlcpy(g_cfg.wifi_ssid, ssid, sizeof g_cfg.wifi_ssid);
    strlcpy(g_cfg.wifi_pass, pw, sizeof g_cfg.wifi_pass);
    strlcpy(g_cfg.admin_pass, apw, sizeof g_cfg.admin_pass);
    g_cfg.wifi_verified = false;       // cleared again if it never connects (see main.c)
    if (!settings_save()) { send_page(r, "<h1 class=e>Could not save settings</h1>"); return ESP_OK; }
    send_page(r, "<h1 class=s>Saved</h1><p>The device is restarting and will join your Wi-Fi. "
                 "Reconnect your phone to your normal network, then open <b>http://deco-optimizer.local</b> "
                 "(or the address shown on the device) and sign in with the admin password you chose.</p>");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// Phones probe fixed URLs to detect a captive portal; redirecting everything unknown to the setup page opens it.
static esp_err_t redirect_404(httpd_req_t *r, httpd_err_code_t e)
{
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://" AP_IP "/");
    httpd_resp_send(r, NULL, 0);
    return ESP_OK;
}

static void dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a)) { ESP_LOGE(TAG, "dns bind failed"); vTaskDelete(NULL); }
    uint8_t buf[300];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof from;
        int n = recvfrom(s, buf, sizeof buf - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        // find the end of the question (name labels, then 4 bytes of type/class)
        int q = 12; while (q < n && buf[q]) q += buf[q] + 1;
        q += 5;
        if (q > n) continue;
        bool is_a = buf[q - 4] == 0 && buf[q - 3] == 1;
        buf[2] = 0x81; buf[3] = 0x80;                     // response, recursion available
        buf[6] = 0; buf[7] = is_a ? 1 : 0;                // answer count
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = q;
        if (is_a) {
            static const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
            memcpy(buf + len, ans, sizeof ans); len += sizeof ans;
        }
        sendto(s, buf, len, 0, (struct sockaddr *)&from, fl);
    }
}

void provision_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[24], pass[65];
    snprintf(ssid, sizeof ssid, "Deco-Setup-%02X%02X", mac[4], mac[5]);
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";    // no look-alike characters
    if (CONFIG_DECO_SETUP_AP_PASSWORD[0]) {
        strlcpy(pass, CONFIG_DECO_SETUP_AP_PASSWORD, sizeof pass);
    } else {
        for (int i = 0; i < 8; i++) pass[i] = alphabet[esp_random() % (sizeof alphabet - 1)];
        pass[8] = 0;
    }

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, ssid, sizeof ap.ap.ssid);
    strlcpy((char *)ap.ap.password, pass, sizeof ap.ap.password);
    ap.ap.ssid_len = strlen(ssid);
    ap.ap.channel = 6;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));       // STA is only used for the network list
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Network list for the form (best effort).
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        wifi_ap_record_t recs[20]; uint16_t n = 20;
        if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            for (int i = 0; i < n && s_net_count < MAX_NETS; i++) {
                if (!recs[i].ssid[0]) continue;
                bool dup = false;
                for (int j = 0; j < s_net_count; j++) if (!strcmp(s_nets[j], (char *)recs[i].ssid)) dup = true;
                if (!dup) strlcpy(s_nets[s_net_count++], (char *)recs[i].ssid, sizeof s_nets[0]);
            }
        }
    }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 6144;
    hc.lru_purge_enable = true;
    httpd_handle_t srv;
    ESP_ERROR_CHECK(httpd_start(&srv, &hc));
    static const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    static const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &save);
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, redirect_404);
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);

    ESP_LOGW(TAG, "setup AP \"%s\" up (%d networks listed); portal at http://" AP_IP "/", ssid, s_net_count);
    ui_show_setup(ssid, pass);
    display_set_backlight(30);          // setup screen is on in a dark room: keep it comfortable
}
