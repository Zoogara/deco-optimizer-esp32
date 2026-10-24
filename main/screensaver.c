#include "screensaver.h"
#include <stdlib.h>
#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "display.h"

static const char *TAG = "screensaver";

#define TIMEOUT_MS        ((uint32_t)CONFIG_DECO_SCREENSAVER_TIMEOUT_S * 1000u)
#define SNOW_TILE         64          /* px; the noise image is tiled across the screen */
#define SNOW_FRAME_MS     120
#define SNOW_DURATION_MS  2500
#define BACKLIGHT_ON_DELAY_MS 150     /* let LVGL repaint the UI before the backlight comes back */

typedef enum { AWAKE, SNOW, OFF } state_t;
static state_t s_state = AWAKE;
static lv_obj_t *s_overlay;
static lv_timer_t *s_anim;
static lv_color_t *s_noise;
static lv_img_dsc_t s_img;
static uint32_t s_snow_start;
static uint32_t s_rng = 2463534242u;

static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

static void fill_noise(void)
{
    for (int i = 0; i < SNOW_TILE * SNOW_TILE; i++) {
        uint8_t g = (uint8_t)(rnd() >> 24);
        s_noise[i] = lv_color_make(g, g, g);
    }
}

static void go_dark(void)
{
    if (s_anim) { lv_timer_del(s_anim); s_anim = NULL; }
    display_set_backlight(0);
    if (s_overlay) lv_obj_set_style_bg_img_src(s_overlay, NULL, 0);   /* plain black from here on */
    free(s_noise); s_noise = NULL;
    s_state = OFF;
    ESP_LOGI(TAG, "backlight off");
}

static void snow_frame(lv_timer_t *t)
{
    if (!s_noise || !s_overlay) return;
    fill_noise();
    lv_img_cache_invalidate_src(&s_img);
    lv_obj_invalidate(s_overlay);
    if (lv_tick_elaps(s_snow_start) >= SNOW_DURATION_MS) go_dark();
}

static void backlight_on_cb(lv_timer_t *t) { display_set_backlight(100); }

// LVGL lock held (or running inside an LVGL callback).
static void wake_locked(const char *why)
{
    lv_disp_trig_activity(NULL);
    if (s_state == AWAKE) return;
    state_t was = s_state;
    s_state = AWAKE;
    ESP_LOGI(TAG, "wake (%s)", why);
    if (s_anim) { lv_timer_del(s_anim); s_anim = NULL; }
    if (s_overlay) {
        lv_obj_set_style_bg_img_src(s_overlay, NULL, 0);   /* stop referencing the noise before it is freed */
        lv_obj_del_async(s_overlay);
        s_overlay = NULL;
    }
    free(s_noise); s_noise = NULL;
    if (was == OFF) {
        lv_timer_t *t = lv_timer_create(backlight_on_cb, BACKLIGHT_ON_DELAY_MS, NULL);
        lv_timer_set_repeat_count(t, 1);
    }
}

static void overlay_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) return;
    // The overlay is on the top layer and clickable, so this press never reached the UI below. Also make the
    // rest of this touch (until release) inert, so it cannot land on a button or switch once the overlay is gone.
    lv_indev_wait_release(lv_indev_get_act());
    wake_locked("touch");
}

static void enter_snow(void)
{
    ESP_LOGI(TAG, "idle %u s: snow", (unsigned)(TIMEOUT_MS / 1000));
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, overlay_cb, LV_EVENT_PRESSED, NULL);
    s_state = SNOW;
    s_snow_start = lv_tick_get();

    s_noise = malloc(SNOW_TILE * SNOW_TILE * sizeof(lv_color_t));
    if (!s_noise) {                       /* no RAM for the effect: just go dark */
        ESP_LOGW(TAG, "no memory for snow");
        go_dark();
        return;
    }
    fill_noise();
    s_img.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_img.header.always_zero = 0;
    s_img.header.w = SNOW_TILE;
    s_img.header.h = SNOW_TILE;
    s_img.data_size = SNOW_TILE * SNOW_TILE * sizeof(lv_color_t);
    s_img.data = (const uint8_t *)s_noise;
    lv_obj_set_style_bg_img_src(s_overlay, &s_img, 0);
    lv_obj_set_style_bg_img_tiled(s_overlay, true, 0);
    s_anim = lv_timer_create(snow_frame, SNOW_FRAME_MS, NULL);
}

static void check_cb(lv_timer_t *t)
{
    if (s_state == AWAKE && lv_disp_get_inactive_time(NULL) >= TIMEOUT_MS) enter_snow();
}

void screensaver_init(void)
{
    if (TIMEOUT_MS == 0) { ESP_LOGI(TAG, "disabled"); return; }
    lv_timer_create(check_cb, 1000, NULL);
    ESP_LOGI(TAG, "sleeps after %u s without touch", (unsigned)(TIMEOUT_MS / 1000));
}

void screensaver_wake(void)
{
    lvgl_port_lock(0);
    wake_locked("run");
    lvgl_port_unlock();
}
