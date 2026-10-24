// Small shared status record (written by the app, read by the web page).
#pragma once
#include <stdbool.h>
#include <stdint.h>
void app_state_set(const char *s);
void app_state_get(char *out, int cap);
void app_busy_set(bool b);
bool app_busy_get(void);
void app_next_set(uint32_t ts);
uint32_t app_next_get(void);
