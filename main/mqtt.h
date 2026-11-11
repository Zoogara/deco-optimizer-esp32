// MQTT status/commands with Home Assistant MQTT discovery.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "history.h"

typedef struct {
    void (*on_run)(void);
    void (*on_mode)(bool optimize);   // from Home Assistant; the app applies it and updates the UI
} mqtt_callbacks_t;

void mqtt_start(const mqtt_callbacks_t *cb, bool optimize_initial);
bool mqtt_is_connected(void);
void mqtt_publish_busy(const char *progress);                    // run in progress
void mqtt_publish_result(const hist_entry_t *e, const char *detail);
void mqtt_publish_mode(bool optimize);
void mqtt_publish_next(uint32_t ts);
#include "pings.h"
void mqtt_publish_pings(const ping_round_t *r);                  // one JSON message per round
#include "settings.h"
void mqtt_ping_discovery_refresh(const ping_dev_t *old);         // after the device list was edited; old = the list before the edit (or NULL)                             // 0 = none
