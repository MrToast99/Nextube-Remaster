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
#include "nvs.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_littlefs.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"   /* esp_timer_get_time — uptime in heap telemetry */

#include "driver/gpio.h"
#include "driver/rtc_io.h"   /* rtc_gpio_isolate — GPIO25 idle state */
/* dac_oneshot.h intentionally not included — GPIO26 (DAC_CHAN_1) is audio
 * hardware on this PCB and must not be claimed by the application. */
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
#include "update_check.h"
#include "subscribers.h"
#include "microphone.h"
#include "fw_version.h"
#include "ha_mqtt.h"
#include "wled_sync.h"

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
        /* Uptime in minutes from the monotonic µs timer.  With log timestamps
         * on wall-clock time (CONFIG_LOG_TIMESTAMP_SOURCE_SYSTEM) this is the
         * one periodic line that still anchors "how long has it been up" —
         * wall-clock stamps jump on NTP corrections; this counter never does. */
        ESP_LOGI("heap",
                 "uptime=%llum  internal: free=%u largest=%u  psram: free=%u largest=%u  (lifetime min total: %u)",
                 (unsigned long long)(esp_timer_get_time() / 60000000LL),
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
    /* Strip social mode bits when the social master switch is off so that
     * touch cycling never lands on YouTube / Instagram / TikTok / Mastodon. */
    if (!cfg->social_enabled)
        enabled_modes &= ~((1u << APP_MODE_YOUTUBE)   | (1u << APP_MODE_INSTAGRAM) |
                           (1u << APP_MODE_TIKTOK)    | (1u << APP_MODE_MASTODON));
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
        display_wake();
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
        display_wake();
        break;
    }
    case TOUCH_MIDDLE: {
        /* Toggle backlight on/off. */
        const char *j = backlight_on
            ? "{\"backlight_onoff\":\"OFF\"}"
            : "{\"backlight_onoff\":\"ON\"}";
        config_set_json(j, strlen(j));
        break;
    }
    }

    /* Play button-click sound (fires after every touch event).
     * audio_play_file() returns immediately if the path is empty so no
     * sound plays until the user configures a click file. */
    if (button_sound && click_file[0] != '\0')
        audio_play_file(click_file);

    /* Optional MQTT button events (HA device triggers) — no-ops unless the
     * "button events" publishing group is enabled and MQTT is connected. */
    ha_mqtt_publish_button(pad == TOUCH_LEFT   ? "left"
                         : pad == TOUCH_MIDDLE ? "middle" : "right");
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

/* ── Deferred audio initialisation ───────────────────────────────────── */
/* Started as a low-priority task from app_main so that audio comes up well
 * after the WiFi AP is broadcasting and any auto-connecting client's WPA2
 * handshake has completed — a fixed 8 s delay, enough to clear that window.
 *
 * Mic setup no longer runs from here — see mic_hw_init()/mic_init() in
 * app_main(), moved to boot time for the same reason audio stays deferred
 * would have broken it: mic_task_start()'s internal-RAM stack allocation
 * hit the same late-boot WiFi/MQTT memory pressure this function's own
 * 8 s/AP-PIN wait was exposing it to. */
