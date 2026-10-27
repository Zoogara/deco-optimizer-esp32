// Two kinds of log, chosen so neither can wear out the flash or starve the RAM the SSH handshake needs:
//  * a RAM ring buffer (4 KB, static) that captures everything the firmware logs; lost on restart.
//  * a short persistent event log in the encrypted NVS: boot reasons, run outcomes with the failure reason,
//    forced Wi-Fi reconnects, self-heal reboots. A handful of writes per day, rate-limited, one small record each.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define EVLOG_EVENT_SLOTS 24
#define EVLOG_EVENT_TEXT  112
#define EVLOG_RAM_SIZE    4096

typedef struct {
    uint32_t ts;        // unix time, 0 if the clock was not set yet
    uint32_t uptime_s;  // seconds since boot when it was recorded
    char text[EVLOG_EVENT_TEXT];
} evlog_event_t;

void evlog_init(void);                              // installs the log hook; call first thing in app_main
void evlog_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // persistent; also goes to the log
int evlog_events_read(evlog_event_t *out, int max);                   // newest first; returns the count
size_t evlog_ram_copy(char *out, size_t cap);       // oldest to newest, NUL terminated; cap >= EVLOG_RAM_SIZE + 1
