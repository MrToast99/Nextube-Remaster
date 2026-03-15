/**
 * @file main.c
 * @brief Nextube open-source firmware – main entry
 *
 * Task architecture (mirrors original firmware's FreeRTOS design):
 *   TaskDisplay      – renders clock / modes on 6× ST7735 LCDs
 *   TaskWifiServer   – captive-portal AP + STA, embedded web UI
 *   TaskNtp          – NTP time synchronisation
 *   TaskWeather      – OpenWeatherMap polling
 *   TaskYoutubeAndBili – YouTube / Bilibili subscriber counts
 *   TaskIIC          – RTC + SHT30 I²C sensor polling
 *   TaskLed          – WS2812 LED effects
 *   TaskAudio        – WAV / tone playback via DAC
 *   TaskButton       – Capacitive touch input
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_spiffs.h"

#include "board_pins.h"
#include "config_mgr.h"
#include "display.h"
#include "audio.h"
#include "leds.h"
#include "touch_input.h"
#include "rtc_pcf8563.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "ntp_time.h"
#include "weather.h"
#include "youtube_bili.h"

static const char *TAG = "main";

/* ── Touch handler ─────────────────────────────────────────────────── */
static void on_touch(touch_pad_id_t pad)
{
    const nextube_config_t *cfg = config_get();
    static const char *mode_names[] = {
        "Clock","Countdown","Scoreboard","Pomodoro","YouTube","CustomClock","Album"
    };
    char json[80];

    switch (pad) {
    case TOUCH_LEFT: {
        /* Step backward; skip modes disabled in enabled_modes bitmask */
        int m = (int)cfg->current_mode;
        for (int tries = 0; tries < APP_MODE_MAX; tries++) {
            m = (m - 1 + APP_MODE_MAX) % APP_MODE_MAX;
            if (cfg->enabled_modes & (1 << m)) break;
        }
        snprintf(json, sizeof(json), "{\"apps\":[{\"app\":\"%s\"}]}", mode_names[m]);
        config_set_json(json, strlen(json));
        break;
    }
    case TOUCH_RIGHT: {
        /* Step forward; skip modes disabled in enabled_modes bitmask */
        int m = (int)cfg->current_mode;
        for (int tries = 0; tries < APP_MODE_MAX; tries++) {
            m = (m + 1) % APP_MODE_MAX;
            if (cfg->enabled_modes & (1 << m)) break;
        }
        snprintf(json, sizeof(json), "{\"apps\":[{\"app\":\"%s\"}]}", mode_names[m]);
        config_set_json(json, strlen(json));
        break;
    }
    case TOUCH_MIDDLE: {
        /* Toggle backlight on/off */
        const char *j = cfg->backlight_on
            ? "{\"backlight_onoff\":\"OFF\"}"
            : "{\"backlight_onoff\":\"ON\"}";
        config_set_json(j, strlen(j));
        break;
    }
    }
}

/* ── SPIFFS mount ──────────────────────────────────────────────────── */
static void init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path       = "/spiffs",
        .partition_label = "spiffs",
        .max_files       = 20,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("spiffs", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%u  used=%u", (unsigned)total, (unsigned)used);
}

/* ── NVS init ──────────────────────────────────────────────────────── */
static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS: erasing and re-init");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* ── Application entry ─────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  Nextube Open-Source Firmware v1.0             ║");
    ESP_LOGI(TAG, "║  https://github.com/MrToast99/Nextube-Remaster ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════╝");

    /* Core initialisations */
    init_nvs();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    init_spiffs();

    /* Load configuration from /spiffs/config.json (or defaults) */
    config_mgr_init();
    const nextube_config_t *cfg = config_get();

    /* Hardware drivers */
    display_init();
    display_task_start();          /* launch 5 Hz FreeRTOS display task */

    audio_init();
    audio_set_volume(cfg->volume); /* restore saved volume level */

    leds_init();
    leds_task_start();
    touch_input_init();
    touch_input_register_callback(on_touch);
    rtc_init();

    /* Networking – start AP+STA, then web server */
    wifi_manager_start();
    web_server_start();

    /* Background services */
    ntp_time_start();
    weather_start();
    youtube_bili_start();

    ESP_LOGI(TAG, "All tasks launched – heap free: %u bytes",
             (unsigned)esp_get_free_heap_size());
}
