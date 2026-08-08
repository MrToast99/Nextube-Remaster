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
#include "freertos/task.h"   /* xTaskGetCurrentTaskHandle — tls_sem ownership */
#include "nvs_flash.h"

static const char *TAG = "config";
static const char *CONFIG_PATH = "/spiffs/config.json";

static nextube_config_t s_cfg;
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_tls_sem;

static uint32_t s_flash_write_fail_count = 0;
void config_mgr_note_flash_write_failure(void) { s_flash_write_fail_count++; }
uint32_t config_mgr_get_flash_write_fail_count(void) { return s_flash_write_fail_count; }

/* ── Defaults ──────────────────────────────────────────────────────── */
static void set_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));

    s_cfg.current_mode    = APP_MODE_CLOCK;
    strncpy(s_cfg.theme, "NixieOY", sizeof(s_cfg.theme) - 1);
    strncpy(s_cfg.time_type, "24H", sizeof(s_cfg.time_type) - 1);
    strncpy(s_cfg.clock_tube5, "blank", sizeof(s_cfg.clock_tube5) - 1);
    s_cfg.leading_zero    = false;
    s_cfg.led_brightness  = 60;
    s_cfg.lcd_brightness  = 60;
    s_cfg.auto_brightness = false;
    s_cfg.night_brightness = 30;
    s_cfg.led_night_brightness = 20;
    s_cfg.night_start_hour = 22;
    s_cfg.night_end_hour   = 7;
    s_cfg.backlight_mode  = BL_MODE_BREATH;
    s_cfg.backlight_on    = true;
    s_cfg.led_effect_speed = 5;
    s_cfg.led_weather_override = false;
    s_cfg.wlive_animate    = true;   /* realtime animation by default */
    /* Custom clock face defaults: WeatherLive sky, white glyphs/text, shadow on */
    s_cfg.clock_face[0]         = '\0';
    strncpy(s_cfg.custom_bg, "WeatherLive", sizeof(s_cfg.custom_bg) - 1);
    strncpy(s_cfg.custom_bg_fill, "solid", sizeof(s_cfg.custom_bg_fill) - 1);
    s_cfg.custom_bg_color1[0]   = 0;   s_cfg.custom_bg_color1[1]   = 0;   s_cfg.custom_bg_color1[2]   = 0;
    s_cfg.custom_bg_color2[0]   = 60;  s_cfg.custom_bg_color2[1]   = 60;  s_cfg.custom_bg_color2[2]   = 120;
    s_cfg.custom_font_color[0]  = 255; s_cfg.custom_font_color[1]  = 255; s_cfg.custom_font_color[2]  = 255;
    s_cfg.custom_glyph_color[0] = 255; s_cfg.custom_glyph_color[1] = 255; s_cfg.custom_glyph_color[2] = 255;
    s_cfg.custom_shadow         = true;
    s_cfg.custom_shadow_color[0]= 0;   s_cfg.custom_shadow_color[1]= 0;   s_cfg.custom_shadow_color[2]= 0;
    s_cfg.custom_glyph_shadow          = true;
    s_cfg.custom_glyph_shadow_color[0] = 0; s_cfg.custom_glyph_shadow_color[1] = 0; s_cfg.custom_glyph_shadow_color[2] = 0;
    /* Night color set: disabled; colors mirror the day defaults so enabling
     * it is a no-op until the user actually picks night colors. */
    s_cfg.custom_night_colors   = false;
    s_cfg.custom_font_color_night[0]  = 255; s_cfg.custom_font_color_night[1]  = 255; s_cfg.custom_font_color_night[2]  = 255;
    s_cfg.custom_glyph_color_night[0] = 255; s_cfg.custom_glyph_color_night[1] = 255; s_cfg.custom_glyph_color_night[2] = 255;
    s_cfg.custom_shadow_night         = true;
    s_cfg.custom_shadow_color_night[0]= 0;   s_cfg.custom_shadow_color_night[1]= 0;   s_cfg.custom_shadow_color_night[2]= 0;
    s_cfg.custom_glyph_shadow_night          = true;
    s_cfg.custom_glyph_shadow_color_night[0] = 0; s_cfg.custom_glyph_shadow_color_night[1] = 0; s_cfg.custom_glyph_shadow_color_night[2] = 0;
    s_cfg.custom_font[0]        = '\0';
    s_cfg.dm_on_color[0]  = 255; s_cfg.dm_on_color[1]  = 255; s_cfg.dm_on_color[2]  = 255;
    s_cfg.dm_off_color[0] = 25;  s_cfg.dm_off_color[1] = 25;  s_cfg.dm_off_color[2] = 25;
    /* All modes enabled by default. Clock and Date are independent — both
     * can be active simultaneously in the touch cycle. */
    s_cfg.enabled_modes   = 0xFFF;   /* all 12 modes (bits 0–11) */

    /* Spectrum mode LED colour — matches stock firmware spectrum_RGB default */
    s_cfg.spectrum_rgb[0] = 50;
    s_cfg.spectrum_rgb[1] = 80;
    s_cfg.spectrum_rgb[2] = 100;

    /* Spectrum mode LCD bar colour — classic green matches old hardcoded default */
    s_cfg.spectrum_lcd_rgb[0] = 30;
    s_cfg.spectrum_lcd_rgb[1] = 220;
    s_cfg.spectrum_lcd_rgb[2] = 30;
    s_cfg.spectrum_lcd_wled   = false;  /* opt-in: bars follow WLED primary colour */

    /* Follow Sun/Moon LED mode defaults. */
    s_cfg.sunmoon_sun_rgb[0] = 255; s_cfg.sunmoon_sun_rgb[1] = 213; s_cfg.sunmoon_sun_rgb[2] = 46;
    s_cfg.sunmoon_moon_rgb[0] = 230; s_cfg.sunmoon_moon_rgb[1] = 234; s_cfg.sunmoon_moon_rgb[2] = 248;

    /* Spectrum LED source — 0 = custom glow colour (amplitude-modulated),
     * 1 = follow configured accent mode. Default 0 for backward compatibility. */
    s_cfg.spectrum_led_source = 0;

    /* Clock-face update indicator — opt-in (off by default) */
    s_cfg.notify_update_on_display = false;

    /* Per-tube colour-inversion mask (0 = all normal; set bit N for replacement
     * panels that default to INVON, e.g. LH096NT-IF09W variants) */
    s_cfg.lcd_invert_mask = 0;
    /* Per-tube panel profile: 0=Standard (original panels), 1=Vivid (ST7735S
     * replacements that appear washed at Standard VCOM/gamma). */
    memset(s_cfg.lcd_init_profile, 0, sizeof(s_cfg.lcd_init_profile));
    /* Per-tube VCOM voltage (VMCTR1 register, 0x00–0x3F).
     * 0x0E (14) = original Nextube Standard value; 0x3C (60) = Vivid preset.
     * Higher VCOM raises AC driving voltage, restoring contrast on replacement panels.
     * Independent of profile — allows fine-tuning within a chosen gamma curve. */
    for (int i = 0; i < 6; i++) s_cfg.lcd_vcom[i] = 0x0E;
    /* Per-tube software gamma exponent — 1.0 = identity (no correction). */
    for (int i = 0; i < 6; i++) s_cfg.lcd_gamma[i] = 1.0f;
    /* Per-tube window offset fine-tuning (all zero = stock ST7735 alignment).
     * Replacement panels based on ST7735S variants typically need col_offset=+2,
     * row_offset=+1 to avoid exposing uninitialized frame-buffer pixels at the
     * right/bottom edge.  User-configurable via Display Settings > Panel Correction. */
    memset(s_cfg.lcd_col_offset, 0, sizeof(s_cfg.lcd_col_offset));
    memset(s_cfg.lcd_row_offset, 0, sizeof(s_cfg.lcd_row_offset));
    /* Per-tube software brightness (100 = full, no pixel scaling).
     * Replacement panels are often brighter than the originals; reduce to match. */
    for (int i = 0; i < 6; i++) s_cfg.lcd_tube_brightness[i] = 100;

    /* Default rainbow-ish backlight colours */
    uint8_t defaults[6][3] = {
        {200,0,0}, {0,200,0}, {0,0,200},
        {110,100,0}, {0,200,200}, {200,0,200}
    };
    memcpy(s_cfg.backlight_rgb, defaults, sizeof(defaults));

    strncpy(s_cfg.hostname, "nextube-remaster", sizeof(s_cfg.hostname) - 1);
    s_cfg.static_ip_enabled = false;   /* DHCP by default */
    strncpy(s_cfg.timezone, "UTC0", sizeof(s_cfg.timezone) - 1);
    strncpy(s_cfg.ntp_servers[0], "0.pool.ntp.org", sizeof(s_cfg.ntp_servers[0]) - 1);
    strncpy(s_cfg.ntp_servers[1], "1.pool.ntp.org", sizeof(s_cfg.ntp_servers[1]) - 1);
    strncpy(s_cfg.ntp_servers[2], "2.pool.ntp.org", sizeof(s_cfg.ntp_servers[2]) - 1);
    strncpy(s_cfg.ntp_servers[3], "3.pool.ntp.org", sizeof(s_cfg.ntp_servers[3]) - 1);
    s_cfg.time_discipline_mode = 2;   /* PCF8563 slave — best between-sync accuracy (recommended) */

    strncpy(s_cfg.weather_source, "metno", sizeof(s_cfg.weather_source) - 1); /* default: free, no API key needed */
    strncpy(s_cfg.weather_api_key, "", sizeof(s_cfg.weather_api_key) - 1);
    s_cfg.weather_ext_lat       = 0.0f;
    s_cfg.weather_ext_lon       = 0.0f;
    s_cfg.weather_ext_loc_valid = false;
    strncpy(s_cfg.city, "", sizeof(s_cfg.city) - 1);
    strncpy(s_cfg.temp_format, "Celsius", sizeof(s_cfg.temp_format) - 1);
    strncpy(s_cfg.wind_unit,   "km/h",    sizeof(s_cfg.wind_unit)   - 1);
    strncpy(s_cfg.date_format, "DD/MM/YY", sizeof(s_cfg.date_format) - 1);
    strncpy(s_cfg.language,    "en",       sizeof(s_cfg.language)    - 1);

    strncpy(s_cfg.video_site, "youtube", sizeof(s_cfg.video_site) - 1);
    strncpy(s_cfg.youtube_key, "", sizeof(s_cfg.youtube_key) - 1);
    strncpy(s_cfg.bili_uid, "1", sizeof(s_cfg.bili_uid) - 1);

    strncpy(s_cfg.click_file, "/spiffs/audio/click.wav", sizeof(s_cfg.click_file) - 1);
    s_cfg.button_sound  = true;
    strncpy(s_cfg.ticker_file, "/spiffs/audio/bell.wav", sizeof(s_cfg.ticker_file) - 1);
    s_cfg.ticker_sound  = false;   /* opt-in: chime when MQTT/HA ticker text arrives */
    s_cfg.audio_enabled = false;   /* off by default — amp is always powered,
                                    * user opts in via Settings > Audio */
    s_cfg.volume = 20;
    s_cfg.mic_enabled      = true;
    s_cfg.mic_adc_channel  = 7;      /* ADC1_CH7 = GPIO35 — confirmed via hardware debug */
    s_cfg.mic_silence_gate = 25.0f; /* SPECTRAL gate (sum of post-floor band
                                     * power): silence ≈ <10, quiet real audio
                                     * >50.  NOTE: semantics changed from the
                                     * old time-domain RMS² gate (default 250)
                                     * — users upgrading should re-tune the
                                     * noise-floor slider, starting at ~25. */
    memset(s_cfg.mic_noise_floor, 0, sizeof(s_cfg.mic_noise_floor));
    s_cfg.mic_calibration_saved = false;
    s_cfg.sht30_temp_offset = 0.0f;    /* no correction by default */

    /* Background-feature toggles (boot-time gates).  Default true so
     * existing behaviour is preserved on upgrade. */
    s_cfg.weather_enabled = true;
    s_cfg.update_check_enabled = true;
    s_cfg.social_enabled           = false; /* opt-in: user must enable explicitly */
    s_cfg.youtube_enabled          = false; /* opt-in: user must enable explicitly */
    s_cfg.sub_poll_interval_min    = 60;   /* 1 hour default */
    s_cfg.instagram_enabled        = false;
    s_cfg.tiktok_enabled    = false;
    s_cfg.instagram_user[0] = '\0';
    strncpy(s_cfg.instagram_method, "internal", sizeof(s_cfg.instagram_method) - 1);
    s_cfg.tiktok_user[0]          = '\0';
    s_cfg.tiktok_key[0]           = '\0';
    s_cfg.tiktok_relay_host[0]    = '\0';
    s_cfg.mastodon_enabled        = false;
    s_cfg.mastodon_user[0]        = '\0';
    s_cfg.mastodon_instance[0]    = '\0';
    s_cfg.mdns_enabled            = true;

    /* MQTT / Home Assistant */
    s_cfg.mqtt_enabled      = false;
    s_cfg.mqtt_broker[0]    = '\0';
    s_cfg.mqtt_port         = 1883;
    s_cfg.mqtt_user[0]      = '\0';
    s_cfg.mqtt_password[0]  = '\0';
    s_cfg.mqtt_ha_discovery = true;
    s_cfg.mqtt_pub_ntp      = false;  /* clock telemetry — opt-in              */
    s_cfg.mqtt_pub_health   = false;  /* RSSI/heap/uptime each 60 s — opt-in   */
    s_cfg.mqtt_pub_buttons  = false;  /* touch presses as HA triggers — opt-in */

    /* WLED Sync */
    s_cfg.wled_sync_enabled = false;
    s_cfg.wled_sync_port    = 21324;

    s_cfg.album_switch_ms   = 2000;
    s_cfg.album_shuffle     = false;
    s_cfg.weather_panel_ms  = 5000;  /* 5 s between temp and humidity panels */
    s_cfg.weather_panel0_en = true;   /* temperature panel on by default */
    s_cfg.weather_panel1_en = true;   /* humidity panel on by default */
    s_cfg.weather_panel2_en = false;  /* sunrise/sunset panel off by default */
    s_cfg.weather_panel3_en = false;  /* wind speed panel off by default */
    s_cfg.weather_panel4_en = false;  /* Hi/Lo panel off by default */

    /* 24H Custom — tube 6 panel rotation */
    s_cfg.tube6_panel_weather  = false;
    s_cfg.tube6_panel_weekdate = true;
    s_cfg.tube6_panel_ht       = true;
    s_cfg.tube6_panel_temp     = false;
    s_cfg.tube6_panel_sunrise  = false;
    s_cfg.tube6_panel_push     = false;
    s_cfg.tube6_panel_humidity = false;
    s_cfg.tube6_panel_wind     = false;
    s_cfg.tube6_panel_aqi      = false;
    s_cfg.tube6_panel_outdoor_ht = false;
    s_cfg.tube6_panel_ms       = 5000;
    s_cfg.cx_dual_panel        = false;   /* single panel + colon (original layout) */
    s_cfg.tube5_panel_weather  = false;
    s_cfg.tube5_panel_weekdate = false;
    s_cfg.tube5_panel_ht       = true;    /* sensible distinct default vs tube 6 */
    s_cfg.tube5_panel_temp     = false;
    s_cfg.tube5_panel_sunrise  = false;
    s_cfg.tube5_panel_push     = false;
    s_cfg.tube5_panel_humidity = false;
    s_cfg.tube5_panel_wind     = false;
    s_cfg.tube5_panel_aqi      = false;
    s_cfg.tube5_panel_outdoor_ht = false;
    strncpy(s_cfg.aqi_standard, "auto", sizeof(s_cfg.aqi_standard) - 1);
    s_cfg.update_repo[0]       = '\0';

    /* Rotation off by default; user must explicitly enable it */
    s_cfg.rotation_enabled    = false;
    s_cfg.rotation_interval_s = 60;
    s_cfg.rotation_modes      = 0;    /* 0 = cycle all enabled modes */
    for (int i = 0; i < APP_MODE_MAX; i++) s_cfg.rotation_weights[i] = 1;

    /* Theme rotation off by default; 0 count = all installed themes */
    s_cfg.theme_rotation_enabled    = false;
    s_cfg.theme_rotation_interval_s = 300;   /* 5 minutes */
    s_cfg.theme_rotation_count      = 0;
    memset(s_cfg.theme_rotation_themes, 0, sizeof(s_cfg.theme_rotation_themes));

    /* Scheduled burn-in — off by default */
    s_cfg.burnin_auto_enabled    = false;
    s_cfg.burnin_auto_mask       = 0x3F;   /* all 6 tubes */
    s_cfg.burnin_auto_duration_s = 3600;   /* 1 hour */
    strncpy(s_cfg.burnin_auto_interval, "weekly", sizeof(s_cfg.burnin_auto_interval) - 1);
    s_cfg.burnin_auto_hour       = 0;      /* midnight */
    strncpy(s_cfg.burnin_auto_mode, "colour-cycle", sizeof(s_cfg.burnin_auto_mode) - 1);
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

