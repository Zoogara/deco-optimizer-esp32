// Reads the Deco's mesh node list over its local web API. Plain HTTP, but the login is RSA-signed and the bodies are
// AES-CBC encrypted: see the notes in README. Nothing here keeps the 16-22 KB reply: it is base64-decoded, decrypted and
// scanned for a handful of fields as it streams past, so the memory needed is a few KB.
#include "deco_nodes.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mbedtls/rsa.h"
#include "mbedtls/bignum.h"
#include "mbedtls/aes.h"
#include "mbedtls/md5.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include "settings.h"
#include "wifi.h"
#include "evlog.h"

static const char *TAG = "nodes";

/* ---------- the result: a small table of nodes ---------- */

static deco_node_t s_nodes[DECO_MAX_NODES];
static int s_count;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

int deco_nodes_count(void) { return s_count; }

static bool is_zero(const uint8_t a[6]) { static const uint8_t z[6] = {0}; return !memcmp(a, z, 6); }

// All the radio addresses of a node are its base MAC with the first byte's flag bits and the last byte changed (a node's
// 2.4 GHz radio is MAC+2, the 5 GHz one MAC+3, and the Deco may set the "locally administered" bits of the first byte for
// its SSID). The four middle bytes stay the same, so after an exact match fails they identify the node. This also covers
// the master, whose own BSSIDs the Deco reports as empty.
static bool same_device(const uint8_t bssid[6], const uint8_t mac[6])
{
    if (is_zero(mac) || memcmp(bssid + 1, mac + 1, 4)) return false;
    int d = (int)bssid[5] - (int)mac[5];
    return d >= -8 && d <= 8;
}

bool deco_node_for_bssid(const uint8_t bssid[6], deco_node_t *out)
{
    int hit = -1;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_count && hit < 0; i++)                       // pass 1: an exact radio or MAC match
        if (!memcmp(s_nodes[i].bssid_2g, bssid, 6) || !memcmp(s_nodes[i].bssid_5g, bssid, 6) || !memcmp(s_nodes[i].mac, bssid, 6)) hit = i;
    for (int i = 0; i < s_count && hit < 0; i++)                       // pass 2: the same device, by its middle bytes
        if (same_device(bssid, s_nodes[i].mac)) hit = i;
    if (hit >= 0) *out = s_nodes[hit];
    portEXIT_CRITICAL(&s_mux);
    return hit >= 0;
}

/* ---------- JSON scanner: picks fields out of the device_list objects as the bytes go past ---------- */

enum { F_MODEL, F_ROLE, F_MAC, F_B2, F_B5, F_NICK, F_CUSTOM, F_COUNT };
static const char *const FIELD_KEY[F_COUNT] = {"device_model", "role", "mac", "bssid_2g", "bssid_5g", "nickname", "custom_nickname"};
#define STR_MAX 72
#define NODE_DEPTH 4        // {"result":{"device_list":[ {node} ... : the node objects open at depth 4

typedef struct {
    int depth;
    bool in_str, esc, have_str;
    char str[STR_MAX];
    int str_len, str_depth;
    int cur_key;                         // which wanted field the pending value belongs to, or -1
    char f[F_COUNT][STR_MAX];            // fields of the node being read
    deco_node_t nodes[DECO_MAX_NODES];
    int count;
} jx_t;

static bool parse_mac(const char *s, uint8_t out[6])
{
    int n = 0, hi = -1;
    for (; *s && n < 6; s++) {
        int v = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
        if (v < 0) { hi = -1; continue; }                // a separator
        if (hi < 0) hi = v; else { out[n++] = (uint8_t)(hi << 4 | v); hi = -1; }
    }
    return n == 6;
}

