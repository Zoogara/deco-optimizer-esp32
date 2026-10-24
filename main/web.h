// Web page + JSON API for status, control and all settings (HTTP Basic auth, user "admin").
#pragma once
#include <stdbool.h>
typedef struct {
    void (*on_run)(void);
    void (*on_mode)(bool optimize);   // saves the mode and updates screen + MQTT
} web_callbacks_t;
void web_start(const web_callbacks_t *cb);