static void json_read_float(cJSON *root, const char *key, float *dst)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(item)) *dst = (float)item->valuedouble;
}

/* Parse a 6-element per-tube integer array from a JSON key.
 * Values are clamped to [lo, hi] and cast to T via the caller's pointer.
 * Avoids six near-identical copy-pasted blocks for lcd_* per-tube fields. */
#define JSON_READ_TUBE_INT_ARRAY(root, key, field, lo, hi, T) do {             \
    cJSON *_arr = cJSON_GetObjectItem((root), (key));                           \
    if (cJSON_IsArray(_arr)) {                                                  \
        int _cnt = cJSON_GetArraySize(_arr); if (_cnt > 6) _cnt = 6;           \
        for (int _i = 0; _i < _cnt; _i++) {                                    \
            cJSON *_v = cJSON_GetArrayItem(_arr, _i);                           \
            if (cJSON_IsNumber(_v)) {                                           \
                int _val = _v->valueint;                                        \
                if (_val < (lo)) _val = (lo);                                   \
                if (_val > (hi)) _val = (hi);                                   \
                s_cfg.field[_i] = (T)_val;                                     \
            }                                                                   \
        }                                                                       \
    }                                                                           \
} while (0)

/* Serialise a 6-element per-tube uint8_t array to a JSON array. */
static void json_add_tube_u8(cJSON *root, const char *key, const uint8_t *v)
{
    cJSON *arr = cJSON_AddArrayToObject(root, key);
    for (int i = 0; i < 6; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(v[i]));
}

/* Serialise a 6-element per-tube int8_t array to a JSON array. */
static void json_add_tube_i8(cJSON *root, const char *key, const int8_t *v)
{
    cJSON *arr = cJSON_AddArrayToObject(root, key);
    for (int i = 0; i < 6; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(v[i]));
}