static void jx_finish_node(jx_t *j)
{
    if (j->count >= DECO_MAX_NODES || !j->f[F_MODEL][0]) return;
    deco_node_t *n = &j->nodes[j->count];
    memset(n, 0, sizeof *n);
    strlcpy(n->model, j->f[F_MODEL], sizeof n->model);
    strlcpy(n->role, j->f[F_ROLE], sizeof n->role);
    parse_mac(j->f[F_MAC], n->mac);
    parse_mac(j->f[F_B2], n->bssid_2g);
    parse_mac(j->f[F_B5], n->bssid_5g);
    size_t olen = 0;
    if (j->f[F_CUSTOM][0] && mbedtls_base64_decode((unsigned char *)n->name, sizeof n->name - 1, &olen, (const unsigned char *)j->f[F_CUSTOM], strlen(j->f[F_CUSTOM])) == 0 && olen)
        n->name[olen] = 0;
    else
        strlcpy(n->name, j->f[F_NICK], sizeof n->name);
    char *e = n->name + strlen(n->name);                  // a name can carry stray spaces ("Garden ")
    while (e > n->name && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    char *b = n->name;
    while (*b == ' ' || *b == '\t') b++;
    if (b != n->name) memmove(n->name, b, strlen(b) + 1);
    j->count++;
}

static void jx_feed(jx_t *j, char c)
{
    if (j->in_str) {
        if (j->esc) { j->esc = false; if (j->str_len < STR_MAX - 1) j->str[j->str_len++] = c; }
        else if (c == '\\') j->esc = true;
        else if (c == '"') { j->in_str = false; j->str[j->str_len] = 0; j->have_str = true; j->str_depth = j->depth; }
        else if (j->str_len < STR_MAX - 1) j->str[j->str_len++] = c;
        return;
    }
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') return;
    if (j->have_str) {                                    // the string that just closed is a key if ':' follows, else a value
        if (c == ':') {
            j->cur_key = -1;
            if (j->str_depth == NODE_DEPTH)
                for (int k = 0; k < F_COUNT; k++) if (!strcmp(j->str, FIELD_KEY[k])) { j->cur_key = k; break; }
        } else {
            if (j->cur_key >= 0 && j->str_depth == NODE_DEPTH) strlcpy(j->f[j->cur_key], j->str, STR_MAX);
            j->cur_key = -1;
        }
        j->have_str = false;
    }
    switch (c) {
    case '"': j->in_str = true; j->str_len = 0; break;
    case '{': case '[':
        j->depth++;
        if (c == '{' && j->depth == NODE_DEPTH) memset(j->f, 0, sizeof j->f);
        j->cur_key = -1;
        break;
    case '}': case ']':
        if (c == '}' && j->depth == NODE_DEPTH) jx_finish_node(j);
        j->depth--;
        j->cur_key = -1;
        break;
    case ',': j->cur_key = -1; break;
    default: break;
    }
}

/* ---------- crypto helpers ---------- */

static int rng_cb(void *ctx, unsigned char *buf, size_t len) { esp_fill_random(buf, len); return 0; }

// RSA PKCS#1 v1.5 public-key encryption; the result is lowercase hex.
static bool rsa_encrypt_hex(const char *nn, const char *ee, const uint8_t *in, size_t ilen, char *out, size_t out_cap)
{
    bool ok = false;
    mbedtls_rsa_context rsa; mbedtls_mpi N, E;
    mbedtls_rsa_init(&rsa); mbedtls_mpi_init(&N); mbedtls_mpi_init(&E);
    uint8_t buf[256];
    if (mbedtls_mpi_read_string(&N, 16, nn) || mbedtls_mpi_read_string(&E, 16, ee)) goto done;
    if (mbedtls_rsa_import(&rsa, &N, NULL, NULL, NULL, &E) || mbedtls_rsa_complete(&rsa)) goto done;
    size_t k = mbedtls_rsa_get_len(&rsa);
    if (k > sizeof buf || out_cap < 2 * k + 1 || ilen + 11 > k) goto done;
    if (mbedtls_rsa_pkcs1_encrypt(&rsa, rng_cb, NULL, ilen, in, buf)) goto done;
    for (size_t i = 0; i < k; i++) sprintf(out + 2 * i, "%02x", buf[i]);
    ok = true;
done:
    mbedtls_mpi_free(&N); mbedtls_mpi_free(&E); mbedtls_rsa_free(&rsa);
    return ok;
}

typedef struct {
    char base[80];
    char password[64];
    char pw_nn[300], pw_ee[16];          // key for the password (1024 bit on the Deco)
    char sg_nn[160], sg_ee[16];          // key for the request signature (512 bit)
    long seq;
    char key[17], iv[17];                // 16 ASCII hex characters each, used directly as the AES-128 key and IV
    char hash[33];                       // md5(username + password), hex
    char stok[96], cookie[96];
    bool logged_in;
} session_t;

// JSON -> PKCS7 pad -> AES-128-CBC -> base64. Returns the base64 length, or 0.
static size_t aes_encrypt_b64(const session_t *s, const char *plain, char *out, size_t cap)
{
    size_t n = strlen(plain), padn = 16 - n % 16, total = n + padn;
    uint8_t *buf = malloc(total);
    if (!buf) return 0;
    memcpy(buf, plain, n); memset(buf + n, (int)padn, padn);
    mbedtls_aes_context aes; mbedtls_aes_init(&aes);
    uint8_t iv[16]; memcpy(iv, s->iv, 16);
    size_t olen = 0;
    if (mbedtls_aes_setkey_enc(&aes, (const uint8_t *)s->key, 128) == 0 &&
        mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, total, iv, buf, buf) == 0)
        mbedtls_base64_encode((unsigned char *)out, cap - 1, &olen, buf, total);
    mbedtls_aes_free(&aes); free(buf);
    out[olen] = 0;
    return olen;
}

