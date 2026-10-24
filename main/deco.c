#include "deco.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "cJSON.h"
#include "mbedtls/sha1.h"
#include "lwip/sockets.h"
#include <libssh2.h>
#include "settings.h"
#include "config.h"
#include "tmp_proto.h"
#include "wifi.h"

static const char *TAG = "deco";

#define OP_CONNECT 0x0001
#define OP_SCAN    0x4263
#define OP_POLL    0x4265
#define OP_OPT     0x4266

// The Deco tolerates a stale config_version, so fixed values are used (see PROTOCOL.md section 2.2).
#define SCAN_JSON "{\"config_version\":1792811054775,\"params\":{\"scan_type\":[\"network_quality\"]}}"
#define POLL_JSON "{\"config_version\":1792811057639,\"params\":{\"scan_type\":[\"network_quality\"]}}"
#define OPT_JSON  "{\"config_version\":1792811070546}"

typedef struct {
    int sock;
    LIBSSH2_SESSION *ssh;
    LIBSSH2_CHANNEL *ch;
    uint32_t seq;                  // transport sequence, starts at 1 for CONNECT
    uint8_t rx[1536];
    size_t rx_len;
} conn_t;

static void sha1_hex_lower(const char *s, char out[41])
{
    char low[96]; size_t n = strlen(s); if (n >= sizeof low) n = sizeof low - 1;
    for (size_t i = 0; i < n; i++) low[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i];
    unsigned char d[20];
    mbedtls_sha1((unsigned char *)low, n, d);
    for (int i = 0; i < 20; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void conn_close(conn_t *c)
{
    if (c->ch) { libssh2_channel_free(c->ch); c->ch = NULL; }
    if (c->ssh) { libssh2_session_disconnect(c->ssh, "bye"); libssh2_session_free(c->ssh); c->ssh = NULL; }
    if (c->sock >= 0) { close(c->sock); c->sock = -1; }
    c->rx_len = 0;
}

// Writes the whole buffer to the tunnel.
static bool chan_write(conn_t *c, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = libssh2_channel_write(c->ch, (const char *)p, n);
        if (w == LIBSSH2_ERROR_EAGAIN) continue;
        if (w < 0) return false;
        p += w; n -= w;
    }
    return true;
}

// Reads until one full transport frame is buffered; copies its App-V2 payload. Returns payload length or -1.
static int read_frame(conn_t *c, uint8_t *payload, size_t cap, uint32_t timeout_ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        size_t off = 0;
        // skip bare 01 01 04 00 control tokens
        while (c->rx_len - off >= 4 && c->rx[off] == 1 && c->rx[off + 1] == 1 && c->rx[off + 2] == 4 && c->rx[off + 3] == 0) off += 4;
        if (off) { memmove(c->rx, c->rx + off, c->rx_len - off); c->rx_len -= off; }
        if (c->rx_len >= 16) {
            if (c->rx[0] != 1 || c->rx[1] != 1) return -1;           // lost framing
            size_t plen = (c->rx[4] << 8) | c->rx[5];
            if (plen > sizeof c->rx - 16 || plen > cap) return -1;
            if (c->rx_len >= 16 + plen) {
                memcpy(payload, c->rx + 16, plen);
                memmove(c->rx, c->rx + 16 + plen, c->rx_len - 16 - plen);
                c->rx_len -= 16 + plen;
                return (int)plen;
            }
        }
        if (esp_timer_get_time() > end) return -1;
        ssize_t r = libssh2_channel_read(c->ch, (char *)c->rx + c->rx_len, sizeof c->rx - c->rx_len);
        if (r > 0) c->rx_len += r;
        else if (r != LIBSSH2_ERROR_EAGAIN && r != LIBSSH2_ERROR_TIMEOUT) return -1;
        else if (r == 0) return -1;
    }
}

