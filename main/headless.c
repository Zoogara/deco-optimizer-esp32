// Stand-ins for the display and UI in a DECO_HEADLESS build. The rest of the firmware is unchanged.
#include "display.h"
#include "ui.h"
#include "esp_log.h"

static const char *TAG = "headless";

void display_init(void) {}
void display_orient(bool swap_xy, bool mirror_x, bool mirror_y) {}
void display_set_backlight(int percent) {}

void ui_init(const ui_callbacks_t *cb, bool optimize_initial) { ESP_LOGI(TAG, "headless build: no display"); }
void ui_set_busy(const char *progress) {}
void ui_set_result(deco_verdict_t v, uint32_t ts, uint32_t duration_s, const char *detail) {}
void ui_set_mode(bool optimize) {}
void ui_set_next_run(uint32_t ts) {}
void ui_refresh_history(void) {}

void ui_show_setup(const char *ap_ssid, const char *ap_pass)
{
    ESP_LOGW(TAG, "SETUP MODE: join the Wi-Fi network \"%s\" (password: DECO_SETUP_AP_PASSWORD from the build), "
                  "then open http://192.168.4.1/", ap_ssid);
}