static void parse_json(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        const char *errp = cJSON_GetErrorPtr();
        long offset = (errp && errp >= json) ? (long)(errp - json) : -1L;
        ESP_LOGE(TAG, "CONFIG PARSE FAILED — keeping defaults. len=%u offset=%ld near: \"%.40s\"",
                 (unsigned)len, offset,
                 (offset >= 0 && (size_t)offset < len) ? json + offset : "");
        return;
    }
    ESP_LOGI(TAG, "config JSON parsed OK (%u B)", (unsigned)len);

    /* Mode */
    cJSON *apps = cJSON_GetObjectItem(root, "apps");
    if (cJSON_IsArray(apps) && cJSON_GetArraySize(apps) > 0) {
        cJSON *app0 = cJSON_GetArrayItem(apps, 0);
        char app_name[32] = {0};
        json_read_str(app0, "app", app_name, sizeof(app_name));
        if      (strcmp(app_name, "Clock")      == 0) s_cfg.current_mode = APP_MODE_CLOCK;
        else if (strcmp(app_name, "Countdown")  == 0) s_cfg.current_mode = APP_MODE_CLOCK; /* removed mode → fall back to Clock */
        else if (strcmp(app_name, "Scoreboard")  == 0) s_cfg.current_mode = APP_MODE_CLOCK; /* removed mode → fall back to Clock */
        else if (strcmp(app_name, "Pomodoro")    == 0) s_cfg.current_mode = APP_MODE_CLOCK; /* removed mode → fall back to Clock */
        else if (strcmp(app_name, "YouTube")     == 0) s_cfg.current_mode = APP_MODE_YOUTUBE;
        else if (strcmp(app_name, "Date")        == 0) s_cfg.current_mode = APP_MODE_DATE;
        else if (strcmp(app_name, "CustomClock") == 0) s_cfg.current_mode = APP_MODE_DATE; /* legacy alias */
        else if (strcmp(app_name, "Album")       == 0) s_cfg.current_mode = APP_MODE_ALBUM;
        else if (strcmp(app_name, "Weather")     == 0) s_cfg.current_mode = APP_MODE_WEATHER;
        else if (strcmp(app_name, "Spectrum")    == 0) s_cfg.current_mode = APP_MODE_SPECTRUM;
        else if (strcmp(app_name, "Instagram")   == 0) s_cfg.current_mode = APP_MODE_INSTAGRAM;
        else if (strcmp(app_name, "TikTok")      == 0) s_cfg.current_mode = APP_MODE_TIKTOK;
        else if (strcmp(app_name, "Mastodon")    == 0) s_cfg.current_mode = APP_MODE_MASTODON;

        json_read_str(app0, "theme", s_cfg.theme, sizeof(s_cfg.theme));
        /* Removed baked-JPEG themes → migrate to their procedural
         * successor rather than leaving a config pointed at assets that no
         * longer exist on disk. */
        if (strcmp(s_cfg.theme, "DotMatrixRG") == 0 || strcmp(s_cfg.theme, "DotMatrixY") == 0) {
            strncpy(s_cfg.theme, "DotMatrix", sizeof(s_cfg.theme) - 1);
            s_cfg.theme[sizeof(s_cfg.theme) - 1] = '\0';
        }
        json_read_str(app0, "type",  s_cfg.time_type, sizeof(s_cfg.time_type));
        json_read_str(app0, "clock_tube5", s_cfg.clock_tube5, sizeof(s_cfg.clock_tube5));
        if (s_cfg.clock_tube5[0] == '\0') strncpy(s_cfg.clock_tube5, "blank", sizeof(s_cfg.clock_tube5) - 1);
    }

    json_read_str(root, "ssid",             s_cfg.ssid,            sizeof(s_cfg.ssid));
    json_read_str(root, "password",         s_cfg.password,        sizeof(s_cfg.password));
    json_read_str(root, "video_site",       s_cfg.video_site,      sizeof(s_cfg.video_site));
    json_read_str(root, "youtube_id",       s_cfg.youtube_id,      sizeof(s_cfg.youtube_id));
    json_read_str(root, "youtube_key",      s_cfg.youtube_key,     sizeof(s_cfg.youtube_key));
    json_read_str(root, "bili_uid",         s_cfg.bili_uid,        sizeof(s_cfg.bili_uid));
    json_read_str(root, "weather_source",   s_cfg.weather_source,  sizeof(s_cfg.weather_source));
    json_read_str(root, "weather_api_key",  s_cfg.weather_api_key, sizeof(s_cfg.weather_api_key));
    json_read_float(root, "weather_ext_lat", &s_cfg.weather_ext_lat);
    json_read_float(root, "weather_ext_lon", &s_cfg.weather_ext_lon);
    {
        cJSON *v = cJSON_GetObjectItem(root, "weather_ext_loc_valid");
        if (cJSON_IsBool(v)) s_cfg.weather_ext_loc_valid = cJSON_IsTrue(v);
    }
    json_read_str(root, "City",             s_cfg.city,            sizeof(s_cfg.city));
    /* Accept the old misspelled key first, then the corrected one so that
     * new configs with the fixed key take precedence over legacy files. */
    json_read_str(root, "temperature_formate", s_cfg.temp_format, sizeof(s_cfg.temp_format));
    json_read_str(root, "temperature_format",  s_cfg.temp_format, sizeof(s_cfg.temp_format));
    json_read_str(root, "wind_unit",           s_cfg.wind_unit,   sizeof(s_cfg.wind_unit));
    json_read_str(root, "date_format",         s_cfg.date_format, sizeof(s_cfg.date_format));
    json_read_str(root, "language",            s_cfg.language,    sizeof(s_cfg.language));
    json_read_str(root, "click_file",       s_cfg.click_file,      sizeof(s_cfg.click_file));
    json_read_str(root, "ticker_file",      s_cfg.ticker_file,     sizeof(s_cfg.ticker_file));
    json_read_str(root, "hostname",        s_cfg.hostname,        sizeof(s_cfg.hostname));
    {
        cJSON *v = cJSON_GetObjectItem(root, "static_ip_enabled");
        if (cJSON_IsBool(v)) s_cfg.static_ip_enabled = cJSON_IsTrue(v);
    }
    json_read_str(root, "static_ip",       s_cfg.static_ip,       sizeof(s_cfg.static_ip));
    json_read_str(root, "static_netmask",  s_cfg.static_netmask,  sizeof(s_cfg.static_netmask));
    json_read_str(root, "static_gateway",  s_cfg.static_gateway,  sizeof(s_cfg.static_gateway));
    json_read_str(root, "static_dns1",     s_cfg.static_dns1,     sizeof(s_cfg.static_dns1));
    json_read_str(root, "static_dns2",     s_cfg.static_dns2,     sizeof(s_cfg.static_dns2));
    {
        cJSON *bs = cJSON_GetObjectItem(root, "button_sound");
        if (cJSON_IsBool(bs)) s_cfg.button_sound = cJSON_IsTrue(bs);
    }
    {
        cJSON *ts = cJSON_GetObjectItem(root, "ticker_sound");
        if (cJSON_IsBool(ts)) s_cfg.ticker_sound = cJSON_IsTrue(ts);
    }
    {
        cJSON *ae = cJSON_GetObjectItem(root, "audio_enabled");
        if (cJSON_IsBool(ae)) s_cfg.audio_enabled = cJSON_IsTrue(ae);
    }
    {
        cJSON *me = cJSON_GetObjectItem(root, "mic_enabled");
        if (cJSON_IsBool(me)) s_cfg.mic_enabled = cJSON_IsTrue(me);
    }
    /* Background-feature toggles — default true if key absent (preserves
     * behaviour for existing devices upgrading from older firmware). */
    {
        cJSON *we = cJSON_GetObjectItem(root, "weather_enabled");
        if (cJSON_IsBool(we)) s_cfg.weather_enabled = cJSON_IsTrue(we);
    }
    {
        cJSON *uce = cJSON_GetObjectItem(root, "update_check_enabled");
        if (cJSON_IsBool(uce)) s_cfg.update_check_enabled = cJSON_IsTrue(uce);
    }
    {
        cJSON *se = cJSON_GetObjectItem(root, "social_enabled");
        if (cJSON_IsBool(se)) s_cfg.social_enabled = cJSON_IsTrue(se);
    }
    {
        cJSON *ye = cJSON_GetObjectItem(root, "youtube_enabled");
        if (cJSON_IsBool(ye)) s_cfg.youtube_enabled = cJSON_IsTrue(ye);
        cJSON *pi = cJSON_GetObjectItem(root, "sub_poll_interval_min");
        if (cJSON_IsNumber(pi) && pi->valueint >= 5)
            s_cfg.sub_poll_interval_min = (uint16_t)pi->valueint;
    }
    {
        cJSON *v = cJSON_GetObjectItem(root, "instagram_enabled");
        if (cJSON_IsBool(v)) s_cfg.instagram_enabled = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tiktok_enabled");
        if (cJSON_IsBool(v)) s_cfg.tiktok_enabled = cJSON_IsTrue(v);
        json_read_str(root, "instagram_user",   s_cfg.instagram_user,   sizeof(s_cfg.instagram_user));
        json_read_str(root, "instagram_method", s_cfg.instagram_method, sizeof(s_cfg.instagram_method));
        if (s_cfg.instagram_method[0] == '\0')
            strncpy(s_cfg.instagram_method, "internal", sizeof(s_cfg.instagram_method) - 1);
        json_read_str(root, "tiktok_user",         s_cfg.tiktok_user,         sizeof(s_cfg.tiktok_user));
        json_read_str(root, "tiktok_key",          s_cfg.tiktok_key,          sizeof(s_cfg.tiktok_key));
        json_read_str(root, "tiktok_relay_host",   s_cfg.tiktok_relay_host,   sizeof(s_cfg.tiktok_relay_host));
        v = cJSON_GetObjectItem(root, "mastodon_enabled");
        if (cJSON_IsBool(v)) s_cfg.mastodon_enabled = cJSON_IsTrue(v);
        json_read_str(root, "mastodon_user",     s_cfg.mastodon_user,     sizeof(s_cfg.mastodon_user));
        json_read_str(root, "mastodon_instance", s_cfg.mastodon_instance, sizeof(s_cfg.mastodon_instance));
    }
    {
        cJSON *de = cJSON_GetObjectItem(root, "mdns_enabled");
        if (cJSON_IsBool(de)) s_cfg.mdns_enabled = cJSON_IsTrue(de);
    }
    {
        cJSON *v = cJSON_GetObjectItem(root, "mqtt_enabled");
        if (cJSON_IsBool(v)) s_cfg.mqtt_enabled = cJSON_IsTrue(v);
        json_read_str(root, "mqtt_broker",   s_cfg.mqtt_broker,   sizeof(s_cfg.mqtt_broker));
        json_read_u16(root, "mqtt_port",     &s_cfg.mqtt_port);
        if (s_cfg.mqtt_port == 0) s_cfg.mqtt_port = 1883;
        json_read_str(root, "mqtt_user",     s_cfg.mqtt_user,     sizeof(s_cfg.mqtt_user));
        json_read_str(root, "mqtt_password", s_cfg.mqtt_password, sizeof(s_cfg.mqtt_password));
        v = cJSON_GetObjectItem(root, "mqtt_ha_discovery");
        if (cJSON_IsBool(v)) s_cfg.mqtt_ha_discovery = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "mqtt_pub_ntp");
        if (cJSON_IsBool(v)) s_cfg.mqtt_pub_ntp = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "mqtt_pub_health");
        if (cJSON_IsBool(v)) s_cfg.mqtt_pub_health = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "mqtt_pub_buttons");
        if (cJSON_IsBool(v)) s_cfg.mqtt_pub_buttons = cJSON_IsTrue(v);
    }
    {
        cJSON *v = cJSON_GetObjectItem(root, "wled_sync_enabled");
        if (cJSON_IsBool(v)) s_cfg.wled_sync_enabled = cJSON_IsTrue(v);
        json_read_u16(root, "wled_sync_port", &s_cfg.wled_sync_port);
        if (s_cfg.wled_sync_port == 0) s_cfg.wled_sync_port = 21324;
    }
    json_read_u8(root, "mic_adc_channel", &s_cfg.mic_adc_channel);
    if (s_cfg.mic_adc_channel > 7) s_cfg.mic_adc_channel = 0; /* clamp to valid ADC1 range */
    json_read_float(root, "mic_silence_gate", &s_cfg.mic_silence_gate);
    if (s_cfg.mic_silence_gate < 0.0f) s_cfg.mic_silence_gate = 0.0f; /* no negative gate */
    /* mic noise floor — 24-element float array saved by "Capture Baseline" */
    {
        cJSON *mf = cJSON_GetObjectItem(root, "mic_noise_floor");
        if (cJSON_IsArray(mf) && cJSON_GetArraySize(mf) >= CFG_MIC_BAND_COUNT) {
            for (int i = 0; i < CFG_MIC_BAND_COUNT; i++) {
                cJSON *v = cJSON_GetArrayItem(mf, i);
                if (cJSON_IsNumber(v))
                    s_cfg.mic_noise_floor[i] = (float)v->valuedouble;
            }
        }
    }
    {
        cJSON *mc = cJSON_GetObjectItem(root, "mic_calibration_saved");
        if (cJSON_IsBool(mc)) s_cfg.mic_calibration_saved = cJSON_IsTrue(mc);
    }
    json_read_float(root, "sht30_temp_offset", &s_cfg.sht30_temp_offset);
    /* clamp to ±20 °C — large values indicate a misconfiguration */
    if (s_cfg.sht30_temp_offset >  20.0f) s_cfg.sht30_temp_offset =  20.0f;
    if (s_cfg.sht30_temp_offset < -20.0f) s_cfg.sht30_temp_offset = -20.0f;
    {
        cJSON *lz = cJSON_GetObjectItem(root, "leading_zero");
        if (cJSON_IsBool(lz)) s_cfg.leading_zero = cJSON_IsTrue(lz);
    }

    /* NTP servers — new array format, with fallback to legacy single key */
    {
        cJSON *arr = cJSON_GetObjectItem(root, "ntp_servers");
        if (cJSON_IsArray(arr)) {
            int cnt = cJSON_GetArraySize(arr);
            if (cnt > 4) cnt = 4;
            for (int i = 0; i < cnt; i++) {
                cJSON *it = cJSON_GetArrayItem(arr, i);
                if (cJSON_IsString(it) && it->valuestring)
                    strncpy(s_cfg.ntp_servers[i], it->valuestring,
                            sizeof(s_cfg.ntp_servers[i]) - 1);
            }
        } else {
            /* Legacy: single "ntp_server" key → slot 0 */
            json_read_str(root, "ntp_server", s_cfg.ntp_servers[0],
                          sizeof(s_cfg.ntp_servers[0]));
        }
        json_read_u8(root, "time_discipline_mode", &s_cfg.time_discipline_mode);
        if (s_cfg.time_discipline_mode > 2) s_cfg.time_discipline_mode = 0;
    }
    /* Timezone — POSIX TZ string; migrate from legacy numeric time_zone if absent */
    {
        cJSON *tz = cJSON_GetObjectItem(root, "timezone");
        if (cJSON_IsString(tz) && tz->valuestring && tz->valuestring[0]) {
            strncpy(s_cfg.timezone, tz->valuestring, sizeof(s_cfg.timezone) - 1);
            s_cfg.timezone[sizeof(s_cfg.timezone) - 1] = '\0';
        } else {
            /* Legacy: numeric time_zone (seconds if |v|>24, else hours) → UTC±H:MM */
            cJSON *old = cJSON_GetObjectItem(root, "time_zone");
            if (cJSON_IsNumber(old)) {
                double v = old->valuedouble;
                int32_t secs = (v > 24.0 || v < -24.0) ? (int32_t)v
                                                        : (int32_t)(v * 3600.0);
                int hrs  = secs / 3600;
                int mins = abs((secs % 3600) / 60);
                /* POSIX sign is inverted vs conventional: UTC-6 → "UTC+6" */
                snprintf(s_cfg.timezone, sizeof(s_cfg.timezone),
                         "UTC%+d:%02d", -hrs, mins);
            }
        }
    }

    json_read_u8(root, "volume",         &s_cfg.volume);
    if (s_cfg.volume > 100) s_cfg.volume = 100;
    json_read_u8(root, "led_brightness", &s_cfg.led_brightness);
    if (s_cfg.led_brightness > 100) s_cfg.led_brightness = 100;
    json_read_u8(root, "led_effect_speed", &s_cfg.led_effect_speed);
    if (s_cfg.led_effect_speed < 1)  s_cfg.led_effect_speed = 1;
    if (s_cfg.led_effect_speed > 10) s_cfg.led_effect_speed = 10;
    {
        cJSON *lwo = cJSON_GetObjectItem(root, "led_weather_override");
        if (cJSON_IsBool(lwo)) s_cfg.led_weather_override = cJSON_IsTrue(lwo);
        cJSON *wla = cJSON_GetObjectItem(root, "wlive_animate");
        if (cJSON_IsBool(wla)) s_cfg.wlive_animate = cJSON_IsTrue(wla);
    }
    json_read_str(root, "clock_face", s_cfg.clock_face, sizeof(s_cfg.clock_face));
    json_read_str(root, "custom_bg",  s_cfg.custom_bg,  sizeof(s_cfg.custom_bg));
    if (s_cfg.custom_bg[0] == '\0') strncpy(s_cfg.custom_bg, "WeatherLive", sizeof(s_cfg.custom_bg) - 1);
    json_read_str(root, "custom_bg_fill", s_cfg.custom_bg_fill, sizeof(s_cfg.custom_bg_fill));
    if (s_cfg.custom_bg_fill[0] == '\0') strncpy(s_cfg.custom_bg_fill, "solid", sizeof(s_cfg.custom_bg_fill) - 1);
    {
        cJSON *v = cJSON_GetObjectItem(root, "custom_shadow");
        if (cJSON_IsBool(v)) s_cfg.custom_shadow = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "custom_night_colors");
        if (cJSON_IsBool(v)) s_cfg.custom_night_colors = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "custom_shadow_night");
        if (cJSON_IsBool(v)) s_cfg.custom_shadow_night = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "custom_glyph_shadow");
        bool have_glyph_shadow = cJSON_IsBool(v);
        if (have_glyph_shadow) s_cfg.custom_glyph_shadow = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "custom_glyph_shadow_night");
        bool have_glyph_shadow_night = cJSON_IsBool(v);
        if (have_glyph_shadow_night) s_cfg.custom_glyph_shadow_night = cJSON_IsTrue(v);
        /* Migration: configs saved before the font/digit shadow split have no
         * custom_glyph_shadow* keys at all. Rather than leaving the glyph
         * side at the hardcoded default, inherit whatever the user had
         * already tuned for the (formerly shared) shadow — preserves their
         * prior look instead of silently resetting the digit shadow to
         * black. Only applies once: once custom_glyph_shadow is saved even
         * a single time, the key exists from then on and this is skipped. */
        if (!have_glyph_shadow) s_cfg.custom_glyph_shadow = s_cfg.custom_shadow;
        if (!have_glyph_shadow_night) s_cfg.custom_glyph_shadow_night = s_cfg.custom_shadow_night;
    }
    json_read_str(root, "custom_font", s_cfg.custom_font, sizeof(s_cfg.custom_font));
    {
        bool have_glyph_shadow_color       = cJSON_IsArray(cJSON_GetObjectItem(root, "custom_glyph_shadow_color"));
        bool have_glyph_shadow_color_night = cJSON_IsArray(cJSON_GetObjectItem(root, "custom_glyph_shadow_color_night"));
        static const char *const color_keys[12] = { "custom_font_color", "custom_glyph_color", "custom_shadow_color",
                                                    "dm_on_color", "dm_off_color",
                                                    "custom_bg_color1", "custom_bg_color2",
                                                    "custom_font_color_night", "custom_glyph_color_night",
                                                    "custom_shadow_color_night",
                                                    "custom_glyph_shadow_color", "custom_glyph_shadow_color_night" };
        uint8_t *const color_ptrs[12] = { s_cfg.custom_font_color, s_cfg.custom_glyph_color, s_cfg.custom_shadow_color,
                                          s_cfg.dm_on_color, s_cfg.dm_off_color,
                                          s_cfg.custom_bg_color1, s_cfg.custom_bg_color2,
                                          s_cfg.custom_font_color_night, s_cfg.custom_glyph_color_night,
                                          s_cfg.custom_shadow_color_night,
                                          s_cfg.custom_glyph_shadow_color, s_cfg.custom_glyph_shadow_color_night };
        for (int ci = 0; ci < 12; ci++) {
            cJSON *arr = cJSON_GetObjectItem(root, color_keys[ci]);
            if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) >= 3) {
                for (int ch = 0; ch < 3; ch++) {
                    cJSON *c = cJSON_GetArrayItem(arr, ch);
                    if (cJSON_IsNumber(c) && c->valueint >= 0 && c->valueint <= 255)
                        color_ptrs[ci][ch] = (uint8_t)c->valueint;
                }
            }
        }
        /* Same migration as above, for the shadow *color* (see comment there). */
        if (!have_glyph_shadow_color)
            memcpy(s_cfg.custom_glyph_shadow_color, s_cfg.custom_shadow_color, 3);
        if (!have_glyph_shadow_color_night)
            memcpy(s_cfg.custom_glyph_shadow_color_night, s_cfg.custom_shadow_color_night, 3);
    }
    json_read_u8(root, "lcd_brightness", &s_cfg.lcd_brightness);
    if (s_cfg.lcd_brightness > 100) s_cfg.lcd_brightness = 100;
    {
        cJSON *ab = cJSON_GetObjectItem(root, "auto_brightness");
        if (cJSON_IsBool(ab)) s_cfg.auto_brightness = cJSON_IsTrue(ab);
    }
    json_read_u8(root, "night_brightness", &s_cfg.night_brightness);
    if (s_cfg.night_brightness > 100) s_cfg.night_brightness = 100;
    json_read_u8(root, "led_night_brightness", &s_cfg.led_night_brightness);
    if (s_cfg.led_night_brightness > 100) s_cfg.led_night_brightness = 100;
    json_read_u8(root, "night_start_hour", &s_cfg.night_start_hour);
    if (s_cfg.night_start_hour > 23) s_cfg.night_start_hour = 23;
    json_read_u8(root, "night_end_hour",   &s_cfg.night_end_hour);
    if (s_cfg.night_end_hour > 23) s_cfg.night_end_hour = 23;

    json_read_u16(root, "album_switch_time",      &s_cfg.album_switch_ms);
    if (s_cfg.album_switch_ms < 500) s_cfg.album_switch_ms = 2000; /* 0/tiny would thrash SPIFFS JPEG reads every frame */
    { cJSON *v = cJSON_GetObjectItem(root, "album_shuffle");
      if (cJSON_IsBool(v)) s_cfg.album_shuffle = cJSON_IsTrue(v); }
    json_read_u16(root, "weather_panel_ms",       &s_cfg.weather_panel_ms);
    if (s_cfg.weather_panel_ms < 1000) s_cfg.weather_panel_ms = 5000; /* resets to default 5 s if below 1 s */
    /* Panel enable flags — guard: at least one of p0/p1 must be on.
     * Use the conditional-update pattern (same as tube6_panel_* flags) so that
     * absent keys preserve the value set by set_defaults() rather than
     * unconditionally overwriting it.  This prevents a partial-payload
     * config_set_json call (or an old config.json that pre-dates weather_panel2_en)
     * from resetting panel2 to false on every parse. */
    {
        cJSON *p0 = cJSON_GetObjectItem(root, "weather_panel0_en");
        cJSON *p1 = cJSON_GetObjectItem(root, "weather_panel1_en");
        cJSON *p2 = cJSON_GetObjectItem(root, "weather_panel2_en");
        cJSON *p3 = cJSON_GetObjectItem(root, "weather_panel3_en");
        cJSON *p4 = cJSON_GetObjectItem(root, "weather_panel4_en");
        if (cJSON_IsBool(p0)) s_cfg.weather_panel0_en = cJSON_IsTrue(p0);
        if (cJSON_IsBool(p1)) s_cfg.weather_panel1_en = cJSON_IsTrue(p1);
        if (cJSON_IsBool(p2)) s_cfg.weather_panel2_en = cJSON_IsTrue(p2);
        if (cJSON_IsBool(p3)) s_cfg.weather_panel3_en = cJSON_IsTrue(p3);
        if (cJSON_IsBool(p4)) s_cfg.weather_panel4_en = cJSON_IsTrue(p4);
    }
    if (!s_cfg.weather_panel0_en && !s_cfg.weather_panel1_en &&
        !s_cfg.weather_panel2_en && !s_cfg.weather_panel3_en &&
        !s_cfg.weather_panel4_en)
        s_cfg.weather_panel0_en = true;

    /* 24H Custom — tube 6 panel rotation */
    {
        cJSON *v;
        v = cJSON_GetObjectItem(root, "tube6_panel_weather");
        if (cJSON_IsBool(v)) s_cfg.tube6_panel_weather = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube6_panel_weekdate");
        if (cJSON_IsBool(v)) s_cfg.tube6_panel_weekdate = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube6_panel_ht");
        if (cJSON_IsBool(v)) s_cfg.tube6_panel_ht = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube6_panel_temp");
        if (cJSON_IsBool(v)) s_cfg.tube6_panel_temp = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube6_panel_sunrise");
        if (cJSON_IsBool(v)) s_cfg.tube6_panel_sunrise = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube6_panel_push");
        if (cJSON_IsBool(v)) s_cfg.tube6_panel_push = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube6_panel_humidity");
        if (cJSON_IsBool(v)) s_cfg.tube6_panel_humidity = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube6_panel_wind");
        if (cJSON_IsBool(v)) s_cfg.tube6_panel_wind = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube6_panel_aqi");
        if (cJSON_IsBool(v)) s_cfg.tube6_panel_aqi = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube6_panel_outdoor_ht");
        if (cJSON_IsBool(v)) s_cfg.tube6_panel_outdoor_ht = cJSON_IsTrue(v);

        /* Dual-panel mode + tube 5's independent panel set */
        v = cJSON_GetObjectItem(root, "cx_dual_panel");
        if (cJSON_IsBool(v)) s_cfg.cx_dual_panel = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube5_panel_weather");
        if (cJSON_IsBool(v)) s_cfg.tube5_panel_weather = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube5_panel_weekdate");
        if (cJSON_IsBool(v)) s_cfg.tube5_panel_weekdate = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube5_panel_ht");
        if (cJSON_IsBool(v)) s_cfg.tube5_panel_ht = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube5_panel_temp");
        if (cJSON_IsBool(v)) s_cfg.tube5_panel_temp = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube5_panel_sunrise");
        if (cJSON_IsBool(v)) s_cfg.tube5_panel_sunrise = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube5_panel_push");
        if (cJSON_IsBool(v)) s_cfg.tube5_panel_push = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube5_panel_humidity");
        if (cJSON_IsBool(v)) s_cfg.tube5_panel_humidity = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube5_panel_wind");
        if (cJSON_IsBool(v)) s_cfg.tube5_panel_wind = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube5_panel_aqi");
        if (cJSON_IsBool(v)) s_cfg.tube5_panel_aqi = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "tube5_panel_outdoor_ht");
        if (cJSON_IsBool(v)) s_cfg.tube5_panel_outdoor_ht = cJSON_IsTrue(v);
    }
    json_read_str(root, "aqi_standard", s_cfg.aqi_standard, sizeof(s_cfg.aqi_standard));
    json_read_u16(root, "tube6_panel_ms", &s_cfg.tube6_panel_ms);
    if (s_cfg.tube6_panel_ms < 1000) s_cfg.tube6_panel_ms = 5000;
    /* Guard: fall back to weekdate if every panel is disabled */
    if (!s_cfg.tube6_panel_weather && !s_cfg.tube6_panel_weekdate &&
        !s_cfg.tube6_panel_ht      && !s_cfg.tube6_panel_temp     &&
        !s_cfg.tube6_panel_sunrise && !s_cfg.tube6_panel_push     &&
        !s_cfg.tube6_panel_humidity && !s_cfg.tube6_panel_wind &&
        !s_cfg.tube6_panel_aqi    && !s_cfg.tube6_panel_outdoor_ht)
        s_cfg.tube6_panel_weekdate = true;
    /* Tube 5 only matters in dual mode — guarantee ≥1 panel there too. */
    if (s_cfg.cx_dual_panel &&
        !s_cfg.tube5_panel_weather && !s_cfg.tube5_panel_weekdate &&
        !s_cfg.tube5_panel_ht      && !s_cfg.tube5_panel_temp     &&
        !s_cfg.tube5_panel_sunrise && !s_cfg.tube5_panel_push     &&
        !s_cfg.tube5_panel_humidity && !s_cfg.tube5_panel_wind &&
        !s_cfg.tube5_panel_aqi    && !s_cfg.tube5_panel_outdoor_ht)
        s_cfg.tube5_panel_ht = true;

    /* Backlight mode */
    char bl_mode[16] = {0};
    json_read_str(root, "backlight_mode", bl_mode, sizeof(bl_mode));
    if      (strcmp(bl_mode, "Static")  == 0) s_cfg.backlight_mode = BL_MODE_STATIC;
    else if (strcmp(bl_mode, "Breath")  == 0) s_cfg.backlight_mode = BL_MODE_BREATH;
    else if (strcmp(bl_mode, "Rainbow") == 0) s_cfg.backlight_mode = BL_MODE_RAINBOW;
    else if (strcmp(bl_mode, "Off")     == 0) s_cfg.backlight_mode = BL_MODE_OFF;
    else if (strcmp(bl_mode, "WLED")    == 0) s_cfg.backlight_mode = BL_MODE_WLED;
    else if (strcmp(bl_mode, "SunMoon") == 0) s_cfg.backlight_mode = BL_MODE_SUNMOON;

    /* sunmoon_sun_RGB / sunmoon_moon_RGB — Follow Sun/Moon mode colours,
     * [R,G,B] arrays following the spectrum_RGB pattern. */
    {
        cJSON *sun_arr = cJSON_GetObjectItem(root, "sunmoon_sun_RGB");
        if (cJSON_IsArray(sun_arr) && cJSON_GetArraySize(sun_arr) >= 3) {
            for (int i = 0; i < 3; i++) {
                cJSON *v = cJSON_GetArrayItem(sun_arr, i);
                if (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint <= 255)
                    s_cfg.sunmoon_sun_rgb[i] = (uint8_t)v->valueint;
            }
        }
    }
    {
        cJSON *moon_arr = cJSON_GetObjectItem(root, "sunmoon_moon_RGB");
        if (cJSON_IsArray(moon_arr) && cJSON_GetArraySize(moon_arr) >= 3) {
            for (int i = 0; i < 3; i++) {
                cJSON *v = cJSON_GetArrayItem(moon_arr, i);
                if (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint <= 255)
                    s_cfg.sunmoon_moon_rgb[i] = (uint8_t)v->valueint;
            }
        }
    }

    char bl_onoff[8] = {0};
    json_read_str(root, "backlight_onoff", bl_onoff, sizeof(bl_onoff));
    /* Only update when the key is actually present in the payload.
     * If absent (e.g. a mode-change-only JSON), bl_onoff stays empty and
     * we must not corrupt the current state: strcmp("","OFF")!=0 would
     * incorrectly force backlight_on = true every time. */
    if (bl_onoff[0] != '\0') {
        s_cfg.backlight_on = (strcmp(bl_onoff, "OFF") != 0);
    }

    /* enabled_modes — uint16_t (was uint8_t); old value 0xFF = 255 parses correctly */
    {
        cJSON *em = cJSON_GetObjectItem(root, "enabled_modes");
        if (cJSON_IsNumber(em)) s_cfg.enabled_modes = (uint16_t)em->valueint;
    }
    /* Clock and Date are independent — both may be enabled simultaneously.
     * Safety fallback: if the user has disabled every time-display mode,
     * re-enable Clock so the device can always show the time. */
    {
        bool has_clock = (s_cfg.enabled_modes & (1 << APP_MODE_CLOCK))        != 0;
        bool has_date  = (s_cfg.enabled_modes & (1 << APP_MODE_DATE)) != 0;
        if (!has_clock && !has_date)
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
    json_read_u16(root, "rotation_modes", &s_cfg.rotation_modes);
    {
        cJSON *wa = cJSON_GetObjectItem(root, "rotation_weights");
        if (cJSON_IsArray(wa)) {
            int n = cJSON_GetArraySize(wa);
            if (n > APP_MODE_MAX)
                ESP_LOGW("cfg", "rotation_weights: %d entries in JSON, only %d used", n, APP_MODE_MAX);
            for (int i = 0; i < n && i < APP_MODE_MAX; i++) {
                cJSON *item = cJSON_GetArrayItem(wa, i);
                if (cJSON_IsNumber(item)) {
                    uint8_t w = (uint8_t)item->valueint;
                    if (w < 1)  w = 1;
                    if (w > 99) w = 99;
                    s_cfg.rotation_weights[i] = w;
                }
            }
        }
    }

    /* Theme rotation */
    {
        cJSON *tr = cJSON_GetObjectItem(root, "theme_rotation_enabled");
        if (cJSON_IsBool(tr)) s_cfg.theme_rotation_enabled = cJSON_IsTrue(tr);
    }
    json_read_u16(root, "theme_rotation_interval_s", &s_cfg.theme_rotation_interval_s);
    if (s_cfg.theme_rotation_interval_s == 0) s_cfg.theme_rotation_interval_s = 300;
    {
        cJSON *ta = cJSON_GetObjectItem(root, "theme_rotation_themes");
        if (cJSON_IsArray(ta)) {
            int cnt = cJSON_GetArraySize(ta);
            if (cnt > 16) cnt = 16;
            s_cfg.theme_rotation_count = 0;
            memset(s_cfg.theme_rotation_themes, 0, sizeof(s_cfg.theme_rotation_themes));
            for (int i = 0; i < cnt; i++) {
                cJSON *v = cJSON_GetArrayItem(ta, i);
                if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
                    strncpy(s_cfg.theme_rotation_themes[s_cfg.theme_rotation_count],
                            v->valuestring, 31);
                    s_cfg.theme_rotation_themes[s_cfg.theme_rotation_count][31] = '\0';
                    s_cfg.theme_rotation_count++;
                }
            }
        }
    }

    /* Scheduled burn-in */
    {
        cJSON *be = cJSON_GetObjectItem(root, "burnin_auto_enabled");
        if (cJSON_IsBool(be)) s_cfg.burnin_auto_enabled = cJSON_IsTrue(be);
    }
    json_read_u8(root, "burnin_auto_mask", &s_cfg.burnin_auto_mask);
    s_cfg.burnin_auto_mask &= 0x3F;
    if (s_cfg.burnin_auto_mask == 0) s_cfg.burnin_auto_mask = 0x3F;
    {
        cJSON *bd = cJSON_GetObjectItem(root, "burnin_auto_duration_s");
        if (cJSON_IsNumber(bd)) {
            s_cfg.burnin_auto_duration_s = (uint32_t)bd->valueint;
            if (s_cfg.burnin_auto_duration_s < 1800)
                s_cfg.burnin_auto_duration_s = 1800;   /* minimum 30 min */
            if (s_cfg.burnin_auto_duration_s > 14400)
                s_cfg.burnin_auto_duration_s = 14400;  /* maximum 4 hours */
        }
    }
    json_read_str(root, "burnin_auto_interval", s_cfg.burnin_auto_interval,
                  sizeof(s_cfg.burnin_auto_interval));
    /* Only "weekly" and "monthly" are valid; default to "weekly" for any other string */
    if (strcmp(s_cfg.burnin_auto_interval, "monthly") != 0)
        strncpy(s_cfg.burnin_auto_interval, "weekly", sizeof(s_cfg.burnin_auto_interval) - 1);
    json_read_u8(root, "burnin_auto_hour", &s_cfg.burnin_auto_hour);
    if (s_cfg.burnin_auto_hour > 23) s_cfg.burnin_auto_hour = 0;
    json_read_str(root, "burnin_auto_mode", s_cfg.burnin_auto_mode,
                  sizeof(s_cfg.burnin_auto_mode));
    /* Only "colour-cycle" and "snow" are valid; default to "colour-cycle" */
    if (strcmp(s_cfg.burnin_auto_mode, "snow") != 0)
        strncpy(s_cfg.burnin_auto_mode, "colour-cycle", sizeof(s_cfg.burnin_auto_mode) - 1);

    /* Backlight RGB array */
    cJSON *bl_rgb = cJSON_GetObjectItem(root, "backlight_RGB");
    if (cJSON_IsArray(bl_rgb)) {
        int cnt = cJSON_GetArraySize(bl_rgb);
        if (cnt > 6) cnt = 6;
        for (int i = 0; i < cnt; i++) {
            cJSON *rgb = cJSON_GetArrayItem(bl_rgb, i);
            if (cJSON_IsArray(rgb) && cJSON_GetArraySize(rgb) >= 3) {
                cJSON *r = cJSON_GetArrayItem(rgb, 0);
                cJSON *g = cJSON_GetArrayItem(rgb, 1);
                cJSON *b = cJSON_GetArrayItem(rgb, 2);
                if (r && g && b &&
                    r->valueint >= 0 && r->valueint <= 255 &&
                    g->valueint >= 0 && g->valueint <= 255 &&
                    b->valueint >= 0 && b->valueint <= 255) {
                    s_cfg.backlight_rgb[i][0] = (uint8_t)r->valueint;
                    s_cfg.backlight_rgb[i][1] = (uint8_t)g->valueint;
                    s_cfg.backlight_rgb[i][2] = (uint8_t)b->valueint;
                }
            }
        }
    }

    /* spectrum_RGB — [R, G, B] array (matches stock firmware config.json key) */
    {
        cJSON *sp = cJSON_GetObjectItem(root, "spectrum_RGB");
        if (cJSON_IsArray(sp) && cJSON_GetArraySize(sp) >= 3) {
            for (int i = 0; i < 3; i++) {
                cJSON *v = cJSON_GetArrayItem(sp, i);
                if (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint <= 255)
                    s_cfg.spectrum_rgb[i] = (uint8_t)v->valueint;
            }
        }
    }

    /* spectrum_lcd_RGB — LCD bar colour for Spectrum mode */
    {
        cJSON *sp = cJSON_GetObjectItem(root, "spectrum_lcd_RGB");
        if (cJSON_IsArray(sp) && cJSON_GetArraySize(sp) >= 3) {
            for (int i = 0; i < 3; i++) {
                cJSON *v = cJSON_GetArrayItem(sp, i);
                if (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint <= 255)
                    s_cfg.spectrum_lcd_rgb[i] = (uint8_t)v->valueint;
            }
        }
    }

    /* spectrum_led_source — 0 = custom glow, 1 = follow accent mode */
    json_read_u8(root, "spectrum_led_source", &s_cfg.spectrum_led_source);
    if (s_cfg.spectrum_led_source > 1) s_cfg.spectrum_led_source = 0;

    /* spectrum_lcd_wled — LCD bars follow the WLED primary colour */
    {
        cJSON *sw = cJSON_GetObjectItem(root, "spectrum_lcd_wled");
        if (cJSON_IsBool(sw)) s_cfg.spectrum_lcd_wled = cJSON_IsTrue(sw);
    }

    /* notify_update_on_display — opt-in clock-face update indicator */
    {
        cJSON *nu = cJSON_GetObjectItem(root, "notify_update_on_display");
        if (cJSON_IsBool(nu)) s_cfg.notify_update_on_display = cJSON_IsTrue(nu);
    }

    /* lcd_invert_mask — per-tube INVON flag for colour-inverted replacement panels */
    json_read_u8(root, "lcd_invert_mask", &s_cfg.lcd_invert_mask);
    s_cfg.lcd_invert_mask &= 0x3F;   /* only 6 tubes */

    /* Per-tube panel profile — 0=Standard, 1=Vivid */
    JSON_READ_TUBE_INT_ARRAY(root, "lcd_init_profile", lcd_init_profile, 0, 1, uint8_t);

    /* Per-tube VCOM (VMCTR1) — clamped to 0x00..0x3F (0–63) */
    JSON_READ_TUBE_INT_ARRAY(root, "lcd_vcom", lcd_vcom, 0x00, 0x3F, uint8_t);

    /* Per-tube software gamma — clamped to 0.5..3.0.
     * Accepts both the new array form [g0,g1,g2,g3,g4,g5] and the legacy
     * scalar form (written by firmware before the per-tube refactor):
     * if a scalar is found it is broadcast to all 6 tubes so old configs
     * produce the same visual result they did before the upgrade. */
    {
        cJSON *v = cJSON_GetObjectItem(root, "lcd_gamma");
        if (cJSON_IsArray(v)) {
            int cnt = cJSON_GetArraySize(v);
            if (cnt > 6) cnt = 6;
            for (int i = 0; i < cnt; i++) {
                cJSON *el = cJSON_GetArrayItem(v, i);
                if (cJSON_IsNumber(el)) {
                    float g = (float)el->valuedouble;
                    if (g < 0.5f) g = 0.5f;
                    if (g > 3.0f) g = 3.0f;
                    s_cfg.lcd_gamma[i] = g;
                }
            }
        } else if (cJSON_IsNumber(v)) {
            /* Legacy scalar — broadcast to all tubes */
            float g = (float)v->valuedouble;
            if (g < 0.5f) g = 0.5f;
            if (g > 3.0f) g = 3.0f;
            for (int i = 0; i < 6; i++) s_cfg.lcd_gamma[i] = g;
        }
    }

    /* Per-tube CASET/RASET window offset adjustments — clamped to -8..+8 */
    JSON_READ_TUBE_INT_ARRAY(root, "lcd_col_offset", lcd_col_offset, -8, 8, int8_t);
    JSON_READ_TUBE_INT_ARRAY(root, "lcd_row_offset", lcd_row_offset, -8, 8, int8_t);
    /* Per-tube software brightness — clamped to 0-100 */
    JSON_READ_TUBE_INT_ARRAY(root, "lcd_tube_brightness", lcd_tube_brightness, 0, 100, uint8_t);

    json_read_str(root, "update_repo", s_cfg.update_repo, sizeof(s_cfg.update_repo));

    /* ── Post-parse normalization ──────────────────────────────────────
     * mic_enabled is no longer a user-settable toggle — it is derived
     * entirely from whether Spectrum mode is present in enabled_modes.
     * This enforces consistency after loading any config.json, including
     * old backups or hand-edited files where the two fields may disagree.
     * APP_MODE_SPECTRUM == 7 → bit 7 of the bitmask. */
    s_cfg.mic_enabled = (s_cfg.enabled_modes & (1u << APP_MODE_SPECTRUM)) != 0;

    /* If the weather SERVICE is enabled, the weather MODE must also be present
     * in enabled_modes, otherwise the task fetches data that is never displayed.
     * Old backup configs may have weather_enabled=true but bit 6 absent because
     * the user had removed weather from the display rotation while keeping the
     * service running, or because the config pre-dates the service/mode sync.
     * The web UI now enforces this invariant at save time via syncCounterMode();
     * apply the same rule here so backup restores work without a manual
     * toggle-off / toggle-on cycle. */
    if (s_cfg.weather_enabled)
        s_cfg.enabled_modes |= (1u << APP_MODE_WEATHER);

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
    if (sz <= 0 || sz > 8192) {
        ESP_LOGE(TAG, "CONFIG NOT LOADED: %s size=%ld out of range (1..8192) — using defaults",
                 CONFIG_PATH, sz);
        fclose(f);
        return false;
    }

    char *buf = malloc(sz + 1);
    if (!buf) { ESP_LOGE(TAG, "config load: OOM for %ld B", sz); fclose(f); return false; }
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    if (rd != (size_t)sz) { ESP_LOGE(TAG, "config load: short read %u/%ld", (unsigned)rd, sz); free(buf); return false; }
    buf[sz] = '\0';

    parse_json(buf, (size_t)sz);
    free(buf);
    ESP_LOGI(TAG, "Config loaded from flash (%ld B)", sz);
    return true;
}

