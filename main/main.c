/**
 * @file main.c
 * @brief Nextube open-source firmware – main entry
 *
 * Task architecture (mirrors original firmware's FreeRTOS design — kept
 * here as a still-useful map of the original 9, not a changelog of every
 * addition since):
 *   TaskDisplay      – renders clock / modes on 6× ST7735 LCDs
 *   TaskWifiServer   – captive-portal AP + STA, embedded web UI
 *   TaskNtp          – NTP time synchronisation
 *   TaskWeather      – OpenWeatherMap polling
 *   TaskYoutubeAndBili – subscriber/follower counts; grew past its original
 *                        name to also cover Instagram, TikTok, and Mastodon
 *   TaskIIC          – RTC + SHT30 I²C sensor polling
 *   TaskLed          – WS2812 LED effects
 *   TaskAudio        – WAV / tone playback via DAC
 *   TaskButton       – Capacitive touch input
 * Added since: update_check (GitHub release poll), ha_mqtt (Home Assistant),
 * wled_sync (WLED strip mirroring), mic/spectrum (microphone band capture),
 * heap_telemetry (periodic heap/stack monitor) — see each one's own
 * xTaskCreate*()/_start() call site below for what it does.
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
#include "cJSON.h"       /* cJSON_InitHooks — see install_cjson_psram_hooks() */

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
 * Logs internal-RAM and PSRAM free-size + largest-block every 5 minutes,
 * also exposed via /api/status for the System tab's chart. largest-block
 * is the real fragmentation indicator: 200KB free / 180KB largest is
 * fine; 200KB free / 8KB largest is in trouble despite the same aggregate
 * free. Uses heap_caps_get_free_size(CAP_INTERNAL/CAP_SPIRAM) directly
 * rather than esp_get_free_heap_size(), which combines both caps and is
 * misleading on this board's internal-SRAM/PSRAM split. internal largest
 * uses CAP_INTERNAL|CAP_8BIT, not CAP_INTERNAL alone, since CAP_INTERNAL
 * alone also counts reclaimed IRAM (not byte-addressable, can't hold a
 * real allocation).
 *
 * httpd sockets: how many of web_server's max_open_sockets slots are in
 * use — makes a slow climb toward the cap visible over days of uptime
 * instead of only seeing the aftermath once lru_purge kicks in.
 *
 * Per-task stack dump: uxTaskGetStackHighWaterMark() reports the closest
 * any task has come to overflowing its stack; this logs bytes still
 * unused per task, so a small number is a stack-size candidate to grow
 * and a large one a candidate to shrink. Gated behind the hidden debug
 * panel's "Per-task stack log" checkbox — one ESP_LOGI per task every 5
 * minutes is too much to leave on by default. */
