/**
 * @file config_mgr.c
 * @brief JSON-based persistent configuration for Nextube
 */
#include "config_mgr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "config";
static const char *CONFIG_PATH = "/spiffs/config.json";

static nextube_config_t s_cfg;
static SemaphoreHandle_t s_mutex;

/* ── Defaults ──────────────────────────────────────────────────────── */
static void set_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));

    s_cfg.current_mode    = APP_MODE_CLOCK;
    strcpy(s_cfg.theme, "NixieOY");
    strcpy(s_cfg.time_type, "24H");
    s_cfg.lcd_brightness  = 30;
    s_cfg.led_brightness  = 60;
    s_cfg.backlight_mode  = BL_MODE_BREATH;
    s_cfg.backlight_on    = true;
    /* All modes enabled by default except Custom Clock (bit 5).
     * Custom Clock is an opt-in alternative to Clock; having both active
     * at once just clutters the touch cycle for new users. */
    s_cfg.enabled_modes   = 0xFF & ~(1 << APP_MODE_CUSTOM_CLOCK);  /* 0xDF */

    /* Default rainbow-ish backlight colours */
    uint8_t defaults[6][3] = {
        {200,0,0}, {0,200,0}, {0,0,200},
        {110,100,0}, {0,200,200}, {200,0,200}
    };
    memcpy(s_cfg.backlight_rgb, defaults, sizeof(defaults));

    strcpy(s_cfg.hostname, "nextube-remaster");
    strcpy(s_cfg.ntp_server, "pool.ntp.org");
    s_cfg.time_zone = -21600;  /* UTC-6 */

    strcpy(s_cfg.weather_source, "wttr");  /* default: no API key needed */
    strcpy(s_cfg.weather_api_key, "");
    strcpy(s_cfg.city, "Airdrie,CA");
    strcpy(s_cfg.temp_format, "Celsius");

    strcpy(s_cfg.video_site, "youtube");
    strcpy(s_cfg.youtube_key, "");
    strcpy(s_cfg.bili_uid, "1");

    strcpy(s_cfg.music_file, "/spiffs/audio/Unwritten.mp3");
    strcpy(s_cfg.bell_file, "/spiffs/audio/bell.wav");
    strcpy(s_cfg.tone_file, "/spiffs/audio/tremolo3.wav");
    strcpy(s_cfg.timer_file, "/spiffs/audio/timer.wav");
    s_cfg.volume = 20;

    s_cfg.countdown_minutes = 1;
    s_cfg.pomodoro_work     = 25;
    s_cfg.pomodoro_break    = 5;
    s_cfg.album_switch_ms   = 2000;

    /* Rotation off by default; user must explicitly enable it */
    s_cfg.rotation_enabled    = false;
    s_cfg.rotation_interval_s = 60;
}

/* ── JSON helpers ──────────────────────────────────────────────────── */
static void json_read_str(cJSON *root, const char *key, char *dst, size_t max)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(dst, item->valuestring, max - 1);
        dst[max - 1] = '\0';
    }
}

static void json_read_int(cJSON *root, const char *key, int *dst)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(item)) *dst = item->valueint;
}

static void json_read_u8(cJSON *root, const char *key, uint8_t *dst)
{
    int v = *dst;
    json_read_int(root, key, &v);
    *dst = (uint8_t)v;
}

static void json_read_u16(cJSON *root, const char *key, uint16_t *dst)
{
    int v = *dst;
    json_read_int(root, key, &v);
    *dst = (uint16_t)v;
}

