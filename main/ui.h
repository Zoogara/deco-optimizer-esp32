// Status page + history page on the round display (LVGL tileview, swipe left/right).
#pragma once
#include <stdbool.h>
#include "deco.h"

typedef struct {
    void (*on_run)(void);                  // Run button pressed
    void (*on_mode)(bool optimize);        // mode switch changed
} ui_callbacks_t;

void ui_init(const ui_callbacks_t *cb, bool optimize_initial);
void ui_set_busy(const char *progress);                 // shows progress text, disables Run
void ui_set_result(deco_verdict_t v, uint32_t ts, uint32_t duration_s, const char *detail);
void ui_set_mode(bool optimize);                        // reflect a mode change made elsewhere (MQTT)
void ui_set_next_run(uint32_t ts);                      // 0 = unknown / disabled
typedef struct {
    bool wifi_up;
    char ip[16];
    char host[28];            // e.g. deco-optimizer.local
    char ssid[33];
    int rssi;
    bool mqtt_enabled, mqtt_connected;
    uint32_t uptime_s;
    uint32_t free_kb;
    char version[24];
} ui_device_info_t;
void ui_set_device_info(const ui_device_info_t *info);   // the third page; call every few seconds

void ui_refresh_history(void);
void ui_show_setup(const char *ap_ssid, const char *ap_pass);   // setup-mode screen with a join-Wi-Fi QR code