// "sign" = the text below, RSA-encrypted in 53-byte pieces. The login also carries the AES key and IV.
static bool make_sign(const session_t *s, bool login, long seq_plus_len, char *out, size_t cap)
{
    char text[200];
    if (login) snprintf(text, sizeof text, "k=%s&i=%s&h=%s&s=%ld", s->key, s->iv, s->hash, seq_plus_len);
    else snprintf(text, sizeof text, "h=%s&s=%ld", s->hash, seq_plus_len);
    size_t len = strlen(text); out[0] = 0;
    for (size_t pos = 0; pos < len; pos += 53) {
        char piece[260];
        size_t n = len - pos < 53 ? len - pos : 53;
        if (!rsa_encrypt_hex(s->sg_nn, s->sg_ee, (const uint8_t *)text + pos, n, piece, sizeof piece)) return false;
        if (strlen(out) + strlen(piece) + 1 > cap) return false;
        strcat(out, piece);
    }
    return true;
}

// Request body: sign=<hex>&data=<urlencoded base64>. Caller frees.
static char *build_body(const session_t *s, bool login, const char *json)
{
    char b64[900];
    size_t blen = aes_encrypt_b64(s, json, b64, sizeof b64);
    char sign[700];
    if (!blen || !make_sign(s, login, s->seq + (long)blen, sign, sizeof sign)) return NULL;
    char *body = malloc(strlen(sign) + blen * 3 + 16);
    if (!body) return NULL;
    char *p = body + sprintf(body, "sign=%s&data=", sign);
    for (size_t i = 0; i < blen; i++) {
        char c = b64[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) *p++ = c;
        else p += sprintf(p, "%%%02X", (unsigned char)c);       // + / = and friends
    }
    *p = 0;
    return body;
}

/* ---------- reply pipeline: {"data":"<base64>"} -> AES-CBC decrypt -> plaintext bytes ---------- */

typedef struct {
    const session_t *s;
    mbedtls_aes_context aes;
    uint8_t iv[16];
    enum { P_SEEK, P_COLON, P_B64, P_DONE } st;
    int match;                          // progress through the literal "data"
    uint8_t quad[4]; int qn;
    uint8_t cb[48]; int cn;             // cipher bytes waiting; the last block is held back for the padding
    bool broken;
    char *collect; size_t clen, ccap;   // keep the plaintext (small replies)
    jx_t *scan;                         // or scan it for nodes (the big reply)
    size_t plain_total;
} pipe_t;

