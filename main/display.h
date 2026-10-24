// 240x240 GC9A01 round LCD + CST816S touch, wired into LVGL.
#pragma once
#include <stdbool.h>
void display_init(void);
void display_orient(bool swap_xy, bool mirror_x, bool mirror_y);
void display_set_backlight(int percent);     // 0-100, PWM on the backlight pin
