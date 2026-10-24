// Screensaver: after a period without touch the screen shows a burst of "snow" (TV static), then the backlight goes
// off. A touch wakes it, and that touch is swallowed instead of reaching the widgets underneath. A run wakes it too.
#pragma once
void screensaver_init(void);     // from ui_init(), in LVGL context. Timeout is CONFIG_DECO_SCREENSAVER_TIMEOUT_S (0 = off)
void screensaver_wake(void);     // any task: wake the screen (if asleep) and restart the inactivity timer