// One request/response exchange. json_out (may be NULL) receives the response body.
static bool tmp_request(conn_t *c, uint16_t opcode, const char *json, char *json_out, size_t out_cap)
{
    uint8_t app[160], frame[16 + 160], payload[1200];
    size_t alen;
    if (opcode == OP_CONNECT) {
        static const uint8_t hello[20] = {0x01, 0x02, 0x00, 0x01, 0x02};  // fixed 20-byte CONNECT
        memcpy(app, hello, 20); alen = 20; memset(app + 5, 0, 15);
    } else {
        alen = tmp_build_app(app, opcode, c->seq, json);                   // app_seq == transport seq - 1
    }
    c->seq++;
    size_t flen = tmp_build_frame(frame, c->seq, app, alen);
    if (!chan_write(c, frame, flen)) return false;
    int n = read_frame(c, payload, sizeof payload, 15000);
    if (n < 20) { ESP_LOGW(TAG, "op 0x%04x: no/NAK response (%d)", opcode, n); return false; }
    size_t jl = ((size_t)payload[12] << 24) | (payload[13] << 16) | (payload[14] << 8) | payload[15];
    if (20 + jl > (size_t)n) jl = n - 20;
    if (json_out) {
        size_t m = jl < out_cap - 1 ? jl : out_cap - 1;
        memcpy(json_out, payload + 20, m); json_out[m] = 0;
    }
    return true;
}

