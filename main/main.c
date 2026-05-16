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
#include <sys/stat.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_littlefs.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"

#include "board_pins.h"
#include "config_mgr.h"
#include "display.h"
#include "audio.h"
#include "leds.h"
#include "touch_input.h"
#include "rtc_pcf8563.h"
#include "sht30.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "ntp_time.h"
#include "weather.h"
#include "youtube_bili.h"
#include "microphone.h"
#include "fw_version.h"

static const char *TAG = "main";

/* Verify that the config's band count stays in sync with the mic driver.
 * main.c includes both headers so this is the natural place for the check. */
_Static_assert(CFG_MIC_BAND_COUNT == MIC_BAND_COUNT,
               "CFG_MIC_BAND_COUNT in config_mgr.h must equal MIC_BAND_COUNT in microphone.h");

/* ── Heap telemetry ───────────────────────────────────────────────────
 * Logs internal-RAM and PSRAM free-size + largest-block every 5 minutes.
 * largest-block is the real fragmentation indicator: a healthy device with
 * 200 KB free / 180 KB largest is fine; 200 KB free / 8 KB largest is in
 * trouble even though aggregate free looks the same.  The same numbers
 * are exposed via /api/status so the System tab can chart them.
 *
 * Note on the per-cap queries: esp_get_free_heap_size() returns the total
 * across ALL caps (internal + PSRAM combined when SPIRAM_USE_MALLOC=y),
 * which is misleading on ESP32-WROVER where internal SRAM is ~320 KB and
 * PSRAM contributes the bulk.  We use heap_caps_get_free_size(CAP_INTERNAL)
 * and CAP_SPIRAM directly so each line is unambiguous. */
