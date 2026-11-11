// All configuration and secrets, kept as one versioned blob in the encrypted NVS partition.
// The HMAC-derived NVS key lives in an eFuse, so a flash dump contains only ciphertext.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct { char label[16]; uint32_t ip; } ping_dev_t;     // ip in network byte order, 0 = slot unused

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
    uint8_t report_every_h;  // 0 = off; else 1,2,3,4,6,8 or 12: a report-only scan every N hours, in step with the daily run
    uint8_t ping_every_min;  // 0 = off; else 5,10,15,20,30 or 60: ping the gateway, the connected node and the devices below
    uint16_t ping_size;      // payload bytes per ping (0 = the default, 1000)
    ping_dev_t ping_dev[7];
} settings_t;

extern settings_t g_cfg;

// Loads from NVS, or applies defaults (setup is then required).
void settings_load(void);
bool settings_save(void);
bool settings_wifi_configured(void);
void settings_erase_all(void);        // wipes all settings, secrets and history; caller restarts