static void parse_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed, keeping defaults");
        return;
    }

    /* Mode */
    cJSON *apps = cJSON_GetObjectItem(root, "apps");
    if (cJSON_IsArray(apps) && cJSON_GetArraySize(apps) > 0) {
        cJSON *app0 = cJSON_GetArrayItem(apps, 0);
        char app_name[32] = {0};
        json_read_str(app0, "app", app_name, sizeof(app_name));
        if      (strcmp(app_name, "Clock")      == 0) s_cfg.current_mode = APP_MODE_CLOCK;
        else if (strcmp(app_name, "Countdown")   == 0) s_cfg.current_mode = APP_MODE_COUNTDOWN;
        else if (strcmp(app_name, "Scoreboard")  == 0) s_cfg.current_mode = APP_MODE_SCOREBOARD;
        else if (strcmp(app_name, "Pomodoro")    == 0) s_cfg.current_mode = APP_MODE_POMODORO;
        else if (strcmp(app_name, "YouTube")     == 0) s_cfg.current_mode = APP_MODE_YOUTUBE;
        else if (strcmp(app_name, "CustomClock") == 0) s_cfg.current_mode = APP_MODE_CUSTOM_CLOCK;
        else if (strcmp(app_name, "Album")       == 0) s_cfg.current_mode = APP_MODE_ALBUM;
        else if (strcmp(app_name, "Weather")     == 0) s_cfg.current_mode = APP_MODE_WEATHER;

        json_read_str(app0, "theme", s_cfg.theme, sizeof(s_cfg.theme));
        json_read_str(app0, "type",  s_cfg.time_type, sizeof(s_cfg.time_type));
    }

    json_read_str(root, "ssid",             s_cfg.ssid,            sizeof(s_cfg.ssid));
    json_read_str(root, "password",         s_cfg.password,        sizeof(s_cfg.password));
    json_read_str(root, "video_site",       s_cfg.video_site,      sizeof(s_cfg.video_site));
    json_read_str(root, "youtube_id",       s_cfg.youtube_id,      sizeof(s_cfg.youtube_id));
    json_read_str(root, "youtube_key",      s_cfg.youtube_key,     sizeof(s_cfg.youtube_key));
    json_read_str(root, "bili_uid",         s_cfg.bili_uid,        sizeof(s_cfg.bili_uid));
    json_read_str(root, "weather_source",   s_cfg.weather_source,  sizeof(s_cfg.weather_source));
    json_read_str(root, "weather_api_key",  s_cfg.weather_api_key, sizeof(s_cfg.weather_api_key));
    json_read_str(root, "City",             s_cfg.city,            sizeof(s_cfg.city));
    json_read_str(root, "temperature_formate", s_cfg.temp_format,  sizeof(s_cfg.temp_format));
    json_read_str(root, "music_file",       s_cfg.music_file,      sizeof(s_cfg.music_file));
    json_read_str(root, "bell_file",        s_cfg.bell_file,       sizeof(s_cfg.bell_file));
    json_read_str(root, "tone_file",        s_cfg.tone_file,       sizeof(s_cfg.tone_file));
    json_read_str(root, "timer_file",       s_cfg.timer_file,      sizeof(s_cfg.timer_file));

    /* time_zone: new format = ±hours (|value| ≤ 24), legacy = raw seconds (|value| > 24).
     * Migration: old SPIFFS files stored -21600 (seconds); new format stores -6 (hours). */
    {
        cJSON *tz_item = cJSON_GetObjectItem(root, "time_zone");
        if (cJSON_IsNumber(tz_item)) {
            double v = tz_item->valuedouble;
            if (v > 24.0 || v < -24.0)
                s_cfg.time_zone = (int32_t)v;             /* legacy: already in seconds */
            else
                s_cfg.time_zone = (int32_t)(v * 3600.0); /* new: ±hours                */
        }
    }

    json_read_u8(root, "volume",         &s_cfg.volume);
    json_read_u8(root, "lcd_brightness", &s_cfg.lcd_brightness);
    json_read_u8(root, "led_brightness", &s_cfg.led_brightness);

    json_read_u16(root, "default_countdown_time", &s_cfg.countdown_minutes);
    json_read_u16(root, "album_switch_time",      &s_cfg.album_switch_ms);

    /* Backlight mode */
    char bl_mode[16] = {0};
    json_read_str(root, "backlight_mode", bl_mode, sizeof(bl_mode));
    if      (strcmp(bl_mode, "Static")  == 0) s_cfg.backlight_mode = BL_MODE_STATIC;
    else if (strcmp(bl_mode, "Breath")  == 0) s_cfg.backlight_mode = BL_MODE_BREATH;
    else if (strcmp(bl_mode, "Rainbow") == 0) s_cfg.backlight_mode = BL_MODE_RAINBOW;

    char bl_onoff[8] = {0};
    json_read_str(root, "backlight_onoff", bl_onoff, sizeof(bl_onoff));
    /* Only update when the key is actually present in the payload.
     * If absent (e.g. a mode-change-only JSON), bl_onoff stays empty and
     * we must not corrupt the current state: strcmp("","OFF")!=0 would
     * incorrectly force backlight_on = true every time. */
    if (bl_onoff[0] != '\0') {
        s_cfg.backlight_on = (strcmp(bl_onoff, "OFF") != 0);
    }

    json_read_u8(root, "enabled_modes", &s_cfg.enabled_modes);
    /* Clock and Custom Clock are mutually exclusive in the UI.  Enforce that
     * at least one clock mode is always active so the device can never be
     * stuck with no way to show the time. */
    {
        bool has_clock  = (s_cfg.enabled_modes & (1 << APP_MODE_CLOCK))        != 0;
        bool has_custom = (s_cfg.enabled_modes & (1 << APP_MODE_CUSTOM_CLOCK)) != 0;
        if (!has_clock && !has_custom)
            s_cfg.enabled_modes |= (1 << APP_MODE_CLOCK);
    }

    /* Mode rotation */
    {
        cJSON *rot_en = cJSON_GetObjectItem(root, "rotation_enabled");
        if (cJSON_IsBool(rot_en))
            s_cfg.rotation_enabled = cJSON_IsTrue(rot_en);
    }
    json_read_u16(root, "rotation_interval_s", &s_cfg.rotation_interval_s);
    if (s_cfg.rotation_interval_s == 0) s_cfg.rotation_interval_s = 60;

    /* Backlight RGB array */
    cJSON *bl_rgb = cJSON_GetObjectItem(root, "backlight_RGB");
    if (cJSON_IsArray(bl_rgb)) {
        int cnt = cJSON_GetArraySize(bl_rgb);
        if (cnt > 6) cnt = 6;
        for (int i = 0; i < cnt; i++) {
            cJSON *rgb = cJSON_GetArrayItem(bl_rgb, i);
            if (cJSON_IsArray(rgb) && cJSON_GetArraySize(rgb) >= 3) {
                s_cfg.backlight_rgb[i][0] = cJSON_GetArrayItem(rgb, 0)->valueint;
                s_cfg.backlight_rgb[i][1] = cJSON_GetArrayItem(rgb, 1)->valueint;
                s_cfg.backlight_rgb[i][2] = cJSON_GetArrayItem(rgb, 2)->valueint;
            }
        }
    }

    cJSON_Delete(root);
}