/* Atomically replace the on-flash config with `json` (frees it).
 *
 * Writes a temp file first, verifies the full length landed, then rename()s
 * it over the live file — LittleFS rename replaces the destination in a
 * single commit, so a power cut at ANY point leaves either the old or the
 * new config.json intact, never a truncated one.  (The old fopen(..., "w")
 * truncated the live file before writing: a power cut mid-save destroyed
 * all settings including WiFi creds, dropping the device into setup-AP.)
 *
 * Call WITHOUT the config mutex held: LittleFS writes can take hundreds of
 * ms while garbage-collecting, and the display task takes the lock every
 * frame — holding it across the write visibly freezes the clock. */
static void write_config_file(char *json)
{
    if (!json) return;
    static const char *TMP_PATH = "/spiffs/config.json.tmp";

    size_t len = strlen(json);
    bool   ok  = false;
    FILE  *f   = fopen(TMP_PATH, "w");
    if (f) {
        ok = (fwrite(json, 1, len, f) == len);
        if (fclose(f) != 0) ok = false;
    }
    if (!ok) {
        ESP_LOGE(TAG, "Config save failed (temp write) — old config left intact");
        config_mgr_note_flash_write_failure();
        remove(TMP_PATH);
    } else if (rename(TMP_PATH, CONFIG_PATH) != 0) {
        ESP_LOGE(TAG, "Config save failed (rename) — old config left intact");
        config_mgr_note_flash_write_failure();
        remove(TMP_PATH);
    } else {
        ESP_LOGI(TAG, "Config saved to flash (%u bytes)", (unsigned)len);
    }
    free(json);
}