static void heap_telemetry_task(void *arg)
{
    (void)arg;
    /* Wait one cycle before the first log so boot-time peaks settle. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
        ESP_LOGI("heap",
                 "internal: free=%u largest=%u  psram: free=%u largest=%u  (lifetime min total: %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned)esp_get_minimum_free_heap_size());
    }
}

/* ── WiFi-safe warm-boot ───────────────────────────────────────────────
 * After esptool flashes and hard-resets via the EN pin (ESP_RST_EXT) the
 * WiFi PHY is left in an unclean state and the soft-AP silently fails to
 * start.  A firmware-initiated esp_restart() resolves it cleanly.
 *
 * RTC fast memory persists through software resets but is cleared by any
 * hardware reset (EN pin / power-on), so s_warm_boot is 0 whenever we
 * come from a hard reset and WARM_BOOT_MAGIC otherwise.  We restart exactly
 * once per hard-reset without looping. */
RTC_DATA_ATTR static uint32_t s_warm_boot;
#define WARM_BOOT_MAGIC  0x574F524Du   /* "WORM" */

/* ── Touch handler ─────────────────────────────────────────────────── */
static void on_touch(touch_pad_id_t pad)
{
    app_mode_t  current_mode;
    uint16_t    enabled_modes;
    bool        backlight_on;
    bool        button_sound;
    char        click_file[64];

    config_lock();
    const nextube_config_t *cfg = config_get();
    current_mode  = cfg->current_mode;
    enabled_modes = cfg->enabled_modes;
    backlight_on  = cfg->backlight_on;
    button_sound  = cfg->button_sound;
    strncpy(click_file, cfg->click_file, sizeof(click_file) - 1);
    click_file[sizeof(click_file) - 1] = '\0';
    config_unlock();

    switch (pad) {
    case TOUCH_LEFT: {
        /* Step backward; skip modes disabled in enabled_modes bitmask.
         * config_set_mode() updates RAM only — no flash write per button press. */
        int m = (int)current_mode;
        for (int tries = 0; tries < APP_MODE_MAX; tries++) {
            m = (m - 1 + APP_MODE_MAX) % APP_MODE_MAX;
            if (enabled_modes & (1 << m)) break;
        }
        config_set_mode((app_mode_t)m);
        break;
    }
    case TOUCH_RIGHT: {
        /* Step forward; skip modes disabled in enabled_modes bitmask. */
        int m = (int)current_mode;
        for (int tries = 0; tries < APP_MODE_MAX; tries++) {
            m = (m + 1) % APP_MODE_MAX;
            if (enabled_modes & (1 << m)) break;
        }
        config_set_mode((app_mode_t)m);
        break;
    }
    case TOUCH_MIDDLE: {
        /* In countdown / pomodoro: start / stop the timer.
         * In all other modes: toggle backlight on/off. */
        if (current_mode == APP_MODE_COUNTDOWN || current_mode == APP_MODE_POMODORO) {
            display_timer_toggle();
        } else {
            const char *j = backlight_on
                ? "{\"backlight_onoff\":\"OFF\"}"
                : "{\"backlight_onoff\":\"ON\"}";
            config_set_json(j, strlen(j));
        }
        break;
    }
    }

    /* Play button-click sound (fires after every touch event).
     * audio_play_file() returns immediately if the path is empty so no
     * sound plays until the user configures a click file. */
    if (button_sound && click_file[0] != '\0')
        audio_play_file(click_file);
}

/* ── LittleFS mount ────────────────────────────────────────────────── */
static void init_littlefs(void)
{
    /* base_path is kept as "/spiffs" so all existing path strings in the
     * firmware (config.json, audio/, images/) are unchanged — only the
     * partition label and VFS API differ from the old SPIFFS setup. */
    esp_vfs_littlefs_conf_t conf = {
        .base_path       = "/spiffs",
        .partition_label = "littlefs",
        .dont_mount      = false,
        .grow_on_mount   = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info("littlefs", &total, &used);
    ESP_LOGI(TAG, "LittleFS: total=%u  used=%u", (unsigned)total, (unsigned)used);

    /* mklittlefs drops empty directories, so data/images/album/ only lands
     * on the partition when the user uploads their first image via the web
     * UI's mkdir call — or on a fresh flash if the .keep placeholder is
     * present.  Create the directory here so album mode works immediately
     * on devices whose partition was flashed before the .keep was added. */
    if (mkdir("/spiffs/images/album", 0755) == 0) {
        ESP_LOGI(TAG, "Created /spiffs/images/album");
    } else if (errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir /spiffs/images/album: %s", strerror(errno));
    }
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
    /* Restart once after any hard reset so WiFi PHY initialises cleanly.
     * (See s_warm_boot comment above.) */
    if (esp_reset_reason() == ESP_RST_EXT && s_warm_boot != WARM_BOOT_MAGIC) {
        s_warm_boot = WARM_BOOT_MAGIC;
        esp_restart();   /* causes ESP_RST_SW on next boot → skips this block */
    }
    s_warm_boot = 0;     /* clear so the next hard-reset also triggers a restart */

    ESP_LOGI(TAG, "╔════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  Nextube-Remaster Open-Source Firmware v%-7s ║", FW_VERSION_STR);
    ESP_LOGI(TAG, "║  https://github.com/MrToast99/Nextube-Remaster ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════╝");

    /* Allow power rails and SPI peripherals to fully settle. */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Core initialisations */
    init_nvs();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    init_littlefs();

    /* Load configuration from /spiffs/config.json (or defaults) — /spiffs is the LittleFS mount point */
    config_mgr_init();

    /* Read the small boot-time scalars under the config lock. */
    uint8_t boot_volume;
    bool    boot_audio_enabled;
    bool    boot_mic_enabled;
    bool    boot_mic_cal_saved;
    bool    boot_weather_enabled;
    bool    boot_youtube_enabled;
    uint8_t boot_invert_mask;
    uint8_t boot_init_profile[6];
    uint8_t boot_vcom[6];
    float   boot_gamma[6];
    int8_t  boot_col_offset[6];
    int8_t  boot_row_offset[6];
    uint8_t boot_tube_brightness[6];
    config_lock();
    const nextube_config_t *cfg_boot = config_get();
    boot_volume          = cfg_boot->volume;
    boot_audio_enabled   = cfg_boot->audio_enabled;
    boot_mic_enabled     = cfg_boot->mic_enabled;
    boot_mic_cal_saved   = cfg_boot->mic_calibration_saved;
    boot_weather_enabled = cfg_boot->weather_enabled;
    boot_youtube_enabled = cfg_boot->youtube_enabled;
    boot_invert_mask     = cfg_boot->lcd_invert_mask;
    memcpy(boot_init_profile,    cfg_boot->lcd_init_profile,    sizeof(boot_init_profile));
    memcpy(boot_vcom,            cfg_boot->lcd_vcom,            sizeof(boot_vcom));
    memcpy(boot_gamma,           cfg_boot->lcd_gamma,            sizeof(boot_gamma));
    memcpy(boot_col_offset,      cfg_boot->lcd_col_offset,      sizeof(boot_col_offset));
    memcpy(boot_row_offset,      cfg_boot->lcd_row_offset,      sizeof(boot_row_offset));
    memcpy(boot_tube_brightness, cfg_boot->lcd_tube_brightness, sizeof(boot_tube_brightness));
    config_unlock();

    /* Hardware drivers */
    display_init();
    display_apply_invert_mask(boot_invert_mask);                  /* INVON for replacement panels    */
    display_apply_tube_vcom(boot_vcom);                           /* per-tube VMCTR1 VCOM            */
    display_apply_tube_gamma(boot_gamma);                         /* per-tube software gamma LUT     */
    display_apply_init_profiles(boot_init_profile);               /* gamma per-tube profile          */
    display_apply_tube_offsets(boot_col_offset, boot_row_offset); /* ST7735S window alignment        */
    display_apply_tube_brightness(boot_tube_brightness);          /* per-tube brightness scale       */
    display_task_start();          /* launch 5 Hz FreeRTOS display task */

    audio_init();
    audio_set_volume(boot_volume);          /* restore saved volume level   */
    audio_set_enabled(boot_audio_enabled);  /* tear down DAC if disabled    */

    /* Microphone: only initialise the ADC and start the sampling task when
     * mic is enabled.  The task self-gates anyway (checks config each frame),
     * but skipping init entirely avoids touching ADC1 when the user has
     * permanently disabled the mic (e.g. no mic fitted). */
    if (boot_mic_enabled) {
        mic_init();
        mic_task_start();
        /* Restore a user-captured noise baseline so Phase 1 is skipped on boot.
         * mic_apply_calibration() only does a memcpy internally, so passing the
         * locked config pointer is safe — no blocking or recursive lock needed. */
        if (boot_mic_cal_saved) {
            config_lock();
            mic_apply_calibration(config_get()->mic_noise_floor);
            config_unlock();
        }
    }

    leds_init();
    leds_task_start();
    touch_input_init();
    touch_input_register_callback(on_touch);
    pcf8563_init();
    sht30_init();          /* probe optional sensor; safe no-op if absent */

    /* Networking – start AP+STA, then web server */
    wifi_manager_start();
    web_server_start();

    /* Background services — gated by their respective config flags so users
     * can disable features they don't use, freeing each task's stack and
     * stopping the periodic HTTPS polling.  Boot-time only — toggling the
     * UI checkboxes requires a reboot to take effect. */
    ntp_time_start();
    if (boot_weather_enabled) {
        weather_start();
    } else {
        ESP_LOGI(TAG, "Weather disabled in config — task not started");
    }
    if (boot_youtube_enabled) {
        youtube_bili_start();
    } else {
        ESP_LOGI(TAG, "YouTube/Bilibili disabled in config — task not started");
    }
    sht30_task_start();    /* no-op task if sensor absent */

    /* Mark this OTA image as valid so the bootloader does not roll back to
     * the previous firmware on the next reboot.  CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
     * leaves a freshly-flashed image in ESP_OTA_IMG_PENDING_VERIFY state; if the
     * app never calls this, the bootloader treats the next reboot as a failed
     * boot and silently reverts to the previous slot.
     * Calling here — after all hardware and services initialised without panic —
     * is the correct point to declare the image healthy. */
    esp_ota_mark_app_valid_cancel_rollback();

    /* Low-priority background heap monitor — fires every 5 minutes.
     * 4 KB stack: ESP_LOGI through the log-ring vprintf hook
     * (web_server.c::log_vprintf_hook) uses a 160-byte format buffer on
     * top of vprintf/vsnprintf's own scratch, plus the captured va_list
     * copy.  2 KB overflowed reliably in field testing. */
    xTaskCreatePinnedToCore(heap_telemetry_task, "heap_tel",
                            4096, NULL, 1, NULL, 0);

    ESP_LOGI(TAG, "All tasks launched – heap free: %u bytes",
             (unsigned)esp_get_free_heap_size());
}
