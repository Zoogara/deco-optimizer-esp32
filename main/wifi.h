#pragma once
#include <stdbool.h>
#include <stdint.h>
void wifi_start(void);                       // non-blocking; reconnects automatically
bool wifi_wait_connected(uint32_t timeout_ms);
bool wifi_is_connected(void);
void wifi_kick(void);                        // drop the association and rescan (recovers a stale link)
// Pings the gateway; if the current mesh node is losing/delaying replies, avoids it and moves to the
// next-strongest node. Returns false only if it needed to switch and could not rejoin. Blocking (~2-25 s).
bool wifi_check_link(void);