/* ── File I/O ──────────────────────────────────────────────────────── */
static bool load_from_flash(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "No config file, using defaults");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8192) { fclose(f); return false; }

    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    parse_json(buf);
    free(buf);
    ESP_LOGI(TAG, "Config loaded from flash");
    return true;
}

static void save_to_flash(void)
{
    char *json = config_to_json();
    if (!json) return;

    FILE *f = fopen(CONFIG_PATH, "w");
    if (f) {
        fwrite(json, 1, strlen(json), f);
        fclose(f);
        ESP_LOGI(TAG, "Config saved to flash (%u bytes)", (unsigned)strlen(json));
    } else {
        ESP_LOGE(TAG, "Failed to open config for writing");
    }
    free(json);
}

/* ── Public API ────────────────────────────────────────────────────── */
void config_mgr_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    set_defaults();
    load_from_flash();
}

const nextube_config_t *config_get(void)
{
    return &s_cfg;
}

bool config_set_json(const char *json, size_t len)
{
    if (!json || len == 0) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    parse_json(json);
    save_to_flash();
    xSemaphoreGive(s_mutex);
    return true;
}

char *config_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    /* apps array (for backward compat with original firmware format) */
    cJSON *apps = cJSON_AddArrayToObject(root, "apps");
    cJSON *app0 = cJSON_CreateObject();
    cJSON_AddStringToObject(app0, "name", "app1");

    const char *mode_names[] = {"Clock","Countdown","Scoreboard","Pomodoro",
                                "YouTube","CustomClock","Album","Weather"};
    int mode_idx = (int)s_cfg.current_mode;
    if (mode_idx < 0 || mode_idx >= (int)(sizeof(mode_names)/sizeof(mode_names[0])))
        mode_idx = 0;
    cJSON_AddStringToObject(app0, "app",   mode_names[mode_idx]);
    cJSON_AddStringToObject(app0, "theme", s_cfg.theme);
    cJSON_AddStringToObject(app0, "type",  s_cfg.time_type);
    cJSON_AddItemToArray(apps, app0);

    cJSON_AddStringToObject(root, "ssid",             s_cfg.ssid);
    cJSON_AddStringToObject(root, "password",         s_cfg.password);
    cJSON_AddStringToObject(root, "video_site",       s_cfg.video_site);
    cJSON_AddStringToObject(root, "youtube_id",       s_cfg.youtube_id);
    cJSON_AddStringToObject(root, "youtube_key",      s_cfg.youtube_key);
    cJSON_AddStringToObject(root, "bili_uid",         s_cfg.bili_uid);
    /* Serialize as ±hours so the web UI shows human-readable values (e.g. -6, +5.5) */
    cJSON_AddNumberToObject(root, "time_zone", s_cfg.time_zone / 3600.0);
    cJSON_AddStringToObject(root, "weather_source",   s_cfg.weather_source);
    cJSON_AddStringToObject(root, "weather_api_key",  s_cfg.weather_api_key);
    cJSON_AddStringToObject(root, "City",             s_cfg.city);
    cJSON_AddStringToObject(root, "temperature_formate", s_cfg.temp_format);
    cJSON_AddStringToObject(root, "music_file",       s_cfg.music_file);
    cJSON_AddStringToObject(root, "bell_file",        s_cfg.bell_file);
    cJSON_AddStringToObject(root, "tone_file",        s_cfg.tone_file);
    cJSON_AddStringToObject(root, "timer_file",       s_cfg.timer_file);
    cJSON_AddNumberToObject(root, "volume",           s_cfg.volume);
    cJSON_AddNumberToObject(root, "lcd_brightness",   s_cfg.lcd_brightness);
    cJSON_AddNumberToObject(root, "led_brightness",   s_cfg.led_brightness);
    cJSON_AddNumberToObject(root, "default_countdown_time", s_cfg.countdown_minutes);
    cJSON_AddNumberToObject(root, "album_switch_time",      s_cfg.album_switch_ms);

    const char *bl_modes[] = {"Static","Breath","Rainbow","Off"};
    cJSON_AddStringToObject(root, "backlight_mode",  bl_modes[s_cfg.backlight_mode]);
    cJSON_AddStringToObject(root, "backlight_onoff", s_cfg.backlight_on ? "ON" : "OFF");
    cJSON_AddNumberToObject(root, "enabled_modes",      s_cfg.enabled_modes);
    cJSON_AddBoolToObject  (root, "rotation_enabled",   s_cfg.rotation_enabled);
    cJSON_AddNumberToObject(root, "rotation_interval_s", s_cfg.rotation_interval_s);

    cJSON *bl_rgb = cJSON_AddArrayToObject(root, "backlight_RGB");
    for (int i = 0; i < 6; i++) {
        cJSON *c = cJSON_CreateIntArray((const int[]){
            s_cfg.backlight_rgb[i][0],
            s_cfg.backlight_rgb[i][1],
            s_cfg.backlight_rgb[i][2]}, 3);
        cJSON_AddItemToArray(bl_rgb, c);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

void config_advance_mode(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Step forward through APP_MODE_MAX slots, skipping disabled ones.
     * Worst case: all modes except the current one are disabled, so we
     * try APP_MODE_MAX times before giving up (stays on current mode). */
    int m = (int)s_cfg.current_mode;
    for (int tries = 0; tries < APP_MODE_MAX; tries++) {
        m = (m + 1) % APP_MODE_MAX;
        if (s_cfg.enabled_modes & (1 << m)) break;
    }

    if ((app_mode_t)m != s_cfg.current_mode) {
        s_cfg.current_mode = (app_mode_t)m;
        save_to_flash();
        ESP_LOGI(TAG, "Rotation: advanced to mode %d", m);
    }

    xSemaphoreGive(s_mutex);
}

void config_reset(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    set_defaults();
    save_to_flash();
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Config reset to factory defaults");
}
