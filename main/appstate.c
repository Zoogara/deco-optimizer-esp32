#include "appstate.h"
#include <string.h>
static char s_state[40] = "starting";
static volatile bool s_busy;
static volatile uint32_t s_next;
void app_state_set(const char *s) { strlcpy(s_state, s, sizeof s_state); }
void app_state_get(char *out, int cap) { strlcpy(out, s_state, cap); }
void app_busy_set(bool b) { s_busy = b; }
bool app_busy_get(void) { return s_busy; }
void app_next_set(uint32_t ts) { s_next = ts; }
uint32_t app_next_get(void) { return s_next; }