static void pipe_emit(pipe_t *p, const uint8_t *b, size_t n)
{
    p->plain_total += n;
    if (p->collect) { size_t m = n < p->ccap - 1 - p->clen ? n : p->ccap - 1 - p->clen; memcpy(p->collect + p->clen, b, m); p->clen += m; p->collect[p->clen] = 0; }
    if (p->scan) for (size_t i = 0; i < n; i++) jx_feed(p->scan, (char)b[i]);
}

static void pipe_block(pipe_t *p)               // decrypt the oldest 16 pending bytes
{
    uint8_t out[16];
    if (mbedtls_aes_crypt_cbc(&p->aes, MBEDTLS_AES_DECRYPT, 16, p->iv, p->cb, out)) { p->broken = true; return; }
    memmove(p->cb, p->cb + 16, p->cn - 16); p->cn -= 16;
    pipe_emit(p, out, 16);
}

static void pipe_cipher_byte(pipe_t *p, uint8_t b)
{
    if (p->cn >= (int)sizeof p->cb) { p->broken = true; return; }
    p->cb[p->cn++] = b;
    if (p->cn >= 32) pipe_block(p);             // two blocks pending: the older one cannot be the last
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

static void pipe_char(pipe_t *p, char c)
{
    switch (p->st) {
    case P_SEEK: {
        static const char pat[] = "\"data\"";
        if (c == pat[p->match]) { if (++p->match == 6) { p->st = P_COLON; p->match = 0; } } else p->match = (c == pat[0]) ? 1 : 0;
        break;
    }
    case P_COLON: if (c == '"') p->st = P_B64; break;
    case P_B64:
        if (c == '"') { p->st = P_DONE; break; }
        if (c != '=' && b64val(c) < 0) break;                     // ignore stray characters
        p->quad[p->qn++] = (uint8_t)c;
        if (p->qn == 4) {
            int a = b64val(p->quad[0]), b = b64val(p->quad[1]), cc = p->quad[2] == '=' ? -1 : b64val(p->quad[2]), d = p->quad[3] == '=' ? -1 : b64val(p->quad[3]);
            pipe_cipher_byte(p, (uint8_t)(a << 2 | b >> 4));
            if (cc >= 0) pipe_cipher_byte(p, (uint8_t)((b & 15) << 4 | cc >> 2));
            if (cc >= 0 && d >= 0) pipe_cipher_byte(p, (uint8_t)((cc & 3) << 6 | d));
            p->qn = 0;
        }
        break;
    case P_DONE: break;
    }
}

static void pipe_feed(void *ctx, const uint8_t *b, size_t n)
{
    pipe_t *p = ctx;
    for (size_t i = 0; i < n && !p->broken; i++) pipe_char(p, (char)b[i]);
}

// Handles the last block and its padding. True if the whole reply decrypted cleanly.
static bool pipe_finish(pipe_t *p)
{
    if (p->broken || p->st != P_DONE || p->qn != 0 || p->cn != 16) return false;
    uint8_t out[16];
    if (mbedtls_aes_crypt_cbc(&p->aes, MBEDTLS_AES_DECRYPT, 16, p->iv, p->cb, out)) return false;
    p->cn = 0;
    if (out[15] == 0 || out[15] > 16) return false;
    pipe_emit(p, out, 16 - out[15]);
    return true;
}

static bool pipe_init(pipe_t *p, const session_t *s)
{
    memset(p, 0, sizeof *p);
    p->s = s;
    mbedtls_aes_init(&p->aes);
    memcpy(p->iv, s->iv, 16);
    return mbedtls_aes_setkey_dec(&p->aes, (const uint8_t *)s->key, 128) == 0;
}

/* ---------- HTTP ---------- */

typedef struct { char cookie[96]; } hdr_ctx_t;
typedef void (*sink_fn)(void *ctx, const uint8_t *p, size_t n);

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->user_data && e->header_key && e->header_value && !strcasecmp(e->header_key, "Set-Cookie")) {
        hdr_ctx_t *h = e->user_data;
        const char *p = strstr(e->header_value, "sysauth=");
        if (p) { p += 8; size_t n = strcspn(p, "; \r\n"); if (n < sizeof h->cookie) { memcpy(h->cookie, p, n); h->cookie[n] = 0; } }
    }
    return ESP_OK;
}

