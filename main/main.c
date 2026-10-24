// Deco optimizer: scans/optimizes a TP-Link Deco mesh on demand or on a daily schedule.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "config.h"
#include "display.h"
#include "wifi.h"
#include "deco.h"
#include "history.h"
#include "ui.h"
#include "mqtt.h"
#include "web.h"
#include "appstate.h"
#include "settings.h"
#include "provision.h"
#include "driver/gpio.h"

static const char *TAG = "app";

typedef enum { CMD_RUN_MANUAL, CMD_RUN_SCHEDULED, CMD_RUN_STARTUP } cmd_t;
static QueueHandle_t s_cmds;
static volatile bool s_busy;
#define NET_DEAD_REBOOT_S 600          // see scheduler_task: last-resort self-heal
static volatile int64_t s_last_net_ok_us;

static void on_run(void) { cmd_t c = CMD_RUN_MANUAL; xQueueSend(s_cmds, &c, 0); }
static void on_mode(bool opt)                 // from the touch UI
{
    g_cfg.optimize = opt; settings_save(); mqtt_publish_mode(opt);
    ESP_LOGI(TAG, "mode: %s", opt ? "optimise" : "report");
}

static void on_mode_remote(bool opt)          // from Home Assistant: also move the on-screen switch
{
    ui_set_mode(opt);
    on_mode(opt);
}

static void set_next(time_t t) { app_next_set((uint32_t)t); ui_set_next_run((uint32_t)t); mqtt_publish_next((uint32_t)t); }

static void progress(const char *msg) { app_state_set(msg); ui_set_busy(msg); mqtt_publish_busy(msg); ESP_LOGI(TAG, "%s", msg); }

// A scan that times out or gets no answer is retried on a fresh connection (the Deco app does this too).
static bool is_retryable(const deco_result_t *r)
{
    return r->verdict == DECO_ERROR && (strstr(r->detail, "scan") || strstr(r->detail, "timed") || strstr(r->detail, "TMP"));
}