/* ── NVS config backup / restore ───────────────────────────────────── */
#define NVS_CFG_NS  "nextube_cfg"
#define NVS_CFG_KEY "cfg_backup"

void config_backup_to_nvs(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "cfg backup: no config.json to back up");
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8192) { fclose(f); return; }

    char *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return; }

    bool ok = (fread(buf, 1, (size_t)sz, f) == (size_t)sz);
    fclose(f);

    if (ok) {
        nvs_handle_t h;
        if (nvs_open(NVS_CFG_NS, NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_set_blob(h, NVS_CFG_KEY, buf, (size_t)sz) == ESP_OK) {
                nvs_commit(h);
                ESP_LOGI(TAG, "Config backed up to NVS (%ld B)", sz);
            } else {
                ESP_LOGE(TAG, "cfg backup: nvs_set_blob failed");
            }
            nvs_close(h);
        }
    }
    free(buf);
}

/* Returns true if a backup was found, restored to LittleFS, and deleted. */
static bool config_restore_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_CFG_NS, NVS_READWRITE, &h) != ESP_OK)
        return false;

    size_t sz = 0;
    esp_err_t pe = nvs_get_blob(h, NVS_CFG_KEY, NULL, &sz);
    if (pe != ESP_OK || sz == 0 || sz > 8192) {
        if (pe == ESP_OK && sz > 8192)
            ESP_LOGE(TAG, "CONFIG NVS backup %u B > 8192 — skipping restore", (unsigned)sz);
        nvs_close(h);
        return false;
    }

    char *buf = malloc(sz);
    if (!buf) { nvs_close(h); return false; }

    bool restored = false;
    if (nvs_get_blob(h, NVS_CFG_KEY, buf, &sz) == ESP_OK) {
        /* The NVS backup is only erased below when `restored` is true, so the
         * file write must be VERIFIED — a short write (FS full right after a
         * wipe) with the backup erased would leave the corrupt file as the
         * only copy for the next boot. */
        FILE *f = fopen(CONFIG_PATH, "w");
        if (f) {
            bool wr_ok = (fwrite(buf, 1, sz, f) == sz);
            if (fclose(f) != 0) wr_ok = false;
            if (wr_ok) {
                parse_json(buf, sz);
                ESP_LOGI(TAG, "Config restored from NVS backup (%u B)", (unsigned)sz);
                restored = true;
            } else {
                ESP_LOGE(TAG, "cfg restore: short write to %s — keeping NVS backup", CONFIG_PATH);
            }
        } else {
            ESP_LOGE(TAG, "cfg restore: cannot write %s", CONFIG_PATH);
        }
    }
    free(buf);
    if (restored) {
        nvs_erase_key(h, NVS_CFG_KEY);
        nvs_commit(h);
    }
    nvs_close(h);
    return restored;
}