// POSTs `body` (may be NULL) and gives the reply body, in pieces, to `sink`. Returns the HTTP status, or -1.
// The Deco's web UI sends a form-encoded body labelled application/json; this copies that.
static int http_post(const char *url, const char *body, const char *cookie, hdr_ctx_t *hdr, sink_fn sink, void *sink_ctx)
{
    esp_http_client_config_t cfg = {.url = url, .method = HTTP_METHOD_POST, .timeout_ms = 30000,        /* the Deco can take 12 s or more to answer */ .event_handler = http_evt, .user_data = hdr,
                                    .disable_auto_redirect = true, .buffer_size = 1024, .buffer_size_tx = 1024};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    if (cookie && cookie[0]) { char ck[120]; snprintf(ck, sizeof ck, "sysauth=%s", cookie); esp_http_client_set_header(c, "Cookie", ck); }
    int blen = body ? (int)strlen(body) : 0, status = -1;
    if (esp_http_client_open(c, blen) == ESP_OK) {
        if (!blen || esp_http_client_write(c, body, blen) == blen) {
            esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
            uint8_t buf[512]; int n;
            while ((n = esp_http_client_read(c, (char *)buf, sizeof buf)) > 0) sink(sink_ctx, buf, n);
            if (n < 0) { ESP_LOGW(TAG, "read error %d", n); status = -1; }
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return status;
}

typedef struct { char *buf; size_t len, cap; } plain_sink_t;
static void plain_sink(void *ctx, const uint8_t *p, size_t n)
{
    plain_sink_t *s = ctx;
    size_t m = n < s->cap - 1 - s->len ? n : s->cap - 1 - s->len;
    memcpy(s->buf + s->len, p, m); s->len += m; s->buf[s->len] = 0;
}

static void url_for(const session_t *s, char *out, size_t cap, const char *path_after_stok)
{
    snprintf(out, cap, "%s/cgi-bin/luci/;stok=%s/%s", s->base, s->stok, path_after_stok);
}

// An encrypted request whose small reply is decrypted into `reply`. Returns the HTTP status (200 = ok) or -1.
static int api_small(session_t *s, bool login, const char *path, const char *json, char *reply, size_t cap, hdr_ctx_t *hdr)
{
    char url[200];
    url_for(s, url, sizeof url, path);
    char *body = build_body(s, login, json);
    if (!body) { ESP_LOGW(TAG, "could not build the request"); return -1; }
    pipe_t *p = calloc(1, sizeof *p);
    if (!p) { free(body); return -1; }
    pipe_init(p, s);
    p->collect = reply; p->ccap = cap; reply[0] = 0;
    int st = http_post(url, body, s->logged_in ? s->cookie : NULL, hdr, pipe_feed, p);
    bool ok = pipe_finish(p);
    mbedtls_aes_free(&p->aes);
    free(p); free(body);
    if (st == 200 && !ok) { ESP_LOGW(TAG, "%s: reply did not decrypt", path); return -1; }
    return st;
}

static void get_keys(const char *what, const char *url, char *nn, size_t nn_cap, char *ee, size_t ee_cap, long *seq, bool *ok)
{
    char reply[1024]; plain_sink_t sink = {reply, 0, sizeof reply};
    reply[0] = 0;
    int st = http_post(url, NULL, NULL, NULL, plain_sink, &sink);
    cJSON *j = st == 200 ? cJSON_Parse(reply) : NULL;
    cJSON *res = j ? cJSON_GetObjectItem(j, "result") : NULL;
    cJSON *arr = res ? cJSON_GetObjectItem(res, seq ? "key" : "password") : NULL;
    cJSON *n = cJSON_GetArrayItem(arr, 0), *e = cJSON_GetArrayItem(arr, 1);
    if (cJSON_IsString(n) && cJSON_IsString(e) && strlen(n->valuestring) < nn_cap && strlen(e->valuestring) < ee_cap) {
        strcpy(nn, n->valuestring); strcpy(ee, e->valuestring);
        if (seq) { cJSON *q = cJSON_GetObjectItem(res, "seq"); if (cJSON_IsNumber(q)) *seq = (long)q->valuedouble; else *ok = false; }
        ESP_LOGI(TAG, "%s: ok (%u-bit key)", what, (unsigned)strlen(nn) * 4);
    } else {
        ESP_LOGW(TAG, "%s: failed (HTTP %d)", what, st);
        *ok = false;
    }
    cJSON_Delete(j);
}

/* ---------- the whole exchange ---------- */

// One whole attempt. 0 = read OK, 1 = a transient failure (no reply, timeout: worth another go), 2 = not worth retrying
// (no password, or the Deco refused the login: never hammer a login).
static int fetch_once(void)
{
    int rc = 1;
    if (!wifi_wait_connected(15000)) return 1;
    session_t *s = calloc(1, sizeof *s);
    jx_t *jx = calloc(1, sizeof *jx);
    if (!s || !jx) { ESP_LOGW(TAG, "no memory"); free(s); free(jx); return 1; }
    int64_t t0 = esp_timer_get_time();
    const char *user = "admin";
#ifdef CONFIG_DECO_HTTP_TEST_BASE
    if (CONFIG_DECO_HTTP_TEST_BASE[0]) { strlcpy(s->base, CONFIG_DECO_HTTP_TEST_BASE, sizeof s->base); strlcpy(s->password, CONFIG_DECO_HTTP_TEST_PASSWORD, sizeof s->password); ESP_LOGW(TAG, "TEST MODE: talking to %s, not the Deco", s->base); }
    else
#endif
    { snprintf(s->base, sizeof s->base, "http://%s", g_cfg.deco_host); strlcpy(s->password, g_cfg.deco_pass, sizeof s->password); }
    if (!s->password[0]) { ESP_LOGW(TAG, "no Deco password set"); rc = 2; goto out; }
    ESP_LOGI(TAG, "reading the mesh node list (heap free %u, largest %u)", (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    char url[200]; bool ok = true;
    snprintf(url, sizeof url, "%s/cgi-bin/luci/;stok=/login?form=keys&operation=read", s->base);
    get_keys("password key", url, s->pw_nn, sizeof s->pw_nn, s->pw_ee, sizeof s->pw_ee, NULL, &ok);
    if (!ok) goto out;
    snprintf(url, sizeof url, "%s/cgi-bin/luci/;stok=/login?form=auth&operation=read", s->base);
    get_keys("signing key", url, s->sg_nn, sizeof s->sg_nn, s->sg_ee, sizeof s->sg_ee, &s->seq, &ok);
    if (!ok) goto out;

    // fresh AES key and IV: 16 hex characters each
    uint8_t rnd[16]; esp_fill_random(rnd, sizeof rnd);
    for (int i = 0; i < 8; i++) { sprintf(s->key + 2 * i, "%02x", rnd[i]); sprintf(s->iv + 2 * i, "%02x", rnd[8 + i]); }
    uint8_t md[16]; char hashin[80];
    snprintf(hashin, sizeof hashin, "%s%s", user, s->password);
    mbedtls_md5((const unsigned char *)hashin, strlen(hashin), md);
    for (int i = 0; i < 16; i++) sprintf(s->hash + 2 * i, "%02x", md[i]);
    memset(hashin, 0, sizeof hashin);

    // login
    char hexpw[300];
    if (!rsa_encrypt_hex(s->pw_nn, s->pw_ee, (const uint8_t *)s->password, strlen(s->password), hexpw, sizeof hexpw)) { ESP_LOGW(TAG, "could not encrypt the password"); rc = 2; goto out; }
    char login_json[420], reply[600];
    snprintf(login_json, sizeof login_json, "{\"params\":{\"password\":\"%s\"},\"operation\":\"login\"}", hexpw);
    hdr_ctx_t hdr = {0};
    int st = api_small(s, true, "login?form=login", login_json, reply, sizeof reply, &hdr);
    memset(login_json, 0, sizeof login_json);
    cJSON *lj = st == 200 ? cJSON_Parse(reply) : NULL;
    cJSON *stok = lj ? cJSON_GetObjectItem(cJSON_GetObjectItem(lj, "result"), "stok") : NULL;
    if (!cJSON_IsString(stok) || !hdr.cookie[0] || strlen(stok->valuestring) >= sizeof s->stok) {
        ESP_LOGW(TAG, "login failed (HTTP %d, %s)", st, st == 200 ? (lj ? "no session token or cookie" : "reply not JSON") : "no usable reply");
        cJSON_Delete(lj);
        rc = st == 200 ? 2 : 1;                 // a reply that decrypts but has no token is a refusal; no reply is just the network
        goto out;
    }
    strcpy(s->stok, stok->valuestring); strcpy(s->cookie, hdr.cookie); s->logged_in = true;
    cJSON_Delete(lj);
    ESP_LOGI(TAG, "login ok");

    // the node list, scanned as it streams in
    pipe_t *p = calloc(1, sizeof *p);
    char *body = build_body(s, false, "{\"operation\":\"read\"}");
    for (int attempt = 0; p && body && attempt < 2; attempt++) {      // one retry within the same login
        if (attempt) { ESP_LOGW(TAG, "node list: trying again"); vTaskDelay(pdMS_TO_TICKS(5000)); mbedtls_aes_free(&p->aes); memset(jx, 0, sizeof *jx); }
        pipe_init(p, s);
        p->scan = jx;
        url_for(s, url, sizeof url, "admin/device?form=device_list");
        int dst = http_post(url, body, s->cookie, NULL, pipe_feed, p);
        bool clean = pipe_finish(p);
        ESP_LOGI(TAG, "node list: HTTP %d, %u bytes decrypted%s, %d nodes, %d ms", dst, (unsigned)p->plain_total, clean ? "" : " (NOT clean)", jx->count, (int)((esp_timer_get_time() - t0) / 1000));
        if (dst == 200 && clean && jx->count > 0) {
            portENTER_CRITICAL(&s_mux);
            memcpy(s_nodes, jx->nodes, sizeof s_nodes); s_count = jx->count;
            portEXIT_CRITICAL(&s_mux);
#define M6(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
            for (int i = 0; i < s_count; i++) ESP_LOGI(TAG, "  node %d: %s %s \"%s\" mac %02x:%02x:%02x:%02x:%02x:%02x 2g %02x:%02x:%02x:%02x:%02x:%02x 5g %02x:%02x:%02x:%02x:%02x:%02x", i,
                                                       s_nodes[i].model, s_nodes[i].role, s_nodes[i].name, M6(s_nodes[i].mac), M6(s_nodes[i].bssid_2g), M6(s_nodes[i].bssid_5g));
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                deco_node_t m;
                ESP_LOGI(TAG, "connected to bssid %02x:%02x:%02x:%02x:%02x:%02x ch %d: %s", M6(ap.bssid), ap.primary,
                         deco_node_for_bssid(ap.bssid, &m) ? m.name : "no node matches");
            }
            evlog_event("mesh: %d nodes read from the Deco", s_count);
            rc = 0;
            break;
        }
    }
    if (p) mbedtls_aes_free(&p->aes);
    free(p); free(body);

    // logout, so the Deco's web session is not left open
    api_small(s, false, "admin/system?form=logout", "{\"operation\":\"logout\"}", reply, sizeof reply, NULL);
    ESP_LOGI(TAG, "logged out");
out:
    memset(s, 0, sizeof *s);
    free(s); free(jx);
    return rc;
}

void deco_nodes_fetch(void)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt) { ESP_LOGW(TAG, "reading the node list again in 15 s"); vTaskDelay(pdMS_TO_TICKS(15000)); }
        int rc = fetch_once();
        if (rc == 0) return;
        if (rc == 2) { evlog_event("mesh: node list not read (no password, or the Deco refused the login)"); return; }
    }
    evlog_event("mesh: node list could not be read this boot (network)");
}