static void worker_task(void *arg)
{
    cmd_t c;
    for (;;) {
        if (xQueueReceive(s_cmds, &c, portMAX_DELAY) != pdTRUE) continue;
        if (s_busy) continue;
        s_busy = true; app_busy_set(true);
        bool optimize = (c == CMD_RUN_STARTUP) ? false : g_cfg.optimize;   // a reboot must never optimise
        progress("checking wifi");
        wifi_check_link();      // moves off a mesh node that is not passing traffic
        deco_result_t r;
        for (int attempt = 0; attempt < 3; attempt++) {
            if (attempt) { progress("retrying"); wifi_kick(); vTaskDelay(pdMS_TO_TICKS(5000)); }
            deco_run(optimize, &r, progress);
            if (!is_retryable(&r)) break;
        }
        time_t now = time(NULL);
        hist_entry_t e = {.ts = now > 1700000000 ? (uint32_t)now : 0, .duration_s = r.duration_s > 65535 ? 65535 : r.duration_s,
                          .verdict = r.verdict, .flags = (optimize ? 1 : 0) | (c == CMD_RUN_SCHEDULED ? 2 : 0) | (c == CMD_RUN_STARTUP ? 4 : 0)};
        if (r.verdict != DECO_ERROR) s_last_net_ok_us = esp_timer_get_time();
        history_add(&e);
        app_state_set(r.verdict == DECO_ERROR ? "error" : deco_verdict_str(r.verdict));
        ui_set_result(r.verdict, e.ts, r.duration_s, r.detail);
        mqtt_publish_result(&e, r.detail);
        ESP_LOGI(TAG, "[heap] free=%u min=%u largest=%u", (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        s_busy = false; app_busy_set(false);
    }
}

// Next local occurrence of HH:MM after 'from'.
static time_t next_schedule(time_t from)
{
    struct tm tm; localtime_r(&from, &tm);
    tm.tm_hour = g_cfg.sched_h; tm.tm_min = g_cfg.sched_m; tm.tm_sec = 0; tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t <= from) { tm.tm_mday++; tm.tm_isdst = -1; t = mktime(&tm); }
    return t;
}

// Holding BOOT (GPIO9) for 5 s while running clears the Wi-Fi details and restarts into setup mode.
// (Holding it *during reset* is what enters the ROM download mode, so this only acts once running.)
#define BOOT_PIN 9
static void boot_button_task(void *arg)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << BOOT_PIN, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
    int held = 0;
    for (;;) {
        held = gpio_get_level(BOOT_PIN) == 0 ? held + 1 : 0;
        if (held >= 50) {                                  // 50 x 100 ms
            ESP_LOGW(TAG, "BOOT held 5 s: clearing Wi-Fi settings, restarting into setup");
            g_cfg.wifi_ssid[0] = 0; g_cfg.wifi_pass[0] = 0; g_cfg.wifi_verified = false;
            settings_save();
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Credentials entered in setup that never produce an IP are cleared, so a typo cannot strand the device.
// Established (verified) credentials are never cleared automatically.
static void unverified_wifi_watchdog(void *arg)
{
    for (int i = 0; i < 90 && !g_cfg.wifi_verified; i++) vTaskDelay(pdMS_TO_TICKS(1000));
    if (!g_cfg.wifi_verified) {
        ESP_LOGE(TAG, "new Wi-Fi settings never connected: clearing them, restarting into setup");
        g_cfg.wifi_ssid[0] = 0; g_cfg.wifi_pass[0] = 0;
        settings_save();
        esp_restart();
    }
    vTaskDelete(NULL);
}

// MQTT is started only once Wi-Fi has an IP; starting earlier fails and costs a retry delay.
static void mqtt_boot_task(void *arg)
{
    wifi_wait_connected(portMAX_DELAY);
    static const mqtt_callbacks_t mcb = {.on_run = on_run, .on_mode = on_mode_remote};
    mqtt_start(&mcb, g_cfg.optimize);
    vTaskDelete(NULL);
}

// Last-resort self-heal: if nothing has reached the network for a long time and we are idle, reboot.
// One report-only scan shortly after every boot, so the result and Home Assistant entities are populated
// even when no history survived (first run, wiped storage, repeated reboots).
static void startup_scan_task(void *arg)
{
    wifi_wait_connected(portMAX_DELAY);
    for (int i = 0; i < 30 && g_cfg.mqtt_user[0] && !mqtt_is_connected(); i++) vTaskDelay(pdMS_TO_TICKS(1000));
    for (int i = 0; i < 30 && time(NULL) < 1700000000; i++) vTaskDelay(pdMS_TO_TICKS(1000));   // wait for the clock
    vTaskDelay(pdMS_TO_TICKS(2000));
    cmd_t c = CMD_RUN_STARTUP;
    xQueueSend(s_cmds, &c, 0);
    vTaskDelete(NULL);
}

static void web_boot_task(void *arg)
{
    wifi_wait_connected(portMAX_DELAY);
    static const web_callbacks_t wcb = {.on_run = on_run, .on_mode = on_mode_remote};
    web_start(&wcb);
    vTaskDelete(NULL);
}

static void scheduler_task(void *arg)
{
    time_t next = 0;
    s_last_net_ok_us = esp_timer_get_time();
    for (;;) {
        if (mqtt_is_connected()) s_last_net_ok_us = esp_timer_get_time();
        if (!s_busy && esp_timer_get_time() - s_last_net_ok_us > (int64_t)NET_DEAD_REBOOT_S * 1000000) {
            ESP_LOGE(TAG, "no network contact for %d s, rebooting", NET_DEAD_REBOOT_S);
            esp_restart();
        }
        time_t now = time(NULL);
        if (now > 1700000000 && g_cfg.sched_en) {     // clock is set and the schedule is enabled
            if (!next) { next = next_schedule(now); set_next(next); ESP_LOGI(TAG, "next scheduled run at %lld", (long long)next); }
            if (now >= next) {
                cmd_t c = CMD_RUN_SCHEDULED; xQueueSend(s_cmds, &c, 0);
                next = next_schedule(now + 60);
                set_next(next);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

void app_main(void)
{
    // With CONFIG_NVS_ENCRYPTION + HMAC scheme this also derives the encryption keys; on the very first
    // boot it generates the HMAC key inside the chip and burns it to eFuse (one-time, irreversible).
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND || e == ESP_ERR_NVS_CORRUPT_KEY_PART || e == ESP_ERR_NVS_WRONG_ENCRYPTION) {
        ESP_LOGW(TAG, "NVS not usable (%s): erasing the partition (expected once, when moving to encrypted NVS)", esp_err_to_name(e));
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) { ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(e)); abort(); }
    settings_load();
    setenv("TZ", DECO_TZ, 1); tzset();
    display_init();
    xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 3, NULL);
    if (!settings_wifi_configured()) {      // first run, or BOOT-hold recovery
        provision_start();
        return;
    }
    history_init();
    const ui_callbacks_t cb = {.on_run = on_run, .on_mode = on_mode};
    ui_init(&cb, g_cfg.optimize);
    hist_entry_t last;
    bool have_last = history_get(0, &last);
    if (have_last) { ui_set_result(last.verdict, last.ts, last.duration_s, ""); app_state_set(last.verdict == DECO_ERROR ? "error" : deco_verdict_str(last.verdict)); }

    s_cmds = xQueueCreate(4, sizeof(cmd_t));
    wifi_start();
    if (!g_cfg.wifi_verified) xTaskCreate(unverified_wifi_watchdog, "wifi_wd", 3072, NULL, 3, NULL);
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    xTaskCreate(mqtt_boot_task, "mqtt_boot", 3072, NULL, 4, NULL);
    xTaskCreate(web_boot_task, "web_boot", 3072, NULL, 4, NULL);
    xTaskCreate(startup_scan_task, "startup_scan", 3072, NULL, 4, NULL);
    if (have_last) mqtt_publish_result(&last, "");   // seed state from the stored history; sent once connected
    xTaskCreate(worker_task, "deco", 16384, NULL, 5, NULL);
    xTaskCreate(scheduler_task, "sched", 3072, NULL, 4, NULL);
}
