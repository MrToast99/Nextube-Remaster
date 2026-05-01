/**
 * @file config_mgr.h
 * @brief Persistent JSON configuration – stored in /spiffs/config.json
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── App mode identifiers ──────────────────────────────────────────── */
typedef enum {
    APP_MODE_CLOCK = 0,
    APP_MODE_COUNTDOWN,
    APP_MODE_SCOREBOARD,
    APP_MODE_POMODORO,
    APP_MODE_YOUTUBE,
    APP_MODE_CUSTOM_CLOCK,
    APP_MODE_ALBUM,
    APP_MODE_WEATHER,
    APP_MODE_SPECTRUM,  /* = 8 — microphone audio visualiser */
    APP_MODE_MAX,
} app_mode_t;

/* ── Backlight effect modes ────────────────────────────────────────── */
typedef enum {
    BL_MODE_STATIC = 0,
    BL_MODE_BREATH,
    BL_MODE_RAINBOW,
    BL_MODE_OFF,
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
    bool             auto_brightness;    /* enable night mode */
    uint8_t          night_brightness;   /* 0-100 */
    uint8_t          night_start_hour;   /* 0-23 */
    uint8_t          night_end_hour;     /* 0-23 (hour it switches back to primary) */
    backlight_mode_t backlight_mode;
    bool             backlight_on;
    uint8_t          backlight_rgb[6][3];
    uint8_t          spectrum_rgb[3];    /* LED colour for Spectrum mode [R, G, B] */
    uint16_t         enabled_modes;      /* bitmask: bit N = APP_MODE_N is enabled; default 0x1FF (all 9) */

    /* Network */
    char             ssid[64];
    char             password[64];
    char             hostname[32];

    /* Time */
    char             timezone[64];       /* POSIX TZ string e.g. "EST5EDT,M3.2.0,M11.1.0" */
    char             ntp_servers[4][64]; /* NTP server hostnames; empty string = skip slot */

    /* Weather */
    char             weather_source[16]; /* "wttr" (no key) or "openweather" (API key) */
    char             weather_api_key[48];
    char             city[64];
    char             temp_format[12];    /* "Celsius" or "Fahrenheit" */

    /* YouTube / Bilibili */
    char             video_site[16];     /* "youtube" or "bilibili" */
    char             youtube_id[48];
    char             youtube_key[48];
    char             bili_uid[24];

    /* Audio */
    char             music_file[64];
    char             bell_file[64];
    char             tone_file[64];
    char             timer_file[64];
    char             click_file[64];     /* sound played on physical button press */
    bool             button_sound;       /* enable/disable button click sound */
    bool             audio_enabled;      /* false = DAC off, complete silence  */
    uint8_t          volume;             /* 0-100 */
    bool             mic_enabled;        /* false = mic task idles; Spectrum mode shows silence */
    uint8_t          mic_adc_channel;    /* ADC1 channel 0-7 (0=GPIO36, 1=GPIO37, 2=GPIO38, 3=GPIO39,
                                           4=GPIO32, 5=GPIO33, 6=GPIO34, 7=GPIO35). Runtime-changeable
                                           via the debug panel without a rebuild. */
    float            mic_silence_gate;   /* Frame RMS² silence threshold (0–4096²).
                                           Frames below this value publish all-zero bands.
                                           Runtime-tuneable via the debug panel. Default 250. */

    /* Countdown / Pomodoro */
    uint16_t         countdown_minutes;
    uint16_t         pomodoro_work;
    uint16_t         pomodoro_break;

    /* Album */
    uint16_t         album_switch_ms;

    /* Weather panel rotation interval (ms between temp/humidity panel switch) */
    uint16_t         weather_panel_ms;
    bool             weather_panel0_en;  /* true = show temperature panel */
    bool             weather_panel1_en;  /* true = show humidity panel    */

    /* Mode Rotation – auto-cycle through enabled modes on a timer.
     * When rotation_enabled is false the mode never changes automatically;
     * only UI API calls and physical button presses can change it. */
    bool             rotation_enabled;     /* false = manual-only mode switching */
    uint16_t         rotation_interval_s;  /* seconds per mode; 0 treated as 60 */
} nextube_config_t;

/** Initialise config module – loads from flash or sets defaults. */
void config_mgr_init(void);

/** Get pointer to the live (thread-safe read) config. */
const nextube_config_t *config_get(void);

/** Update config from a JSON string, save to flash, and broadcast. */
bool config_set_json(const char *json, size_t len);

/** Serialise current config to a heap-allocated JSON string (caller frees). */
char *config_to_json(void);

/** Reset to factory defaults and save. */
void config_reset(void);

/** Advance to the next enabled mode and save.
 *  Called by the display task when the rotation timer fires.
 *  Skips modes that are not set in enabled_modes.  Thread-safe. */
void config_set_mode    (app_mode_t mode); /* RAM-only – no flash write */
void config_advance_mode(void);            /* RAM-only – no flash write */

/** Return the canonical display name string for a mode enum value.
 *  Returns "Clock" for any out-of-range value.
 *  Single authoritative definition – eliminates duplicate string tables in
 *  main.c, config_mgr.c, and web_server.c. */
const char *app_mode_name(app_mode_t mode);

#ifdef __cplusplus
}
#endif