static bool conn_open(conn_t *c, char *err, size_t errcap)
{
    memset(c, 0, sizeof *c); c->sock = -1;
    if (!wifi_wait_connected(30000)) { snprintf(err, errcap, "no wifi"); return false; }
    char user[41]; sha1_hex_lower(g_cfg.deco_email, user);
    ESP_LOGI(TAG, "open: tcp");

    c->sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(22)};
    inet_pton(AF_INET, g_cfg.deco_host, &a.sin_addr);
    struct timeval tv = {.tv_sec = 10};
    setsockopt(c->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(c->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(c->sock, (struct sockaddr *)&a, sizeof a)) {
        // "no route" / timeout usually means a stale Wi-Fi association: rejoin once and try again
        int e1 = errno;
        close(c->sock); c->sock = -1;
        wifi_kick();
        if (!wifi_wait_connected(30000)) { snprintf(err, errcap, "no wifi (tcp %d)", e1); goto fail; }
        c->sock = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(c->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(c->sock, (struct sockaddr *)&a, sizeof a)) { snprintf(err, errcap, "tcp errno %d", errno); goto fail; }
    }

    ESP_LOGI(TAG, "open: tcp ok, handshake");
    ESP_LOGI(TAG, "[heap] before session init: free=%u largest=%u", (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    c->ssh = libssh2_session_init();
    if (!c->ssh) { snprintf(err, errcap, "out of memory"); goto fail; }
    libssh2_session_set_blocking(c->ssh, 1);
    libssh2_session_set_timeout(c->ssh, 20000);
    if (libssh2_session_handshake(c->ssh, c->sock)) { snprintf(err, errcap, "ssh handshake"); goto fail; }

    ESP_LOGI(TAG, "[heap] after handshake: free=%u min=%u largest=%u", (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "open: handshake ok, checking key");
    const char *fp = libssh2_hostkey_hash(c->ssh, LIBSSH2_HOSTKEY_HASH_SHA1);
    if (!fp) { snprintf(err, errcap, "no host key"); goto fail; }
    if (!g_cfg.hostkey_set) {                    // trust on first use, then pinned
        memcpy(g_cfg.hostkey_sha1, fp, 20); g_cfg.hostkey_set = true; settings_save();
        ESP_LOGW(TAG, "pinned Deco host key on first use");
    } else if (memcmp(fp, g_cfg.hostkey_sha1, 20)) { snprintf(err, errcap, "host key changed"); goto fail; }

    ESP_LOGI(TAG, "open: key ok, login");
    if (libssh2_userauth_password(c->ssh, user, g_cfg.deco_pass)) { snprintf(err, errcap, "ssh login failed"); goto fail; }
    ESP_LOGI(TAG, "open: login ok, tunnel");
    c->ch = libssh2_channel_direct_tcpip_ex(c->ssh, "127.0.0.1", 20002, "127.0.0.1", 0);
    if (!c->ch) { snprintf(err, errcap, "tunnel refused"); goto fail; }

    ESP_LOGI(TAG, "open: tunnel ok, TMP connect");
    char tok[64];
    if (!tmp_request(c, OP_CONNECT, NULL, tok, sizeof tok)) { snprintf(err, errcap, "TMP connect"); goto fail; }
    ESP_LOGI(TAG, "open: done");
    return true;
fail:
    conn_close(c);
    return false;
}

typedef struct {
    bool idle;                 // network_quality.state == "idle"
    bool optimizable, optimal;
    bool parsed;
} nq_t;

static nq_t parse_poll(const char *json)
{
    nq_t r = {0};
    cJSON *root = cJSON_Parse(json);
    if (!root) return r;
    cJSON *nq = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "result"), "network_quality");
    if (nq) {
        r.parsed = true;
        cJSON *st = cJSON_GetObjectItem(nq, "state");
        r.idle = cJSON_IsString(st) && !strcmp(st->valuestring, "idle");
        cJSON *d;
        cJSON_ArrayForEach(d, cJSON_GetObjectItem(nq, "detail")) {
            cJSON *ds = cJSON_GetObjectItem(d, "state");
            if (!cJSON_IsString(ds)) continue;
            if (!strcmp(ds->valuestring, "optimizable")) r.optimizable = true;
            if (!strcmp(ds->valuestring, "optimal")) r.optimal = true;
        }
    }
    cJSON_Delete(root);
    return r;
}

// Polls until the Deco is idle. Reconnects if the link drops (optimizing re-tunes Wi-Fi, which can drop us too).
static bool poll_until_idle(conn_t *c, uint32_t timeout_s, nq_t *out, deco_progress_cb cb, char *err, size_t errcap)
{
    int64_t end = esp_timer_get_time() + (int64_t)timeout_s * 1000000;
    char body[700];
    while (esp_timer_get_time() < end) {
        if (c->ch && tmp_request(c, OP_POLL, POLL_JSON, body, sizeof body)) {
            nq_t r = parse_poll(body);
            if (r.parsed && r.idle) { *out = r; return true; }
        } else {
            if (cb) cb("link dropped,\nreconnecting");
            conn_close(c);
            vTaskDelay(pdMS_TO_TICKS(3000));
            char e2[32];
            if (!conn_open(c, e2, sizeof e2)) { ESP_LOGW(TAG, "reconnect failed: %s", e2); continue; }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    snprintf(err, errcap, "timed out waiting");
    return false;
}

void deco_run(bool optimize, deco_result_t *out, deco_progress_cb cb)
{
    memset(out, 0, sizeof *out);
    out->optimize_requested = optimize;
    int64_t t0 = esp_timer_get_time();
    conn_t c;
    char err[40] = "";
    nq_t r;
    out->verdict = DECO_ERROR;

    if (cb) cb("connecting");
    if (!conn_open(&c, err, sizeof err)) goto done;

    if (cb) cb("scanning");
    if (!tmp_request(&c, OP_SCAN, SCAN_JSON, NULL, 0)) { snprintf(err, sizeof err, "scan request"); goto done; }
    if (!poll_until_idle(&c, DECO_SCAN_TIMEOUT_S, &r, cb, err, sizeof err)) goto done;

    if (r.optimizable) {
        if (!optimize) { out->verdict = DECO_OPTIMIZABLE; goto done; }
        if (cb) cb("optimizing");
        if (!tmp_request(&c, OP_OPT, OPT_JSON, NULL, 0)) { snprintf(err, sizeof err, "optimize request"); goto done; }
        if (!poll_until_idle(&c, DECO_OPTIMIZE_TIMEOUT_S, &r, cb, err, sizeof err)) goto done;
        out->verdict = DECO_OPTIMIZED;
    } else {
        out->verdict = DECO_OPTIMAL;
    }
done:
    conn_close(&c);
    out->duration_s = (esp_timer_get_time() - t0) / 1000000;
    strlcpy(out->detail, out->verdict == DECO_ERROR ? err : "", sizeof out->detail);
    ESP_LOGI(TAG, "result: %s in %us %s", deco_verdict_str(out->verdict), (unsigned)out->duration_s, out->detail);
}

const char *deco_verdict_str(deco_verdict_t v)
{
    switch (v) {
    case DECO_OPTIMAL: return "optimal";
    case DECO_OPTIMIZABLE: return "optimizable";
    case DECO_OPTIMIZED: return "optimized";
    default: return "error";
    }
}
