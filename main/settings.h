// All configuration and secrets, kept as one versioned blob in the encrypted NVS partition.
// The HMAC-derived NVS key lives in an eFuse, so a flash dump contains only ciphertext.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t magic;          // SETTINGS_MAGIC | version
    char wifi_ssid[33];
    char wifi_pass[65];
    char deco_host[40];
    char deco_email[64];
    char deco_pass[64];
    char mqtt_uri[96];
    char mqtt_user[40];
    char mqtt_pass[64];
    char admin_pass[40];     // web page password
    uint8_t hostkey_sha1[20];
    bool hostkey_set;        // false until the first successful SSH handshake pins the Deco's host key
    bool optimize;           // persisted mode: false = report only
    bool sched_en;
    uint8_t sched_h, sched_m;
    bool wifi_verified;      // false right after setup until the first successful connect
} settings_t;

extern settings_t g_cfg;

// Loads from NVS, or applies defaults (setup is then required).
void settings_load(void);
bool settings_save(void);
bool settings_wifi_configured(void);
void settings_erase_all(void);        // wipes all settings, secrets and history; caller restarts
