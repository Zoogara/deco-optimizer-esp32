#include "ui.h"
#include <stdio.h>
#include <time.h>
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "history.h"

static ui_callbacks_t s_cb;
static lv_obj_t *s_verdict, *s_sub, *s_next, *s_run_btn, *s_run_lbl, *s_mode_sw, *s_mode_lbl, *s_hist;

static lv_color_t verdict_color(deco_verdict_t v)
{
    switch (v) {
    case DECO_OPTIMAL:
    case DECO_OPTIMIZED:   return lv_palette_main(LV_PALETTE_GREEN);
    case DECO_OPTIMIZABLE: return lv_palette_main(LV_PALETTE_AMBER);
    default:               return lv_palette_main(LV_PALETTE_RED);
    }
}

static void fmt_time(uint32_t ts, char *buf, size_t n, bool with_date)
{
    if (ts < 1700000000) { snprintf(buf, n, "--:--"); return; }   // clock not set yet
    time_t t = ts; struct tm tm; localtime_r(&t, &tm);
    if (with_date) strftime(buf, n, "%d/%m %H:%M", &tm); else strftime(buf, n, "%H:%M", &tm);
}

static void run_cb(lv_event_t *e) { if (s_cb.on_run) s_cb.on_run(); }
static void mode_cb(lv_event_t *e)
{
    bool opt = lv_obj_has_state(s_mode_sw, LV_STATE_CHECKED);
    lv_label_set_text(s_mode_lbl, opt ? "Mode: optimise" : "Mode: report");
    if (s_cb.on_mode) s_cb.on_mode(opt);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

void ui_init(const ui_callbacks_t *cb, bool optimize_initial)
{
    s_cb = *cb;
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t *tv = lv_tileview_create(scr);
    lv_obj_set_style_bg_color(tv, lv_color_black(), 0);
    lv_obj_t *t0 = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
    lv_obj_t *t1 = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT);

    /* --- status tile: a centred column that stays inside the round bezel --- */
    lv_obj_t *col = lv_obj_create(t0);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 190, 200);
    lv_obj_center(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 4, 0);

    lv_obj_t *title = make_label(col, &lv_font_montserrat_14, lv_color_hex(0x808080));
    lv_label_set_text(title, "DECO MESH");
    s_verdict = make_label(col, &lv_font_montserrat_20, lv_color_white());
    lv_label_set_text(s_verdict, "no data");
    s_sub = make_label(col, &lv_font_montserrat_14, lv_color_hex(0xB0B0B0));
    lv_label_set_text(s_sub, "");
    s_next = make_label(col, &lv_font_montserrat_14, lv_color_hex(0x808080));
    lv_label_set_text(s_next, "");

    s_run_btn = lv_btn_create(col);
    lv_obj_set_size(s_run_btn, 110, 38);
    lv_obj_add_event_cb(s_run_btn, run_cb, LV_EVENT_CLICKED, NULL);
    s_run_lbl = lv_label_create(s_run_btn);
    lv_label_set_text(s_run_lbl, "Run now");
    lv_obj_center(s_run_lbl);

    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 180, 34);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    s_mode_sw = lv_switch_create(row);
    lv_obj_set_size(s_mode_sw, 44, 24);
    if (optimize_initial) lv_obj_add_state(s_mode_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_mode_sw, mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_mode_lbl = make_label(row, &lv_font_montserrat_14, lv_color_white());
    lv_label_set_text(s_mode_lbl, optimize_initial ? "Mode: optimise" : "Mode: report");

    /* --- history tile --- */
    lv_obj_t *hcol = lv_obj_create(t1);
    lv_obj_remove_style_all(hcol);
    lv_obj_set_size(hcol, 190, 190);
    lv_obj_center(hcol);
    lv_obj_t *ht = make_label(hcol, &lv_font_montserrat_14, lv_color_hex(0x808080));
    lv_label_set_text(ht, "HISTORY");
    lv_obj_align(ht, LV_ALIGN_TOP_MID, 0, 0);
    s_hist = make_label(hcol, &lv_font_montserrat_14, lv_color_white());
    lv_obj_set_style_text_align(s_hist, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_recolor(s_hist, true);
    lv_obj_align(s_hist, LV_ALIGN_TOP_LEFT, 0, 24);
    lvgl_port_unlock();
    ui_refresh_history();
}

void ui_set_busy(const char *progress)
{
    lvgl_port_lock(0);
    lv_label_set_text(s_verdict, progress);
    lv_obj_set_style_text_color(s_verdict, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_label_set_text(s_sub, "");
    lv_obj_add_state(s_run_btn, LV_STATE_DISABLED);
    lvgl_port_unlock();
}

void ui_set_result(deco_verdict_t v, uint32_t ts, uint32_t duration_s, const char *detail)
{
    char buf[64], when[16];
    fmt_time(ts, when, sizeof when, true);
    if (v == DECO_ERROR && detail && *detail) snprintf(buf, sizeof buf, "%s\n%s", when, detail);
    else snprintf(buf, sizeof buf, "%s  (%us)", when, (unsigned)duration_s);
    lvgl_port_lock(0);
    lv_label_set_text(s_verdict, v == DECO_ERROR ? "ERROR" : deco_verdict_str(v));
    lv_obj_set_style_text_color(s_verdict, verdict_color(v), 0);
    lv_label_set_text(s_sub, buf);
    lv_obj_clear_state(s_run_btn, LV_STATE_DISABLED);
    lvgl_port_unlock();
    ui_refresh_history();
}

void ui_set_mode(bool optimize)
{
    lvgl_port_lock(0);
    if (optimize) lv_obj_add_state(s_mode_sw, LV_STATE_CHECKED); else lv_obj_clear_state(s_mode_sw, LV_STATE_CHECKED);
    lv_label_set_text(s_mode_lbl, optimize ? "Mode: optimise" : "Mode: report");
    lvgl_port_unlock();
}

void ui_set_next_run(uint32_t ts)
{
    char buf[32], t[16];
    if (ts) { fmt_time(ts, t, sizeof t, true); snprintf(buf, sizeof buf, "next: %s", t); } else buf[0] = 0;
    lvgl_port_lock(0);
    lv_label_set_text(s_next, buf);
    lvgl_port_unlock();
}

void ui_refresh_history(void)
{
    char buf[400] = ""; size_t n = 0;
    for (int i = 0; i < history_count() && i < 7; i++) {
        hist_entry_t e; history_get(i, &e);
        char when[16]; fmt_time(e.ts, when, sizeof when, true);
        const char *col = (e.verdict == DECO_ERROR) ? "FF5050" : (e.verdict == DECO_OPTIMIZABLE) ? "FFC000" : "50D060";
        n += snprintf(buf + n, sizeof buf - n, "%s #%s %s#%s\n", when, col, e.verdict == DECO_ERROR ? "error" : deco_verdict_str(e.verdict), (e.flags & 1) ? " *" : "");
    }
    if (!n) snprintf(buf, sizeof buf, "no runs yet");
    lvgl_port_lock(0);
    lv_label_set_text(s_hist, buf);
    lvgl_port_unlock();
}

void ui_show_setup(const char *ap_ssid, const char *ap_pass)
{
    char wifi_uri[96], buf[160];
    snprintf(wifi_uri, sizeof wifi_uri, "WIFI:T:WPA;S:%s;P:%s;;", ap_ssid, ap_pass);
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_t *title = make_label(scr, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_AMBER));
    lv_label_set_text(title, "SETUP");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_t *qr = lv_qrcode_create(scr, 110, lv_color_black(), lv_color_white());
    lv_qrcode_update(qr, wifi_uri, strlen(wifi_uri));
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 5, 0);
    lv_obj_t *t = make_label(scr, &lv_font_montserrat_14, lv_color_white());
    snprintf(buf, sizeof buf, "Scan to join, or join\n%s\npassword %s\nthen open 192.168.4.1", ap_ssid, ap_pass);
    lv_label_set_text(t, buf);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_scr_load(scr);
    lvgl_port_unlock();
}
