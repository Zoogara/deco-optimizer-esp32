// Ping monitor: every N minutes it pings the gateway, the Deco node the board is connected to and up to seven devices
// from the settings, and keeps the last 12 hours of replies in RAM for the web page. Each round is also published to
// Home Assistant over MQTT, which keeps the long-term history.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define PING_MAX_DEV   7
#define PING_SERIES    (PING_MAX_DEV + 2)     // 0 = gateway, 1 = connected node, 2.. = the devices
#define PING_COUNT_DEV 5                      // pings per target per round
#define PING_ROUNDS    144                    // 12 hours at the shortest interval (5 min)

typedef struct {
    uint16_t avg, min;                        // ms over the replies that came
    uint8_t lost;                             // of PING_COUNT_DEV
    uint8_t sent;                             // 0 = no target in this slot
} ping_s_t;

typedef struct {
    uint32_t ts;                              // 0 if the clock was not set
    uint8_t bssid[6];                         // the node the board was on
    ping_s_t s[PING_SERIES];
} ping_round_t;

// One round. Blocking (10-60 s); call from the worker task only, never while a scan is running.
void pings_round(void);
int pings_count(void);
bool pings_get(int i, ping_round_t *out);     // i = 0 is the oldest round kept