/* ── Public API ────────────────────────────────────────────────────── */

void config_mgr_init(void)
{
    /* Recursive mutex: config_to_json() may be called from within an
     * already-locked context (config_set_json / config_reset serialise while
     * holding the lock), so a plain mutex would deadlock.  A recursive mutex
     * allows the same task to re-acquire it without blocking. */
    s_mutex   = xSemaphoreCreateRecursiveMutex();
    /* Plain mutex: TLS semaphore is never re-acquired by the same task. */
    s_tls_sem = xSemaphoreCreateMutex();
    set_defaults();
    if (!config_restore_from_nvs())
        load_from_flash();
}

void config_lock(void)   { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
void config_unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

/* Ownership tracking for the take-timeout case: a caller whose take() timed
 * out never owned the mutex, so its later give() must be a no-op.  Giving a
 * FreeRTOS mutex from a non-holder while the true holder still holds it
 * trips the priority-disinheritance configASSERT in queue.c → panic. */
static volatile TaskHandle_t s_tls_owner = NULL;

void tls_sem_take(void) {
    if (!s_tls_sem) return;
    /* 30 s timeout: a stalled HTTPS task must not block everything else forever. */
    if (xSemaphoreTake(s_tls_sem, pdMS_TO_TICKS(30000)) == pdTRUE)
        s_tls_owner = xTaskGetCurrentTaskHandle();
    else
        ESP_LOGE("tls_sem", "tls_sem_take: 30 s timeout — proceeding UNSERIALIZED "
                            "(possible TLS deadlock in the holder)");
}
void tls_sem_give(void) {
    if (!s_tls_sem) return;
    if (s_tls_owner == xTaskGetCurrentTaskHandle()) {
        s_tls_owner = NULL;
        xSemaphoreGive(s_tls_sem);
    }
    /* else: our take() timed out — we never owned it; giving would assert. */
}

const nextube_config_t *config_get(void)
{
    return &s_cfg;
}

bool config_set_json(const char *json, size_t len)
{
    if (!json || len == 0) return false;
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    parse_json(json, len);
    /* Serialise under the lock (RAM-only, fast) so the snapshot is
     * consistent, but do the slow flash write AFTER unlocking — the display
     * task takes this lock every frame and a LittleFS garbage-collection
     * write can stall for hundreds of ms. */
    char *out = config_to_json(true);   /* persist the WiFi password */
    xSemaphoreGiveRecursive(s_mutex);
    write_config_file(out);
    return true;
}

char *config_to_json(bool include_password)
{
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    cJSON *root = cJSON_CreateObject();
    if (!root) { xSemaphoreGiveRecursive(s_mutex); return NULL; }

    /* apps array (for backward compat with original firmware format) */
    cJSON *apps = cJSON_AddArrayToObject(root, "apps");
    cJSON *app0 = cJSON_CreateObject();
    cJSON_AddStringToObject(app0, "name", "app1");

    cJSON_AddStringToObject(app0, "app",        app_mode_name(s_cfg.current_mode));
    cJSON_AddStringToObject(app0, "theme",      s_cfg.theme);
    cJSON_AddStringToObject(app0, "type",       s_cfg.time_type);
    cJSON_AddStringToObject(app0, "clock_tube5", s_cfg.clock_tube5);
    cJSON_AddItemToArray(apps, app0);

    cJSON_AddStringToObject(root, "ssid",             s_cfg.ssid);
    /* WiFi password: included only for flash save / explicit backup.  The
     * GET /api/settings path passes include_password=false so the secret never
     * travels over the wire — a "has_password" bool is sent instead so the UI
     * can show a masked placeholder. */
    if (include_password)
        cJSON_AddStringToObject(root, "password",     s_cfg.password);
    else
        cJSON_AddBoolToObject  (root, "has_password", s_cfg.password[0] != '\0');
    cJSON_AddStringToObject(root, "video_site",       s_cfg.video_site);
    cJSON_AddStringToObject(root, "youtube_id",       s_cfg.youtube_id);
    cJSON_AddStringToObject(root, "youtube_key",      s_cfg.youtube_key);
    cJSON_AddStringToObject(root, "bili_uid",         s_cfg.bili_uid);
    /* Serialize as ±hours so the web UI shows human-readable values (e.g. -6, +5.5) */
    /* time_zone intentionally omitted — new format uses "timezone" POSIX string */
    cJSON_AddStringToObject(root, "weather_source",   s_cfg.weather_source);
    cJSON_AddStringToObject(root, "weather_api_key",  s_cfg.weather_api_key);
    cJSON_AddStringToObject(root, "City",             s_cfg.city);
    cJSON_AddNumberToObject(root, "weather_ext_lat",  s_cfg.weather_ext_lat);
    cJSON_AddNumberToObject(root, "weather_ext_lon",  s_cfg.weather_ext_lon);
    cJSON_AddBoolToObject(root,   "weather_ext_loc_valid", s_cfg.weather_ext_loc_valid);
    cJSON_AddStringToObject(root, "temperature_format",  s_cfg.temp_format);
    cJSON_AddStringToObject(root, "wind_unit",           s_cfg.wind_unit);
    cJSON_AddStringToObject(root, "date_format",         s_cfg.date_format);
    cJSON_AddStringToObject(root, "language",            s_cfg.language);
    cJSON_AddStringToObject(root, "click_file",       s_cfg.click_file);
    cJSON_AddStringToObject(root, "ticker_file",      s_cfg.ticker_file);
    cJSON_AddStringToObject(root, "hostname",        s_cfg.hostname);
    cJSON_AddBoolToObject  (root, "static_ip_enabled", s_cfg.static_ip_enabled);
    cJSON_AddStringToObject(root, "static_ip",        s_cfg.static_ip);
    cJSON_AddStringToObject(root, "static_netmask",   s_cfg.static_netmask);
    cJSON_AddStringToObject(root, "static_gateway",   s_cfg.static_gateway);
    cJSON_AddStringToObject(root, "static_dns1",      s_cfg.static_dns1);
    cJSON_AddStringToObject(root, "static_dns2",      s_cfg.static_dns2);
    cJSON_AddStringToObject(root, "timezone",        s_cfg.timezone);
    {
        cJSON *ntp_arr = cJSON_AddArrayToObject(root, "ntp_servers");
        for (int i = 0; i < 4; i++)
            cJSON_AddItemToArray(ntp_arr, cJSON_CreateString(s_cfg.ntp_servers[i]));
    }
    cJSON_AddNumberToObject(root, "time_discipline_mode", s_cfg.time_discipline_mode);
    cJSON_AddBoolToObject  (root, "button_sound",     s_cfg.button_sound);
    cJSON_AddBoolToObject  (root, "ticker_sound",     s_cfg.ticker_sound);
    cJSON_AddBoolToObject  (root, "audio_enabled",    s_cfg.audio_enabled);
    cJSON_AddBoolToObject  (root, "mic_enabled",       s_cfg.mic_enabled);
    cJSON_AddBoolToObject  (root, "weather_enabled",   s_cfg.weather_enabled);
    cJSON_AddBoolToObject  (root, "update_check_enabled", s_cfg.update_check_enabled);
    cJSON_AddBoolToObject  (root, "social_enabled",    s_cfg.social_enabled);
    cJSON_AddBoolToObject  (root, "youtube_enabled",         s_cfg.youtube_enabled);
    cJSON_AddNumberToObject(root, "sub_poll_interval_min",   s_cfg.sub_poll_interval_min);
    cJSON_AddBoolToObject  (root, "instagram_enabled",       s_cfg.instagram_enabled);
    cJSON_AddBoolToObject  (root, "tiktok_enabled",    s_cfg.tiktok_enabled);
    cJSON_AddStringToObject(root, "instagram_user",    s_cfg.instagram_user);
    cJSON_AddStringToObject(root, "instagram_method",  s_cfg.instagram_method);
    cJSON_AddStringToObject(root, "tiktok_user",         s_cfg.tiktok_user);
    cJSON_AddStringToObject(root, "tiktok_key",          s_cfg.tiktok_key);
    cJSON_AddStringToObject(root, "tiktok_relay_host",   s_cfg.tiktok_relay_host);
    cJSON_AddBoolToObject  (root, "mastodon_enabled",    s_cfg.mastodon_enabled);
    cJSON_AddStringToObject(root, "mastodon_user",       s_cfg.mastodon_user);
    cJSON_AddStringToObject(root, "mastodon_instance",   s_cfg.mastodon_instance);
    cJSON_AddBoolToObject  (root, "mdns_enabled",        s_cfg.mdns_enabled);
    cJSON_AddBoolToObject  (root, "mqtt_enabled",      s_cfg.mqtt_enabled);
    cJSON_AddStringToObject(root, "mqtt_broker",       s_cfg.mqtt_broker);
    cJSON_AddNumberToObject(root, "mqtt_port",         s_cfg.mqtt_port);
    cJSON_AddStringToObject(root, "mqtt_user",         s_cfg.mqtt_user);
    cJSON_AddStringToObject(root, "mqtt_password",     s_cfg.mqtt_password);
    cJSON_AddBoolToObject  (root, "mqtt_ha_discovery", s_cfg.mqtt_ha_discovery);
    cJSON_AddBoolToObject  (root, "mqtt_pub_ntp",      s_cfg.mqtt_pub_ntp);
    cJSON_AddBoolToObject  (root, "mqtt_pub_health",   s_cfg.mqtt_pub_health);
    cJSON_AddBoolToObject  (root, "mqtt_pub_buttons",  s_cfg.mqtt_pub_buttons);
    cJSON_AddNumberToObject(root, "mic_adc_channel",   s_cfg.mic_adc_channel);
    cJSON_AddNumberToObject(root, "mic_silence_gate",  (double)s_cfg.mic_silence_gate);
    {
        cJSON *mf = cJSON_AddArrayToObject(root, "mic_noise_floor");
        for (int i = 0; i < CFG_MIC_BAND_COUNT; i++)
            cJSON_AddItemToArray(mf, cJSON_CreateNumber((double)s_cfg.mic_noise_floor[i]));
    }
    cJSON_AddBoolToObject  (root, "mic_calibration_saved", s_cfg.mic_calibration_saved);
    cJSON_AddNumberToObject(root, "sht30_temp_offset", (double)s_cfg.sht30_temp_offset);
    cJSON_AddBoolToObject  (root, "leading_zero",     s_cfg.leading_zero);
    cJSON_AddNumberToObject(root, "volume",           s_cfg.volume);
    cJSON_AddNumberToObject(root, "led_brightness",   s_cfg.led_brightness);
    cJSON_AddNumberToObject(root, "led_effect_speed", s_cfg.led_effect_speed);
    cJSON_AddBoolToObject  (root, "led_weather_override", s_cfg.led_weather_override);
    cJSON_AddBoolToObject  (root, "wlive_animate",        s_cfg.wlive_animate);
    cJSON_AddStringToObject(root, "clock_face",           s_cfg.clock_face);
    cJSON_AddStringToObject(root, "custom_bg",            s_cfg.custom_bg);
    cJSON_AddStringToObject(root, "custom_bg_fill",       s_cfg.custom_bg_fill);
    cJSON_AddBoolToObject  (root, "custom_shadow",        s_cfg.custom_shadow);
    cJSON_AddBoolToObject  (root, "custom_night_colors",  s_cfg.custom_night_colors);
    cJSON_AddBoolToObject  (root, "custom_shadow_night",  s_cfg.custom_shadow_night);
    cJSON_AddBoolToObject  (root, "custom_glyph_shadow",        s_cfg.custom_glyph_shadow);
    cJSON_AddBoolToObject  (root, "custom_glyph_shadow_night",  s_cfg.custom_glyph_shadow_night);
    cJSON_AddStringToObject(root, "custom_font",          s_cfg.custom_font);
    {
        const char *const keys[12]   = { "custom_font_color", "custom_glyph_color", "custom_shadow_color",
                                        "dm_on_color", "dm_off_color",
                                        "custom_bg_color1", "custom_bg_color2",
                                        "custom_font_color_night", "custom_glyph_color_night",
                                        "custom_shadow_color_night",
                                        "custom_glyph_shadow_color", "custom_glyph_shadow_color_night" };
        const uint8_t *const ptrs[12] = { s_cfg.custom_font_color, s_cfg.custom_glyph_color, s_cfg.custom_shadow_color,
                                          s_cfg.dm_on_color, s_cfg.dm_off_color,
                                          s_cfg.custom_bg_color1, s_cfg.custom_bg_color2,
                                          s_cfg.custom_font_color_night, s_cfg.custom_glyph_color_night,
                                          s_cfg.custom_shadow_color_night,
                                          s_cfg.custom_glyph_shadow_color, s_cfg.custom_glyph_shadow_color_night };
        for (int ci = 0; ci < 12; ci++) {
            cJSON *arr = cJSON_CreateArray();
            for (int ch = 0; ch < 3; ch++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(ptrs[ci][ch]));
            cJSON_AddItemToObject(root, keys[ci], arr);
        }
    }
    cJSON_AddNumberToObject(root, "lcd_brightness",   s_cfg.lcd_brightness);
    cJSON_AddBoolToObject  (root, "auto_brightness",  s_cfg.auto_brightness);
    cJSON_AddNumberToObject(root, "night_brightness", s_cfg.night_brightness);
    cJSON_AddNumberToObject(root, "led_night_brightness", s_cfg.led_night_brightness);
    cJSON_AddNumberToObject(root, "night_start_hour", s_cfg.night_start_hour);
    cJSON_AddNumberToObject(root, "night_end_hour",   s_cfg.night_end_hour);
    cJSON_AddNumberToObject(root, "album_switch_time",      s_cfg.album_switch_ms);
    cJSON_AddBoolToObject  (root, "album_shuffle",          s_cfg.album_shuffle);
    cJSON_AddNumberToObject(root, "weather_panel_ms",       s_cfg.weather_panel_ms);
    cJSON_AddBoolToObject  (root, "weather_panel0_en",      s_cfg.weather_panel0_en);
    cJSON_AddBoolToObject  (root, "weather_panel1_en",      s_cfg.weather_panel1_en);
    cJSON_AddBoolToObject  (root, "weather_panel2_en",      s_cfg.weather_panel2_en);
    cJSON_AddBoolToObject  (root, "weather_panel3_en",      s_cfg.weather_panel3_en);
    cJSON_AddBoolToObject  (root, "weather_panel4_en",      s_cfg.weather_panel4_en);
    cJSON_AddBoolToObject  (root, "tube6_panel_weather",    s_cfg.tube6_panel_weather);
    cJSON_AddBoolToObject  (root, "tube6_panel_weekdate",   s_cfg.tube6_panel_weekdate);
    cJSON_AddBoolToObject  (root, "tube6_panel_ht",         s_cfg.tube6_panel_ht);
    cJSON_AddBoolToObject  (root, "tube6_panel_temp",       s_cfg.tube6_panel_temp);
    cJSON_AddBoolToObject  (root, "tube6_panel_sunrise",    s_cfg.tube6_panel_sunrise);
    cJSON_AddBoolToObject  (root, "tube6_panel_push",       s_cfg.tube6_panel_push);
    cJSON_AddBoolToObject  (root, "tube6_panel_humidity",   s_cfg.tube6_panel_humidity);
    cJSON_AddBoolToObject  (root, "tube6_panel_wind",       s_cfg.tube6_panel_wind);
    cJSON_AddBoolToObject  (root, "tube6_panel_aqi",        s_cfg.tube6_panel_aqi);
    cJSON_AddBoolToObject  (root, "tube6_panel_outdoor_ht", s_cfg.tube6_panel_outdoor_ht);
    cJSON_AddNumberToObject(root, "tube6_panel_ms",         s_cfg.tube6_panel_ms);
    cJSON_AddBoolToObject  (root, "cx_dual_panel",          s_cfg.cx_dual_panel);
    cJSON_AddBoolToObject  (root, "tube5_panel_weather",    s_cfg.tube5_panel_weather);
    cJSON_AddBoolToObject  (root, "tube5_panel_weekdate",   s_cfg.tube5_panel_weekdate);
    cJSON_AddBoolToObject  (root, "tube5_panel_ht",         s_cfg.tube5_panel_ht);
    cJSON_AddBoolToObject  (root, "tube5_panel_temp",       s_cfg.tube5_panel_temp);
    cJSON_AddBoolToObject  (root, "tube5_panel_sunrise",    s_cfg.tube5_panel_sunrise);
    cJSON_AddBoolToObject  (root, "tube5_panel_push",       s_cfg.tube5_panel_push);
    cJSON_AddBoolToObject  (root, "tube5_panel_humidity",   s_cfg.tube5_panel_humidity);
    cJSON_AddBoolToObject  (root, "tube5_panel_wind",       s_cfg.tube5_panel_wind);
    cJSON_AddBoolToObject  (root, "tube5_panel_aqi",        s_cfg.tube5_panel_aqi);
    cJSON_AddBoolToObject  (root, "tube5_panel_outdoor_ht", s_cfg.tube5_panel_outdoor_ht);
    cJSON_AddStringToObject(root, "aqi_standard",           s_cfg.aqi_standard);

    const char *bl_modes[] = {"Static","Breath","Rainbow","Off","WLED","SunMoon"};
    unsigned bl_idx = (unsigned)s_cfg.backlight_mode;
    if (bl_idx >= sizeof(bl_modes) / sizeof(bl_modes[0])) bl_idx = 0;
    cJSON_AddStringToObject(root, "backlight_mode",  bl_modes[bl_idx]);
    cJSON_AddStringToObject(root, "backlight_onoff", s_cfg.backlight_on ? "ON" : "OFF");
    cJSON_AddBoolToObject  (root, "wled_sync_enabled", s_cfg.wled_sync_enabled);
    cJSON_AddNumberToObject(root, "wled_sync_port",    s_cfg.wled_sync_port);
    cJSON_AddNumberToObject(root, "enabled_modes",      s_cfg.enabled_modes);
    cJSON_AddBoolToObject  (root, "rotation_enabled",    s_cfg.rotation_enabled);
    cJSON_AddNumberToObject(root, "rotation_interval_s", s_cfg.rotation_interval_s);
    cJSON_AddNumberToObject(root, "rotation_modes",      s_cfg.rotation_modes);
    {
        cJSON *wa = cJSON_AddArrayToObject(root, "rotation_weights");
        for (int i = 0; i < APP_MODE_MAX; i++)
            cJSON_AddItemToArray(wa, cJSON_CreateNumber(s_cfg.rotation_weights[i]));
    }
    cJSON_AddBoolToObject  (root, "theme_rotation_enabled",    s_cfg.theme_rotation_enabled);
    cJSON_AddNumberToObject(root, "theme_rotation_interval_s", s_cfg.theme_rotation_interval_s);
    {
        cJSON *ta = cJSON_AddArrayToObject(root, "theme_rotation_themes");
        for (int i = 0; i < s_cfg.theme_rotation_count; i++)
            cJSON_AddItemToArray(ta, cJSON_CreateString(s_cfg.theme_rotation_themes[i]));
    }

    cJSON_AddBoolToObject  (root, "burnin_auto_enabled",    s_cfg.burnin_auto_enabled);
    cJSON_AddNumberToObject(root, "burnin_auto_mask",       s_cfg.burnin_auto_mask);
    cJSON_AddNumberToObject(root, "burnin_auto_duration_s", s_cfg.burnin_auto_duration_s);
    cJSON_AddStringToObject(root, "burnin_auto_interval",   s_cfg.burnin_auto_interval);
    cJSON_AddNumberToObject(root, "burnin_auto_hour",       s_cfg.burnin_auto_hour);
    cJSON_AddStringToObject(root, "burnin_auto_mode",       s_cfg.burnin_auto_mode);

    cJSON *bl_rgb = cJSON_AddArrayToObject(root, "backlight_RGB");
    for (int i = 0; i < 6; i++) {
        cJSON *c = cJSON_CreateIntArray((const int[]){
            s_cfg.backlight_rgb[i][0],
            s_cfg.backlight_rgb[i][1],
            s_cfg.backlight_rgb[i][2]}, 3);
        cJSON_AddItemToArray(bl_rgb, c);
    }

    {
        cJSON *sp = cJSON_AddArrayToObject(root, "spectrum_RGB");
        for (int i = 0; i < 3; i++)
            cJSON_AddItemToArray(sp, cJSON_CreateNumber(s_cfg.spectrum_rgb[i]));
    }
    {
        cJSON *sun_arr = cJSON_AddArrayToObject(root, "sunmoon_sun_RGB");
        for (int i = 0; i < 3; i++)
            cJSON_AddItemToArray(sun_arr, cJSON_CreateNumber(s_cfg.sunmoon_sun_rgb[i]));
        cJSON *moon_arr = cJSON_AddArrayToObject(root, "sunmoon_moon_RGB");
        for (int i = 0; i < 3; i++)
            cJSON_AddItemToArray(moon_arr, cJSON_CreateNumber(s_cfg.sunmoon_moon_rgb[i]));
    }

    {
        cJSON *sp = cJSON_AddArrayToObject(root, "spectrum_lcd_RGB");
        for (int i = 0; i < 3; i++)
            cJSON_AddItemToArray(sp, cJSON_CreateNumber(s_cfg.spectrum_lcd_rgb[i]));
    }

    cJSON_AddNumberToObject(root, "spectrum_led_source", s_cfg.spectrum_led_source);
    cJSON_AddBoolToObject(root, "spectrum_lcd_wled",   s_cfg.spectrum_lcd_wled);

    cJSON_AddBoolToObject(root, "notify_update_on_display", s_cfg.notify_update_on_display);
    cJSON_AddNumberToObject(root, "lcd_invert_mask", s_cfg.lcd_invert_mask);
    json_add_tube_u8(root, "lcd_init_profile",   s_cfg.lcd_init_profile);
    json_add_tube_u8(root, "lcd_vcom",            s_cfg.lcd_vcom);
    {
        cJSON *arr = cJSON_AddArrayToObject(root, "lcd_gamma");
        for (int i = 0; i < 6; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)s_cfg.lcd_gamma[i]));
    }
    json_add_tube_i8(root, "lcd_col_offset",      s_cfg.lcd_col_offset);
    json_add_tube_i8(root, "lcd_row_offset",      s_cfg.lcd_row_offset);
    json_add_tube_u8(root, "lcd_tube_brightness", s_cfg.lcd_tube_brightness);

    if (s_cfg.update_repo[0])
        cJSON_AddStringToObject(root, "update_repo", s_cfg.update_repo);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    xSemaphoreGiveRecursive(s_mutex);
    if (!out)
        ESP_LOGE(TAG, "config_to_json: serialization FAILED (out of internal heap?)");
    else
        ESP_LOGD(TAG, "config_to_json: %u B", (unsigned)strlen(out));
    return out;
}

