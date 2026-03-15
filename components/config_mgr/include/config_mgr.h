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
    char             time_type[8];       /* "12H" or "24H" */
    uint8_t          lcd_brightness;     /* 0-100 */
    uint8_t          led_brightness;     /* 0-100 */
    backlight_mode_t backlight_mode;
    bool             backlight_on;
    uint8_t          backlight_rgb[6][3];
    uint8_t          enabled_modes;      /* bitmask: bit N = APP_MODE_N is enabled; default 0x7F (all 7) */

    /* Network */
    char             ssid[64];
    char             password[64];
    char             hostname[32];

    /* Time */
    int32_t          time_zone;          /* offset in seconds */
    char             ntp_server[64];

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
    uint8_t          volume;             /* 0-100 */

    /* Countdown / Pomodoro */
    uint16_t         countdown_minutes;
    uint16_t         pomodoro_work;
    uint16_t         pomodoro_break;

    /* Album */
    uint16_t         album_switch_ms;
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

#ifdef __cplusplus
}
#endif