static void log_task_stacks(void)
{
    UBaseType_t n = uxTaskGetNumberOfTasks() + 2; /* pad: a task can be created between the count and the snapshot */
    TaskStatus_t *tasks = pvPortMalloc(n * sizeof(TaskStatus_t));
    if (!tasks) {
        ESP_LOGW("stack", "task snapshot skipped - alloc failed (%u tasks)", (unsigned)n);
        return;
    }
    n = uxTaskGetSystemState(tasks, n, NULL);
    for (UBaseType_t i = 0; i < n; i++) {
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
        /* tskNO_AFFINITY (INT_MAX) means "not pinned to a core", not a real
         * core ID - xTaskCreate() (vs. ...PinnedToCore()) tasks report this. */
        char core_buf[12] = "any"; /* sized for %d worst-case, not just 0/1 - avoids -Wformat-truncation */
        if (tasks[i].xCoreID != tskNO_AFFINITY)
            snprintf(core_buf, sizeof(core_buf), "%d", (int)tasks[i].xCoreID);
        const char *core_str = core_buf;
#else
        const char *core_str = "?";
#endif
        ESP_LOGI("stack", "%-16s core=%-3s free=%u B",
                 tasks[i].pcTaskName, core_str,
                 (unsigned)(tasks[i].usStackHighWaterMark * sizeof(StackType_t)));
    }
    vPortFree(tasks);
}

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
                 "uptime=%llum  internal: free=%u largest=%u  psram: free=%u largest=%u  (lifetime min total: %u)  httpd sockets: %d",
                 (unsigned long long)(esp_timer_get_time() / 60000000LL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned)esp_get_minimum_free_heap_size(),
                 web_server_socket_count());
        if (web_server_debug_stacklog_enabled())
            log_task_stacks();
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
/* Runs well after the WiFi AP is broadcasting and any auto-connecting
 * client's WPA2 handshake has completed — a fixed 8s delay, enough to
 * clear that window. Deferred via audio_defer_timer_cb() (a one-shot
 * esp_timer) rather than sleeping inside an already-created task: an
 * xTaskCreate() allocates its full stack synchronously at creation time,
 * so sleeping inside the task would leave that stack allocated for the
 * whole 8s wait — competing with WiFi/WPA2 setup allocations for exactly
 * the memory this delay exists to protect. Deferring the task creation
 * itself means the 8s wait costs no new allocation at all (it runs on the
 * esp_timer service task's own stack), and this task's own stack is only
 * allocated once the sensitive window has already passed.
 *
 * Mic setup does NOT defer this way — see mic_hw_init()/mic_init() in
 * app_main(), which run at boot time instead, since mic_task_start()'s
 * stack allocation needs to happen before the same late-boot WiFi/MQTT
 * memory pressure this delay is protecting audio from. audio_init() keeps
 * a persistent playback task out of this path on purpose — see the
 * comment on audio_play_task in audio.c for why a persistent task here
 * would starve sht30/wled_sync/MQTT of internal RAM instead. */
/* 3072: audio_init() (2 semaphore creates, a flag, 2 log lines) and
 * audio_set_volume() (one bounds-checked assignment) are both trivially
 * shallow, so this task needs very little — but not cut further, since a
 * similarly shallow, logging-heavy task elsewhere in this codebase has
 * overflowed a 2KB stack in the field, and this task does comparable
 * logging plus a config_lock()/unlock() cycle on top. High-water-mark
 * logged below — tighten with that real number once it's in hand. */
#define AUDIO_DEFER_STACK_SIZE 3072

static void audio_deferred_start(void *arg)
{
    config_lock();
    const nextube_config_t *cfg = config_get();
    uint8_t vol      = cfg->volume;
    bool    audio_en = cfg->audio_enabled;
    config_unlock();

    /* audio_init() handles the enabled state directly:
     *   enabled=true  → DAC brought up per clip by the playback task.
     *   enabled=false → GPIO25 left isolated, DAC never started.
     * audio_set_enabled() is only called later from the web server when the
     * user toggles the setting at runtime — no need to call it here. */
    audio_init(audio_en);
    audio_set_volume(vol);

    ESP_LOGI("main", "Audio started (deferred)");
    ESP_LOGI("main", "audio_defer stack high-water mark: %u B unused (of %u allocated)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)AUDIO_DEFER_STACK_SIZE);
    vTaskDelete(NULL);
}

/* esp_timer one-shot callback — the actual 8 s wait now happens here, on
 * the esp_timer service task's own pre-existing stack, so no NEW allocation
 * exists at all during the WPA2 window this exists to clear. See
 * audio_deferred_start()'s doc comment above for the full story. */
static void audio_defer_timer_cb(void *arg)
{
    if (xTaskCreate(audio_deferred_start, "audio_defer", AUDIO_DEFER_STACK_SIZE, NULL, 4, NULL) != pdPASS)
        ESP_LOGE(TAG, "audio_defer task creation failed — audio will not start");
}

/* ── cJSON → PSRAM ────────────────────────────────────────────────────
 * cJSON's default allocator is plain malloc()/free(), which — like every
 * other unmarked allocation in this firmware — is subject to
 * SPIRAM_MALLOC_ALWAYSINTERNAL=255 (see sdkconfig.defaults): each tree
 * node and duplicated string value is well under 255 B, so by default
 * every one of them lands in internal SRAM. That threshold exists for
 * DMA-sensitive buffers (WPA2 supplicant, etc.) — cJSON nodes are never
 * touched by DMA or an ISR, so there's no reason for them to compete for
 * that same limited, fragmentation-prone region. api_status() alone
 * rebuilds and tears down a ~20-30-node tree every 5 s for as long as a
 * browser tab is open on the dashboard; weather.c parses/frees a much
 * larger one every 10 min. Routing cJSON's own allocations to PSRAM
 * instead removes that churn from internal SRAM entirely, at the cost of
 * PSRAM's slightly higher access latency — irrelevant here, nothing
 * touching these buffers is timing-critical. Falls back to internal RAM
 * if PSRAM is ever exhausted, rather than failing the allocation outright.
 * Installed as the first thing in app_main(), before any subsystem that
 * might touch cJSON (config_mgr's config.json load included). */
static void *cjson_psram_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(sz);
}

