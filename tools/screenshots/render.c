/* Drives the real UI code with demo data and writes one PPM per scene. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "lvgl.h"
#include "ui.h"
#include "history.h"

uint32_t demo_ts(int day, int hour, int min);
void demo_fill_history(void);
void host_ui_goto_tile(int x);
void demo_device_info(ui_device_info_t *d);

#define W 240
#define H 240
static lv_color_t s_buf[W * H];
static lv_color_t s_fb[W * H];
static lv_disp_draw_buf_t s_draw;
static lv_disp_drv_t s_drv;

static void flush_cb(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px)
{
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++)
            s_fb[y * W + x] = *px++;
    lv_disp_flush_ready(d);
}

static void settle(void)
{
    for (int i = 0; i < 60; i++) { lv_tick_inc(25); lv_timer_handler(); }
}

static void save(const char *dir, const char *name)
{
    settle();
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned char rgb[3] = {s_fb[i].ch.red, s_fb[i].ch.green, s_fb[i].ch.blue};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

static lv_obj_t *s_screen;
static void fresh_screen(void)
{
    lv_obj_t *old = s_screen;
    s_screen = lv_obj_create(NULL);
    lv_scr_load(s_screen);
    if (old) lv_obj_del(old);
}

static void on_run(void) {}
static void on_mode(bool o) {}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "out";
    setenv("TZ", "UTC", 1); tzset();
    lv_init();
    lv_disp_draw_buf_init(&s_draw, s_buf, NULL, W * H);
    lv_disp_drv_init(&s_drv);
    s_drv.draw_buf = &s_draw; s_drv.flush_cb = flush_cb; s_drv.hor_res = W; s_drv.ver_res = H;
    lv_disp_drv_register(&s_drv);
    demo_fill_history();
    const ui_callbacks_t cb = {.on_run = on_run, .on_mode = on_mode};
    uint32_t next = demo_ts(15, 3, 0), last = demo_ts(14, 3, 0);

    fresh_screen(); ui_init(&cb, false); ui_set_next_run(next);
    ui_set_result(DECO_OPTIMAL, last, 12, "");
    save(dir, "status_optimal");

    fresh_screen(); ui_init(&cb, false); ui_set_next_run(next);
    ui_set_result(DECO_OPTIMIZABLE, demo_ts(12, 18, 40), 13, "");
    save(dir, "status_optimizable");

    fresh_screen(); ui_init(&cb, false); ui_set_next_run(next);
    ui_set_busy("scanning");
    save(dir, "status_scanning");

    fresh_screen(); ui_init(&cb, true); ui_set_next_run(next);
    ui_set_result(DECO_OPTIMIZED, demo_ts(12, 18, 42), 34, "");
    save(dir, "status_optimise_mode");

    fresh_screen(); ui_init(&cb, false); ui_set_next_run(next);
    ui_set_result(DECO_ERROR, demo_ts(12, 3, 0), 0, "ssh login failed");
    save(dir, "status_error");

    fresh_screen(); ui_init(&cb, false); ui_set_next_run(next);
    ui_set_result(DECO_OPTIMAL, last, 12, "");
    host_ui_goto_tile(1);
    save(dir, "history");

    fresh_screen(); ui_init(&cb, false); ui_set_next_run(next);
    ui_set_result(DECO_OPTIMAL, last, 12, "");
    ui_device_info_t di; demo_device_info(&di); ui_set_device_info(&di);
    host_ui_goto_tile(2);
    save(dir, "device");

    ui_show_setup("Deco-Setup-A1B2", "k7m3x9pq");
    save(dir, "setup");
    return 0;
}
