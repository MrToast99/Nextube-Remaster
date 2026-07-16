/**
 * @file config_mgr.h
 * @brief Persistent JSON configuration – stored in /spiffs/config.json
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/** Number of frequency bands stored in the mic noise baseline.
 *  Must equal MIC_BAND_COUNT in components/microphone/include/microphone.h.
 *  Kept as a local constant to avoid a circular header dependency between
 *  config_mgr and microphone (microphone already REQUIRES config_mgr).
 *  A _Static_assert in main.c enforces that the two constants stay in sync. */
#define CFG_MIC_BAND_COUNT  24

#ifdef __cplusplus
extern "C" {
#endif

/* ── App mode identifiers ──────────────────────────────────────────── */
typedef enum {
    APP_MODE_CLOCK      = 0,
    /* bit 1 is unused — was APP_MODE_COUNTDOWN; bit 2 is unused — was
     * APP_MODE_SCOREBOARD; bit 3 is unused — was APP_MODE_POMODORO. All
     * three kept as gaps so that saved enabled_modes bitmasks from older
     * firmware are not silently misread. */
    APP_MODE_DATE       = 4,
    APP_MODE_ALBUM      = 5,
    APP_MODE_WEATHER    = 6,
    APP_MODE_SPECTRUM   = 7,  /* microphone audio visualiser */
    APP_MODE_YOUTUBE    = 8,  /* social counter */
    APP_MODE_INSTAGRAM  = 9,
    APP_MODE_TIKTOK     = 10,
    APP_MODE_MASTODON   = 11,
    APP_MODE_MAX        = 12,
} app_mode_t;

/* ── Backlight effect modes ────────────────────────────────────────── */
typedef enum {
    BL_MODE_STATIC = 0,
    BL_MODE_BREATH,
    BL_MODE_RAINBOW,
    BL_MODE_OFF,
    BL_MODE_WLED = 4,  /* Follow WLED strips via UDP Notifier broadcast */
    BL_MODE_SUNMOON = 5,  /* Follow Sun/Moon — track the sun (day) / moon (night) across the 6 tubes */
} backlight_mode_t;

/* ── Configuration structure ───────────────────────────────────────── */
typedef struct {
    /* Display */
    app_mode_t       current_mode;
    char             theme[32];
    char             time_type[8];       /* "12H", "24H", or "24H_NS" */
    char             clock_tube5[16];    /* tube-5 content in 24H-no-sec mode: "blank"|"weather" */
    bool             leading_zero;       /* true = show "09:30", false = show " 9:30" */
    uint8_t          led_brightness;     /* 0-100 */
    uint8_t          lcd_brightness;     /* primary brightness 0-100 */
    bool             auto_brightness;    /* enable night mode (shared window for LCD + LED) */
    uint8_t          night_brightness;   /* 0-100 — LCD backlight during the night window */
    uint8_t          led_night_brightness; /* 0-100 — accent LEDs during the night window */
    uint8_t          night_start_hour;   /* 0-23 */
    uint8_t          night_end_hour;     /* 0-23 (hour it switches back to primary) */
    backlight_mode_t backlight_mode;
    bool             backlight_on;
    uint8_t          backlight_rgb[6][3];
    uint8_t          sunmoon_sun_rgb[3];    /* Follow Sun/Moon mode — sun glow colour */
    uint8_t          sunmoon_moon_rgb[3];   /* Follow Sun/Moon mode — moon glow colour */
    uint8_t          led_effect_speed;  /* Breath / Rainbow animation speed 1 (slow) – 10 (fast); default 5 */
    bool             led_weather_override; /* let weather events (e.g. thunderstorm lightning) flash the accent LEDs */
    bool             wlive_animate;        /* WeatherLive: true = realtime animation, false = static (redraw only on clock change) */
    /* Custom clock face — active when clock_face == "custom".  Ignored for
     * asset-theme clock faces (clock_face == ""), EXCEPT custom_font below,
     * which also drives the 24H_CX asset-theme info panels.                */
    char             clock_face[16];      /* "" = use theme, "custom" = Custom clock face */
    char             custom_bg[32];       /* background: "WeatherLive" = animated sky, "CustomColor" =
                                            * solid/gradient fill below, else theme name */
    char             custom_bg_fill[16];  /* "solid","linear_v","linear_h","diagonal","radial" —
                                            * only used when custom_bg == "CustomColor" */
    uint8_t          custom_bg_color1[3]; /* solid fill colour, or gradient start colour */
    uint8_t          custom_bg_color2[3]; /* gradient end colour (ignored for "solid") */
    uint8_t          custom_font_color[3];   /* info-panel / label text RGB */
    uint8_t          custom_glyph_color[3];  /* clock digit glyph RGB */
    bool             custom_shadow;          /* shadow on/off for glyphs and text */
    uint8_t          custom_shadow_color[3]; /* shadow RGB (used when custom_shadow=true) */
    /* Night color set (issue #73) — a second font/glyph/shadow set that the
     * WeatherLive sky crossfades to through twilight, tracking the scene's
     * real geocoded sunrise/sunset. Only active while the animated WL sky is
     * the background; static/CustomColor backgrounds keep the day set. */
    bool             custom_night_colors;         /* enable the night set */
    uint8_t          custom_font_color_night[3];
    uint8_t          custom_glyph_color_night[3];
    bool             custom_shadow_night;         /* shadow on/off at night (flips at mid-twilight) */
    uint8_t          custom_shadow_color_night[3];
    char             custom_font[64];        /* TTF filename in /spiffs/fonts/; "" = logisoso (u8g2 fallback).
                                               * Also applies to 24H_CX asset-theme Outdoor Temp/Humidity/Wind/
                                               * AQI/combined-H-T panels (they share wl_text() with WeatherLive). */
    /* "DotMatrix" theme — procedural 7x14-cell dot-matrix glyphs (ships with
     * no on-disk assets, like WeatherLive).  Every cell in a glyph's grid is
     * always painted, on or off, so both colours are independently set. */
    uint8_t          dm_on_color[3];         /* lit-dot RGB */
    uint8_t          dm_off_color[3];        /* unlit-dot RGB */
    uint8_t          spectrum_rgb[3];       /* LED ring colour for Spectrum mode [R, G, B] */
    uint8_t          spectrum_lcd_rgb[3];   /* LCD bar colour for Spectrum mode [R, G, B] */
    bool             spectrum_lcd_wled;     /* true = LCD bars follow the WLED primary colour
                                               (live, when WLED Sync is receiving packets);
                                               falls back to spectrum_lcd_rgb otherwise */
    uint8_t          spectrum_led_source;   /* 0 = custom glow colour (amplitude-modulated),
                                               1 = follow configured accent mode (Static/Breath/Rainbow/Off) */
    bool             notify_update_on_display; /* true = draw red indicator on tube 6 when a
                                                  firmware update is available.  Driven by the
                                                  web UI via POST /api/update_notify. */
    uint16_t         enabled_modes;      /* bitmask: bit N = APP_MODE_N is enabled; default 0x1FF (all 9) */
    uint8_t          lcd_invert_mask;    /* bitmask: bit N = tube N needs INVON (colour-inverted replacement panel) */
    uint8_t          lcd_init_profile[6];    /* per-tube panel profile: 0=Standard, 1=Vivid (gamma curve selector) */
    uint8_t          lcd_vcom[6];        /* per-tube VMCTR1 VCOM value 0x00–0x3F (0=14, standard; higher=more contrast) */
    float            lcd_gamma[6];       /* per-tube software gamma exponent 0.5–3.0 (1.0=off; >1 darkens midtones for washed panels) */
    int8_t           lcd_col_offset[6];  /* per-tube CASET column offset adj. -8..+8 (ST7735S variants need +2) */
    int8_t           lcd_row_offset[6];  /* per-tube RASET row offset adj. -8..+8 (ST7735S variants need +1) */
    uint8_t          lcd_tube_brightness[6]; /* per-tube software brightness 0-100 (100 = no scaling, default) */

    /* Network */
    char             ssid[64];
    char             password[64];
    char             hostname[32];
    bool             static_ip_enabled;   /* false = DHCP (default) */
    char             static_ip[16];       /* dotted-quad; applied at boot only */
    char             static_netmask[16];
    char             static_gateway[16];
    char             static_dns1[16];
    char             static_dns2[16];     /* optional — empty = skip */

    /* Time */
    char             timezone[64];       /* POSIX TZ string e.g. "EST5EDT,M3.2.0,M11.1.0" */
    char             ntp_servers[4][64]; /* NTP server hostnames; empty string = skip slot */
    uint8_t          time_discipline_mode; /* experimental between-sync clock keeping (debug):
                                            * 0=off (reactive NTP), 1=ESP frequency disciplining,
                                            * 2=PCF8563 slaving */

    /* Weather */
    char             weather_source[16]; /* "wttr"/"openmeteo"/"metno" (no key), "openweather" (API key), or "external" (POST /api/weather) */
    char             weather_api_key[48];
    char             city[64];
    /* Last lat/lon pushed via POST /api/weather (external source).  Persisted so
     * the on-device Sunrise & Sunset panel works immediately on boot without a
     * geocoding call.  Written only when the coordinates change. */
    float            weather_ext_lat;
    float            weather_ext_lon;
    bool             weather_ext_loc_valid;
    char             temp_format[12];    /* "Celsius" or "Fahrenheit" */
    char             wind_unit[8];       /* "km/h", "mph", or "m/s" */

    /* YouTube / Bilibili */
    char             video_site[16];     /* "youtube" or "bilibili" */
    char             youtube_id[48];
    char             youtube_key[48];
    char             bili_uid[24];

    /* Audio */
    char             click_file[64];     /* sound played on physical button press */
    bool             button_sound;       /* enable/disable button click sound */
    char             ticker_file[64];    /* sound played when MQTT/HA ticker text arrives */
    bool             ticker_sound;       /* play ticker_file when ticker text is set */
    bool             audio_enabled;      /* false = DAC off, complete silence  */
    uint8_t          volume;             /* 0-100 */
    bool             mic_enabled;        /* false = mic task idles; Spectrum mode shows silence */
    uint8_t          mic_adc_channel;    /* ADC1 channel 0-7 (0=GPIO36, 1=GPIO37, 2=GPIO38, 3=GPIO39,
                                           4=GPIO32, 5=GPIO33, 6=GPIO34, 7=GPIO35). Runtime-changeable
                                           via the debug panel without a rebuild. */
    float            mic_silence_gate;   /* Spectral silence gate: display blanks when
                                            the SUM of post-floor band power falls below
                                            this (silence <10, quiet audio >50; 0 = off).
                                            Frames below it publish all-zero bands.
                                            Runtime-tuneable via the debug panel.
                                            Default 25 (semantics changed from the old
                                            RMS² gate whose default was 250). */
    float            mic_noise_floor[CFG_MIC_BAND_COUNT]; /* Saved per-band noise baseline
                                           captured via "Capture Baseline" in the web UI.
                                           Applied at boot when mic_calibration_saved is true,
                                           skipping the ~4 s Phase 1 auto-calibration ramp. */
    bool             mic_calibration_saved;            /* true = mic_noise_floor[] is valid
                                           and should be applied on the next boot. */

    /* SHT30 sensor calibration */
    float            sht30_temp_offset;  /* °C added to every raw reading (negative corrects
                                           for ESP32 self-heating).  Default 0.  Applied at
                                           runtime via sht30_set_offset(); no restart required. */

    /* Background-feature toggles (boot-time gates — restart required to apply).
     * All default true so a config.json from older firmware retains current
     * behaviour after upgrade.  Disabling frees the per-task stack and stops
     * the periodic polling — useful for users who don't want weather data,
     * subscriber counts, or LAN-side mDNS advertisement. */
    bool             weather_enabled;    /* gate weather_task creation */
    bool             update_check_enabled; /* gate update_check_task — periodic GitHub release
                                               check that drives the tube-6 update indicator and
                                               the Home Assistant "Nextube Update Available" topic */
    bool             social_enabled;     /* master gate — if false, subscribers_task never starts */
    bool             youtube_enabled;       /* gate subscribers_task YouTube fetch */
    uint16_t         sub_poll_interval_min; /* social counter re-poll interval in minutes (default 30, min 5) */
    bool             instagram_enabled; /* enable Instagram follower fetches (default true) */
    bool             tiktok_enabled;    /* enable TikTok follower fetches (default true) */
    char             instagram_user[48];   /* public Instagram username, no @ prefix */
    char             instagram_method[16]; /* "internal" (direct API) or "relay" */
    char             tiktok_user[48];       /* public TikTok username, no @ prefix */
    char             tiktok_key[64];        /* TikTok Research API bearer token; empty = use relay */
    char             tiktok_relay_host[64]; /* LAN IP of social_relay.py server, e.g. "192.168.1.100" */
    bool             mastodon_enabled;      /* enable Mastodon follower fetches (default false) */
    char             mastodon_user[48];     /* Mastodon username, no @ prefix */
    char             mastodon_instance[64]; /* Mastodon instance domain, e.g. "mastodon.social" */
    bool             mdns_enabled;       /* gate mDNS advertisement */

    /* MQTT / Home Assistant */
    bool             mqtt_enabled;       /* boot-time gate; restart required */
    char             mqtt_broker[64];    /* hostname or IP of MQTT broker */
    uint16_t         mqtt_port;          /* default 1883 */
    char             mqtt_user[32];      /* broker username (empty = anonymous) */
    char             mqtt_password[64];  /* broker password */
    bool             mqtt_ha_discovery;  /* publish HA auto-discovery payloads */
    /* Optional MQTT publishing groups (web UI checkboxes).  Publishing is
     * gated live; discovery payloads for newly enabled groups appear on the
     * next broker (re)connect or reboot. */
    bool             mqtt_pub_ntp;       /* NTP sync telemetry sensors (XTAL drift,
                                            RTC max error) — default off */
    bool             mqtt_pub_health;    /* WiFi RSSI / free heap / uptime sensors,
                                            published every 60 s — default off */
    bool             mqtt_pub_buttons;   /* touch presses as HA device triggers
                                            (left/middle/right) — default off */

    /* WLED Sync (receive) — listen for WLED UDP Notifier v2 broadcasts
     * and apply the primary colour + brightness to the local accent LEDs.
     * Boot-time gate: restart required to start or stop the listener task.
     * Select backlight_mode = BL_MODE_WLED to activate synchronisation.      */
    bool             wled_sync_enabled;  /* start UDP listener task at boot     */
    uint16_t         wled_sync_port;     /* WLED Notifier port, default 21324   */

    /* Album */
    uint16_t         album_switch_ms;
    bool             album_shuffle;    /* true = randomise playback order on load */

    /* Weather panel rotation interval (ms between temp/humidity panel switch) */
    uint16_t         weather_panel_ms;
    bool             weather_panel0_en;  /* true = show temperature panel    */
    bool             weather_panel1_en;  /* true = show humidity panel       */
    bool             weather_panel2_en;  /* true = show sunrise/sunset panel */
    bool             weather_panel3_en;  /* true = show wind speed panel     */
    bool             weather_panel4_en;  /* true = show daily Hi/Lo panel    */

    /* Mode Rotation – auto-cycle through enabled modes on a timer.
     * When rotation_enabled is false the mode never changes automatically;
     * only UI API calls and physical button presses can change it.
     * rotation_modes == 0  → cycle all modes set in enabled_modes (default).
     * rotation_modes != 0  → cycle only the intersection of rotation_modes
     *                        and enabled_modes; falls back to enabled_modes if
     *                        the intersection is empty. */
    bool             rotation_enabled;     /* false = manual-only mode switching */
    uint16_t         rotation_interval_s;  /* seconds per mode; 0 treated as 60 */
    uint16_t         rotation_modes;       /* bitmask of modes to rotate; 0 = all enabled */
    uint8_t          rotation_weights[12]; /* per-mode dwell multiplier (1–99, default 1).
                                            * effective dwell = rotation_interval_s × weight.
                                            * index == app_mode_t value.  0 treated as 1. */

    /* Theme Rotation – auto-cycle through themes on a timer.
     * theme_rotation_count == 0 → rotate all installed themes.
     * theme_rotation_count  > 0 → rotate only the listed themes. */
    bool             theme_rotation_enabled;         /* false = manual-only */
    uint16_t         theme_rotation_interval_s;      /* seconds per theme; 0 treated as 300 */
    uint8_t          theme_rotation_count;           /* 0 = all themes */
    char             theme_rotation_themes[16][32];  /* selected theme names */

    /* Scheduled Burn-in – automatic LCD colour-cycle recovery.
     * Fires at midnight on the configured interval (weekly = every Sunday,
     * monthly = 1st of month).  Calls display_set_burnin_mask() autonomously
     * from within the display task — no web UI interaction required. */
    bool             burnin_auto_enabled;      /* false = disabled (default)         */
    uint8_t          burnin_auto_mask;         /* tube bitmask; 0x3F = all 6 tubes   */
    uint32_t         burnin_auto_duration_s;   /* session length, 1–14400 s (default 3600 = 1 hr) */
    char             burnin_auto_interval[8];  /* "weekly" (Sunday) or "monthly" (1st) */
    uint8_t          burnin_auto_hour;         /* hour of day to fire, 0-23 (default 0 = midnight) */
    char             burnin_auto_mode[16];     /* "colour-cycle" (default) or "snow" */

    /* Date display format (Custom Clock / date panels) */
    char             date_format[12];    /* "DD/MM/YY" (default, European) or "MM/DD/YY" (US) */

    /* Display localisation — ISO 639-1 language code controlling on-device text
     * that is rendered as words (currently the tube-6 WEEKDATE day name).
     * Supported: "en" (default), Western European (fr, de, es, it, pt, nl) and
     * Nordic (sv, no, da, fi).  The web UI language is chosen separately and
     * stored per-browser; this field is the device/tube setting only. */
    char             language[6];

    /* 24H Custom clock — tube 6 rotating info panels.
     * Each bool gates one panel; at least one must be enabled (enforced on load).
     * Rotation advances to the next enabled panel every tube6_panel_ms ms.      */
    bool             tube6_panel_weather;    /* Weather icon — shows current condition as full-tube JPEG */
    bool             tube6_panel_weekdate;   /* Day name (top half) + MMDD date (bottom half) */
    bool             tube6_panel_ht;         /* SHT30 temp (top half) + humidity (bottom half) */
    bool             tube6_panel_temp;       /* Outdoor temperature + today's forecast Hi/Lo */
    bool             tube6_panel_sunrise;    /* Sunrise & sunset times (U8g2 icon + local time) */
    bool             tube6_panel_push;       /* Externally-pushed JPG (POST /api/cx_image?tube=6) */
    bool             tube6_panel_humidity;   /* Outdoor humidity (drop symbol + value) */
    bool             tube6_panel_wind;        /* Wind speed (wind symbol + km/h value) */
    bool             tube6_panel_aqi;         /* Air quality — US EPA AQI (Open-Meteo, keyless) */
    bool             tube6_panel_outdoor_ht;  /* Outdoor temp (top half) + outdoor humidity (bottom half) —
                                                * combines what tube6_panel_temp + tube6_panel_humidity would
                                                * otherwise need two separate rotation slots for. */
    uint16_t         tube6_panel_ms;         /* ms per panel; below 1000 resets to 5000 */
    /* Dual info-panel mode (24H Custom): when true the colon is dropped and an
     * INDEPENDENT info panel is shown on BOTH tube 5 (2nd-from-right, LCD index
     * 4) and tube 6 (rightmost, LCD index 5).  Layout becomes H H M M [p5][p6].
     * tube 5 rotates through its own enabled set below; tube 6 keeps using the
     * tube6_panel_* set above.  When false: original single-panel layout
     * H H : M M [p6] with the colon. */
    bool             cx_dual_panel;
    bool             tube5_panel_weather;
    bool             tube5_panel_weekdate;
    bool             tube5_panel_ht;
    bool             tube5_panel_temp;
    bool             tube5_panel_sunrise;
    bool             tube5_panel_push;       /* Externally-pushed JPG (POST /api/cx_image?tube=5) */
    bool             tube5_panel_humidity;   /* Outdoor humidity (drop symbol + value) */
    bool             tube5_panel_wind;        /* Wind speed (wind symbol + km/h value) */
    bool             tube5_panel_aqi;         /* Air quality — US EPA AQI (Open-Meteo, keyless) */
    bool             tube5_panel_outdoor_ht;  /* Outdoor temp (top half) + outdoor humidity (bottom half) */
    char             aqi_standard[8];         /* AQI scale: "auto" (by location) | "us" | "eu" */
    char             update_repo[64];
} nextube_config_t;

/** Initialise config module – loads from flash or sets defaults. */
void config_mgr_init(void);

/** Save current config.json to NVS before a LittleFS wipe.
 *  config_mgr_init() will restore it automatically if no config.json
 *  is found on the freshly-flashed filesystem. */
void config_backup_to_nvs(void);

/** Global TLS serialisation – acquire before any esp_http_client HTTPS call,
 *  release after esp_http_client_cleanup().  Guarantees only one mbedTLS
 *  SSL context exists at a time, preventing internal-SRAM exhaustion when
 *  the weather task and the social-counter task would otherwise overlap. */
void tls_sem_take(void);
void tls_sem_give(void);

/** Acquire / release the config mutex.
 *  Must bracket any code that reads multiple fields from config_get() to
 *  prevent torn reads when config_set_json() writes concurrently.
 *  The underlying mutex is recursive: the same task may call config_lock()
 *  while already holding it (e.g. on_touch() → config_set_json()). */
void config_lock(void);
void config_unlock(void);

/** Get pointer to the live config.  Call only while holding config_lock(). */
const nextube_config_t *config_get(void);

/** Update config from a JSON string, save to flash, and broadcast. */
bool config_set_json(const char *json, size_t len);

/** Serialise current config to a heap-allocated JSON string (caller frees).
 *  include_password: true  → includes the WiFi password (for flash save / backup).
 *                    false → omits it and adds a "has_password" bool instead
 *                            (for GET /api/settings, so the secret never leaves
 *                            the device and no second parse is needed). */
char *config_to_json(bool include_password);

/** Reset to factory defaults and save. */
void config_reset(void);

/** Advance to the next enabled mode and save.
 *  Called by the display task when the rotation timer fires.
 *  Skips modes that are not set in enabled_modes.  Thread-safe. */
void config_set_mode    (app_mode_t mode); /* RAM-only – no flash write */
void config_advance_mode(void);            /* RAM-only – no flash write */
void config_set_theme   (const char *theme); /* RAM-only – no flash write */

/** Return the canonical display name string for a mode enum value.
 *  Returns "Clock" for any out-of-range value.
 *  Single authoritative definition – eliminates duplicate string tables in
 *  main.c, config_mgr.c, and web_server.c. */
const char *app_mode_name(app_mode_t mode);

#ifdef __cplusplus
}
#endif
