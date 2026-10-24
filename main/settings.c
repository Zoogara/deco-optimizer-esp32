#include "settings.h"
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "config.h"

#define SETTINGS_MAGIC 0xDEC00002u
static const char *TAG = "settings";

settings_t g_cfg;

static void set_defaults(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.magic = SETTINGS_MAGIC;
    strcpy(g_cfg.deco_host, "192.168.0.1");
    strcpy(g_cfg.mqtt_uri, "mqtt://192.168.0.100:1883");
    g_cfg.sched_en = true;
    g_cfg.sched_h = SCHEDULE_DEFAULT_HOUR;
    g_cfg.sched_m = SCHEDULE_DEFAULT_MINUTE;
}

void settings_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof g_cfg;
    static settings_t tmp;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        esp_err_t e = nvs_get_blob(h, "all", &tmp, &len);
        nvs_close(h);
        if (e == ESP_OK && len == sizeof tmp && tmp.magic == SETTINGS_MAGIC) {
            g_cfg = tmp;
            ESP_LOGI(TAG, "loaded from encrypted NVS");
            return;
        }
    }
    set_defaults();
    ESP_LOGW(TAG, "no stored settings: defaults, setup required");
}

bool settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "all", &g_cfg, sizeof g_cfg) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool settings_wifi_configured(void) { return g_cfg.wifi_ssid[0] != 0; }

void settings_erase_all(void)
{
    nvs_flash_deinit();
    nvs_flash_erase();
}