/* Update the active mode in RAM without a flash write.
 * Used by the touch handler and auto-rotation so that frequent mode
 * changes do not wear the flash.  The mode is persisted to flash only
 * when the user explicitly saves settings via the web UI. */
void config_set_mode(app_mode_t mode)
{
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    s_cfg.current_mode = mode;
    xSemaphoreGiveRecursive(s_mutex);
}

void config_advance_mode(void)
{
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    /* Build the effective rotation pool:
     *   rotation_modes == 0  → all enabled modes
     *   rotation_modes != 0  → user-selected subset intersected with enabled_modes
     * Fall back to enabled_modes if the intersection is empty (misconfiguration
     * or all selected modes were subsequently disabled). */
    uint16_t mask = s_cfg.rotation_modes
                    ? (s_cfg.rotation_modes & s_cfg.enabled_modes)
                    : s_cfg.enabled_modes;
    if (!mask) mask = s_cfg.enabled_modes;
    /* Strip social modes when the master social switch is off so auto-rotation
     * never advances to YouTube / Instagram / TikTok / Mastodon. */
    if (!s_cfg.social_enabled)
        mask &= ~((1u << APP_MODE_YOUTUBE)   | (1u << APP_MODE_INSTAGRAM) |
                  (1u << APP_MODE_TIKTOK)    | (1u << APP_MODE_MASTODON));
    /* If stripping left the pool empty (all enabled modes are social and social
     * is off) the loop below will exhaust all tries and stay on current mode. */

    /* Step forward through APP_MODE_MAX slots, skipping modes not in the pool.
     * Worst case: only the current mode is in the pool — we try APP_MODE_MAX
     * times before giving up (stays on current mode). */
    int m = (int)s_cfg.current_mode;
    for (int tries = 0; tries < APP_MODE_MAX; tries++) {
        m = (m + 1) % APP_MODE_MAX;
        if (mask & (1 << m)) break;
    }

    if ((app_mode_t)m != s_cfg.current_mode) {
        s_cfg.current_mode = (app_mode_t)m;
        /* No flash write — auto-rotation fires every few seconds and flash
         * wear from that frequency is unacceptable.  Mode is persisted only
         * when the user saves settings via the web UI. */
        ESP_LOGI(TAG, "Rotation: advanced to mode %d", m);
    }

    xSemaphoreGiveRecursive(s_mutex);
}