static void cjson_psram_free(void *ptr)
{
    free(ptr);   /* heap_caps_malloc() and malloc() share one heap registry —
                   * free() correctly routes to whichever region ptr is in. */
}

static void install_cjson_psram_hooks(void)
{
    cJSON_Hooks hooks = { .malloc_fn = cjson_psram_malloc, .free_fn = cjson_psram_free };
    cJSON_InitHooks(&hooks);
}

/* ── Application entry ─────────────────────────────────────────────── */
void app_main(void)
{
    install_cjson_psram_hooks();

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
     * GPIO25 (DAC_CHAN_0 → LTK8002D amplifier). rtc_gpio_isolate() (RTC mux,
     * input/output buffers off, no pulls — pad fully disconnected from the
     * digital domain) is the quietest idle state measured: driving it LOW
     * references the amp's AC-coupled input to digital ground through the
     * pull-down FET, so die current spikes appear as ground bounce (static
     * floor, hiss, beeps); Hi-Z input is noisier still for broadband
     * pickup. If audio is enabled, dac_restart() in the deferred task
     * reconfigures the pad for DAC use per clip; dac_teardown() re-isolates
     * it after.
     *
     * GPIO26 (DAC_CHAN_1) is dual-use on this PCB — also PIN_LCD2_CS. We
     * use it only as SPI CS; the DAC driver must not claim it, or
     * dac_oneshot on DAC_CHAN_1 would conflict with the SPI driver
     * asserting CS on the same pin. */
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
    bool    boot_update_check_enabled;
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
    boot_update_check_enabled = cfg_boot->update_check_enabled;
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
     * internal-cleanup bug with no way for us to catch it. This only
     * allocates the hardware (no-op if mic is disabled in config). */
    mic_hw_init();

    /* Finish mic setup and start mic_task HERE too, for the exact same
     * reason as mic_hw_init() above — mic_task_start()'s own 8KB stack
     * allocation hits the identical late-boot WiFi/MQTT memory-pressure
     * window mic_hw_init() was moved here to dodge, so moving the hardware
     * alloc alone only fixed half the problem.
     *
     * Safe to do this early: creating mic_task doesn't by itself trigger
     * the AP-PIN-phase SPI0 bus pressure the old deferred code's
     * wifi_manager_ap_pin_visible() wait guarded against — that risk is
     * specifically about mic_task's ACTIVE capture, and mic_task only
     * starts actively capturing once Spectrum mode is genuinely requested,
     * which requires the device to already be past initial AP-PIN setup.
     *
     * This also drops the old "re-read config in case the user changed it
     * via the web UI during setup" step — at this point in boot neither
     * WiFi nor the web server exist yet, so nothing could have changed it.
     * mic_init() itself already returns false harmlessly if mic_hw_init()
     * found the mic disabled, so no outer enabled-check is needed here
     * either. */
    if (mic_init()) {
        mic_task_start();
        config_lock();
        bool  cal_saved = config_get()->mic_calibration_saved;
        float noise_floor[CFG_MIC_BAND_COUNT];
        if (cal_saved) memcpy(noise_floor, config_get()->mic_noise_floor, sizeof(noise_floor));
        config_unlock();
        if (cal_saved) mic_apply_calibration(noise_floor);
    }

    /* Low-priority background heap monitor — fires every 5 minutes.
     * 4 KB stack: ESP_LOGI through the log-ring vprintf hook uses a
     * 160-byte format buffer on top of vprintf/vsnprintf's own scratch,
     * plus the captured va_list copy — 2KB overflowed reliably in field
     * testing.
     *
     * Created here — right after display/mic, before WiFi/network/MQTT —
     * not at the very end of app_main(): a task created last in the boot
     * sequence is the most exposed to whatever fragmentation everything
     * else has already caused, and a monitoring task should be one of the
     * most reliably-created things here, not the least, since it's the
     * thing meant to tell us when something else is starving. */
    if (xTaskCreatePinnedToCore(heap_telemetry_task, "heap_tel",
                                4096, NULL, 1, NULL, 0) != pdPASS)
        ESP_LOGE(TAG, "heap_telemetry_task creation failed");

    /* Audio — deferred start via a one-shot timer (see audio_defer_timer_cb()
     * / audio_deferred_start() above): the task itself isn't created until
     * 8 s from now, clear of the WPA2 handshake window — no stack allocation
     * happens at all until then. Mic no longer defers through here — see
     * mic_hw_init()/mic_init() above. */
    {
        static esp_timer_handle_t audio_defer_timer;
        const esp_timer_create_args_t a = { .callback = audio_defer_timer_cb, .name = "audio_defer_t" };
        if (esp_timer_create(&a, &audio_defer_timer) == ESP_OK)
            esp_timer_start_once(audio_defer_timer, 8000 * 1000ULL);   /* 8 s in µs */
        else
            ESP_LOGE(TAG, "audio_defer_timer create failed — audio will not start");
    }

    leds_init();
    leds_task_start();
    touch_input_init();
    touch_input_register_callback(on_touch);
    /* Hold LEFT+RIGHT touch pads together for 15 s to summon the WiFi setup AP
     * on demand (replaces the old automatic 90 s fallback timer). */
    touch_input_register_combo_callback(wifi_manager_force_ap);
    sht30_init();               /* probe optional sensor; safe no-op if absent */
    sht30_set_offset(boot_sht30_temp_offset); /* apply saved calibration offset */

    wifi_manager_start();

    /* web_server_start() runs right after WiFi starts, same as stock — this
     * is what makes the web UI reachable in ~8s instead of waiting on
     * weather/update_check/etc. below. Its stock_files_check() scan is its
     * own timer-deferred call inside web_server_start(), so it doesn't
     * contend with weather's/update_check's TLS handshakes for CPU even
     * though this call happens before them; nothing below blocks waiting
     * for the web server. */
    web_server_start();

    /* Background services */
    ntp_time_start();
    /* Weather and social counters register with the shared periodic_net_poll
     * task unconditionally — not gated behind boot_weather_enabled /
     * boot_social_enabled like update_check below still is. Their own tick
     * functions already re-check weather_enabled/social_enabled live from
     * config every cycle and no-op when disabled, so registering
     * unconditionally is what makes BOTH directions of the toggle live: a
     * tick never registered here (config off at boot) would have nothing
     * to re-enable later if the user turns it back on. */
    weather_start();
    /* Update check — periodic GitHub release poll that drives the tube-6
     * update indicator and the Home Assistant "Nextube Update Available"
     * topic autonomously (no browser tab required).  update_check_enabled
     * has no exposed WebUI toggle (only the tube-6 indicator does), so this
     * gate is effectively always true in practice — kept as a real gate
     * rather than also going unconditional since a config-file edit can
     * still set it false and that should still fully skip registration. */
    if (boot_update_check_enabled) {
        update_check_start();
    } else {
        ESP_LOGI(TAG, "Update check disabled in config — task not started");
    }
    subscribers_start();
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

    ESP_LOGI(TAG, "All tasks launched – heap free: %u bytes",
             (unsigned)esp_get_free_heap_size());
}