static void audio_deferred_start(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(8000));   /* 8 s — safely past the WPA2 window */

    config_lock();
    const nextube_config_t *cfg = config_get();
    uint8_t vol      = cfg->volume;
    bool    audio_en = cfg->audio_enabled;
    config_unlock();

    /* audio_init() handles the enabled state directly:
     *   enabled=true  → DAC brought up, APLL locked, DMA ring running.
     *   enabled=false → GPIO25 driven LOW (amp-input clamp), DAC not started.
     * audio_set_enabled() is only called later from the web server when the
     * user toggles the setting at runtime — no need to call it here. */
    audio_init(audio_en);
    audio_set_volume(vol);

    ESP_LOGI("main", "Audio started (deferred)");
    ESP_LOGD("main", "audio_defer stack HWM: %u words",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
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

    ESP_LOGI(TAG, "╔═════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  Nextube-Remaster Open-Source Firmware v%-7s ║", FW_VERSION_STR);
    ESP_LOGI(TAG, "║  https://github.com/MrToast99/Nextube-Remaster  ║");
    ESP_LOGI(TAG, "╚═════════════════════════════════════════════════════╝");

    /* ── Isolate the DAC output pad at idle ──────────────────────────────
     * GPIO25 (DAC_CHAN_0 → LTK8002D amplifier).  Idle state matters enormously:
     *
     *   • Driven LOW (previous approach): references the amp's AC-coupled
     *     input to the ESP32's DIGITAL GROUND through the pin's pull-down
     *     FET.  Every current spike on the die then appears at the amp input
     *     as ground bounce — measured as a constant static floor (1 kHz tick
     *     wake-ups), hiss during flash reads, beeps during panel init, and a
     *     1 Hz tick from the per-second redraw.
     *
     *   • Digital INPUT (Hi-Z): input buffer + GPIO-matrix connection stay
     *     alive; measured noisier than LOW for broadband pickup.
     *
     *   • rtc_gpio_isolate(): RTC mux, input/output buffers off, no pulls —
     *     pad fully disconnected from the digital domain.  This is the exact
     *     state IDF 3.3.5's dac_output_disable() left the pad in, i.e. the
     *     stock firmware's idle.  Measured near-silent: no static floor, no
     *     activity hiss, no boot pop (no DC step into the coupling cap).
     *
     * If audio is enabled, dac_restart() in the deferred task reconfigures
     * the pad for DAC use per clip; dac_teardown() re-isolates it after.
     *
     * GPIO26 (DAC_CHAN_1) is dual-use on this PCB: it is also PIN_LCD2_CS.  The stock
     * firmware time-shares it between LCD CS and audio DAC.  We use it only as SPI CS;
     * the DAC driver must not claim it (dac_oneshot on DAC_CHAN_1 would conflict with
     * the SPI driver asserting CS on the same pin).  Leave it to the SPI driver only. */
    rtc_gpio_isolate(PIN_AUDIO_DAC);

    /* Allow power rails and SPI peripherals to fully settle. */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Core initialisations */
    init_nvs();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    init_littlefs();

    /* Load configuration from /spiffs/config.json (or defaults) — /spiffs is the LittleFS mount point */
    config_mgr_init();

    /* Read the small boot-time scalars under the config lock.
     * audio_enabled / volume are NOT read here — the deferred audio task
     * reads them fresh from config at start time (see audio_deferred_start).
     * mic_* fields are read a little further down, by mic_hw_init()/
     * mic_init() themselves (each takes the lock internally). */
    bool    boot_weather_enabled;
    bool    boot_update_check_enabled;
    bool    boot_social_enabled;
    bool    boot_mqtt_enabled;
    bool    boot_wled_sync_enabled;
    float   boot_sht30_temp_offset;
    uint8_t boot_invert_mask;
    uint8_t boot_init_profile[6];
    uint8_t boot_vcom[6];
    float   boot_gamma[6];
    int8_t  boot_col_offset[6];
    int8_t  boot_row_offset[6];
    uint8_t boot_tube_brightness[6];
    config_lock();
    const nextube_config_t *cfg_boot = config_get();
    boot_weather_enabled    = cfg_boot->weather_enabled;
    boot_update_check_enabled = cfg_boot->update_check_enabled;
    boot_social_enabled     = cfg_boot->social_enabled;
    boot_mqtt_enabled       = cfg_boot->mqtt_enabled && cfg_boot->mqtt_broker[0] != '\0';
    boot_wled_sync_enabled  = cfg_boot->wled_sync_enabled;
    boot_sht30_temp_offset  = cfg_boot->sht30_temp_offset;
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
    pcf8563_init();        /* must come before ntp_seed_rtc_early — initialises the I²C driver */
    ntp_seed_rtc_early();  /* seed system clock from RTC before the display task starts so the
                            * first render sees a valid wall-clock time instead of a stopwatch */
    display_task_start();          /* launch 5 Hz FreeRTOS display task */

    /* Allocate the mic's ADC hardware NOW, before WiFi/MQTT/audio get a
     * chance to claim MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA memory —
     * adc_continuous_new_handle() needs ~10 KB from that specific, small,
     * contended pool, and a failure there aborts the device via an ESP-IDF
     * internal-cleanup bug with no way for us to catch it (confirmed by
     * reading esp_adc/adc_continuous.c directly, and by a live repro
     * 2026-08-14: calling this after WiFi/MQTT connect left only ~2 KB free
     * with no block bigger than 1.4 KB, and it aborted immediately). This
     * only allocates the hardware (no-op if mic is disabled in config). */
    mic_hw_init();

    /* Finish mic setup and start mic_task HERE too, for the exact same
     * reason as mic_hw_init() above — confirmed live: mic_task_start()'s
     * xTaskCreatePinnedToCore() (an 8 KB internal-RAM stack) failed with
     * "mic_task creation failed" when left in the deferred path below,
     * hitting the identical late-boot WiFi/MQTT memory-pressure window
     * mic_hw_init() was moved here to dodge — moving the hardware alloc
     * without also moving the task that uses it only fixed half the
     * problem.
     *
     * Safe to do this early: creating mic_task does not by itself trigger
     * the AP-PIN-phase / PSRAM-cache-errata risk the old deferred code
     * guarded against with its wifi_manager_ap_pin_visible() wait (that risk
     * was always specifically about mic_task's ACTIVE capture — SPI0 bus
     * pressure from adc_continuous colliding with the display task's rapid
     * SPIFFS reads for the AP-PIN JPEGs — never about audio, which has no
     * such wait of its own beyond the unrelated WPA2-window delay below).
     * mic_task only starts actively capturing once Spectrum mode is
     * genuinely requested, which requires the device to already be past
     * initial AP-PIN setup — so the same real-world timing that used to be
     * enforced by an explicit wait is still true here, just implicitly.
     *
     * This also drops the old "re-read config in case the user changed it
     * via the web UI during setup" step mic_init() used to need — at this
     * point in boot neither WiFi nor the web server exist yet, so nothing
     * could have changed it. mic_init() itself already returns false
     * harmlessly if mic_hw_init() found the mic disabled, so no outer
     * enabled-check is needed here either. */
    if (mic_init()) {
        mic_task_start();
        config_lock();
        bool  cal_saved = config_get()->mic_calibration_saved;
        float noise_floor[CFG_MIC_BAND_COUNT];
        if (cal_saved) memcpy(noise_floor, config_get()->mic_noise_floor, sizeof(noise_floor));
        config_unlock();
        if (cal_saved) mic_apply_calibration(noise_floor);
    }

    /* Audio — deferred start (see audio_deferred_start): starts after a
     * fixed 8 s delay, clear of the WPA2 handshake window. Mic no longer
     * defers through here — see mic_hw_init()/mic_init() above.
     *
     * Stack: sized for audio_init()'s call chain alone now that mic_init()'s
     * no longer shares this task; kept at the same 8 KB the combined chain
     * needed rather than guessing a smaller number without measuring
     * audio_init() in isolation. */
    if (xTaskCreate(audio_deferred_start, "audio_defer", 8192, NULL, 4, NULL) != pdPASS)
        ESP_LOGE(TAG, "audio_defer task creation failed — audio will not start");

    leds_init();
    leds_task_start();
    touch_input_init();
    touch_input_register_callback(on_touch);
    /* Hold LEFT+RIGHT touch pads together for 15 s to summon the WiFi setup AP
     * on demand (replaces the old automatic 90 s fallback timer). */
    touch_input_register_combo_callback(wifi_manager_force_ap);
    sht30_init();               /* probe optional sensor; safe no-op if absent */
    sht30_set_offset(boot_sht30_temp_offset); /* apply saved calibration offset */

    /* Networking – start AP+STA, then web server */
    wifi_manager_start();
    web_server_start();

    /* Background services — gated by their respective config flags so users
     * can disable features they don't use, freeing each task's stack and
     * stopping the periodic HTTPS polling.  Weather requires a reboot to
     * enable; the social counter task reads config dynamically each cycle. */
    ntp_time_start();
    if (boot_weather_enabled) {
        weather_start();
    } else {
        ESP_LOGI(TAG, "Weather disabled in config — task not started");
    }
    /* Update check — periodic GitHub release poll that drives the tube-6
     * update indicator and the Home Assistant "Nextube Update Available"
     * topic autonomously (no browser tab required). Same gate pattern as
     * weather_enabled. */
    if (boot_update_check_enabled) {
        update_check_start();
    } else {
        ESP_LOGI(TAG, "Update check disabled in config — task not started");
    }
    /* Social counter task — only started when the user has opted in via the
     * "Enable Social Media Counters" toggle.  Requires a reboot to take effect
     * (same gate pattern as weather_enabled).  When disabled, no HTTPS polling
     * occurs and no stack is allocated for the task. */
    if (boot_social_enabled) {
        subscribers_start();
    } else {
        ESP_LOGI(TAG, "Social counters disabled in config — task not started");
    }
    /* Home Assistant MQTT — only started when enabled and a broker is configured. */
    if (boot_mqtt_enabled) {
        ha_mqtt_start();
    } else {
        ESP_LOGI(TAG, "MQTT disabled or no broker configured — task not started");
    }
    /* WLED Sync — UDP listener that mirrors WLED-controlled strips to accent LEDs.
     * Started when wled_sync_enabled is true AND backlight_mode is BL_MODE_WLED
     * (or the user may start it standalone so LEDs are ready when mode switches). */
    if (boot_wled_sync_enabled) {
        wled_sync_start();
    } else {
        ESP_LOGI(TAG, "WLED sync disabled — task not started");
    }
    sht30_task_start();    /* no-op task if sensor absent */

    /* Detect OTA rollback: if the inactive OTA slot is in ABORTED state it means
     * the previous boot attempt failed (the new firmware crashed or watchdogged
     * before reaching esp_ota_mark_app_valid_cancel_rollback) and the bootloader
     * automatically reverted to this slot.  Log it prominently so it is visible
     * in the serial monitor and the in-RAM device log; the web UI reads the same
     * partition state via /api/status and shows a banner to the user. */
    {
        const esp_partition_t *running  = esp_ota_get_running_partition();
        const esp_partition_t *inactive = esp_ota_get_next_update_partition(NULL);
        if (inactive && inactive != running) {
            esp_ota_img_states_t st;
            if (esp_ota_get_state_partition(inactive, &st) == ESP_OK &&
                    st == ESP_OTA_IMG_ABORTED) {
                esp_app_desc_t desc;
                if (esp_ota_get_partition_description(inactive, &desc) == ESP_OK)
                    ESP_LOGW(TAG, "OTA ROLLBACK — firmware v%s failed to start; "
                             "reverted to v%s", desc.version, FW_VERSION_STR);
                else
                    ESP_LOGW(TAG, "OTA ROLLBACK — failed firmware version unknown; "
                             "reverted to v%s", FW_VERSION_STR);
                /* Persist the rollback event in NVS so it survives a power-cycle
                 * and can be surfaced in diagnostics even if no serial monitor is
                 * attached.  Cleared when the user acknowledges via the web UI. */
                nvs_handle_t _nh;
                if (nvs_open("nextube_diag", NVS_READWRITE, &_nh) == ESP_OK) {
                    nvs_set_u8(_nh, "ota_rollback", 1);
                    nvs_commit(_nh);
                    nvs_close(_nh);
                }
            }
        }
    }

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