/* Update the active theme in RAM without a flash write.
 * Used by theme auto-rotation so that frequent theme changes do not wear
 * the flash.  The theme is persisted only when the user explicitly saves
 * settings via the web UI.  Thread-safe (recursive mutex). */
void config_set_theme(const char *theme)
{
    if (!theme || theme[0] == '\0') return;
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    strncpy(s_cfg.theme, theme, sizeof(s_cfg.theme) - 1);
    s_cfg.theme[sizeof(s_cfg.theme) - 1] = '\0';
    xSemaphoreGiveRecursive(s_mutex);
}

void config_reset(void)
{
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    set_defaults();
    char *out = config_to_json(true);
    xSemaphoreGiveRecursive(s_mutex);
    write_config_file(out);   /* flash write outside the lock */
    ESP_LOGI(TAG, "Config reset to factory defaults");
}

/* ── Mode name table ─────────────────────────────────────────────────
 * Single authoritative mapping from app_mode_t → display string.
 * Previously duplicated verbatim in main.c, config_mgr.c, and
 * web_server.c – adding a new mode only requires editing here. */
const char *app_mode_name(app_mode_t mode)
{
    static const char *const names[APP_MODE_MAX] = {
        [APP_MODE_CLOCK]        = "Clock",
        /* index 1 unused — was Countdown; index 2 unused — was Scoreboard;
         * index 3 unused — was Pomodoro */
        [APP_MODE_YOUTUBE]      = "YouTube",
        [APP_MODE_DATE] = "Date",
        [APP_MODE_ALBUM]        = "Album",
        [APP_MODE_WEATHER]      = "Weather",
        [APP_MODE_SPECTRUM]     = "Spectrum",
        [APP_MODE_INSTAGRAM]    = "Instagram",
        [APP_MODE_TIKTOK]       = "TikTok",
        [APP_MODE_MASTODON]     = "Mastodon",
    };
    if ((unsigned)mode >= APP_MODE_MAX) return names[APP_MODE_CLOCK];
    return names[mode];
}
