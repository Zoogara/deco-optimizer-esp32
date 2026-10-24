/* Stand-in for ESP-IDF's esp_lvgl_port: on the host there is one thread, so the LVGL lock does nothing. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
static inline bool lvgl_port_lock(uint32_t timeout_ms) { (void)timeout_ms; return true; }
static inline void lvgl_port_unlock(void) {}
