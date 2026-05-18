#include "weather.h"
#include "config_mgr.h"
#include "wifi_manager.h"
#include "fw_version.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "weather";
static weather_data_t s_weather = {0};
static SemaphoreHandle_t s_wx_mutex = NULL;

/* Snapshot of the weather-relevant config fields, copied under config_lock()
 * before any blocking HTTP call so we never hold a pointer into s_cfg across
 * a network operation. */
typedef struct {
    char city[64];
    char api_key[48];
} wx_cfg_snap_t;

/* ── API rate limiter ───────────────────────────────────────────────── */
/* Open-Meteo free-tier hard limits: < 600 calls/min, < 5 000/hr, < 10 000/day.
 * We use fixed windows with a 10 % safety margin so bursts near a window
 * boundary never push us over the server-side limit.
 *
 * In normal operation (10-minute poll, 1-2 calls per poll) none of these
 * caps will ever be reached; the limiter is a guard against pathological
 * retry storms or future code changes that increase call frequency.
 *
 * All calls that go through http_get() are counted, regardless of provider.
 * The weather task is single-threaded so no mutex is needed here. */
#define RL_CAP_MIN   540        /* 600  × 0.90 */
#define RL_CAP_HOUR  4500       /* 5000 × 0.90 */
#define RL_CAP_DAY   9000       /* 10000 × 0.90 */

typedef struct { int64_t start_us; int count; } rl_win_t;
static rl_win_t s_rl_min  = {0, 0};
static rl_win_t s_rl_hour = {0, 0};
static rl_win_t s_rl_day  = {0, 0};

static void rl_wait(void)
{
    for (;;) {
        int64_t now = esp_timer_get_time();

        /* Advance any window that has expired */
        if (now - s_rl_min.start_us  >=    60LL * 1000000LL) { s_rl_min.start_us  = now; s_rl_min.count  = 0; }
        if (now - s_rl_hour.start_us >= 3600LL  * 1000000LL) { s_rl_hour.start_us = now; s_rl_hour.count = 0; }
        if (now - s_rl_day.start_us  >= 86400LL * 1000000LL) { s_rl_day.start_us  = now; s_rl_day.count  = 0; }

        /* Find the longest wait imposed by any saturated window */
        int64_t wait_us = 0, w;
        if (s_rl_min.count  >= RL_CAP_MIN) {
            w = 60LL   * 1000000LL - (now - s_rl_min.start_us);
            if (w > wait_us) wait_us = w;
        }
        if (s_rl_hour.count >= RL_CAP_HOUR) {
            w = 3600LL * 1000000LL - (now - s_rl_hour.start_us);
            if (w > wait_us) wait_us = w;
        }
        if (s_rl_day.count  >= RL_CAP_DAY) {
            w = 86400LL * 1000000LL - (now - s_rl_day.start_us);
            if (w > wait_us) wait_us = w;
        }

        if (wait_us <= 0) break;    /* all windows have capacity */

        ESP_LOGW(TAG, "API rate limit reached — waiting %lld s before next call",
                 (long long)(wait_us / 1000000LL));
        /* Sleep until the tightest window expires (+500 ms margin) */
        vTaskDelay(pdMS_TO_TICKS(wait_us / 1000 + 500));
    }

    /* Consume one slot in every active window */
    s_rl_min.count++;
    s_rl_hour.count++;
    s_rl_day.count++;
}

/* ── HTTP helper ────────────────────────────────────────────────────── */
/* Returns heap-allocated NUL-terminated body (up to 4 KB), or NULL on error.
 * Uses open/fetch_headers/read loop so chunked-encoded responses (no
 * Content-Length header) work correctly. Caller frees the returned buffer.
 * A descriptive User-Agent is sent with every request; this is required by
 * Met.no's terms of service and harmless to all other APIs.
 *
 * crt_bundle_attach enables TLS certificate verification using the bundled
 * Mozilla CA store.  Without it, all HTTPS requests fail the TLS handshake
 * silently (wttr.in, Open-Meteo, Met.no are all HTTPS-only). */
#define HTTP_MAX_BODY 4096
#define HTTP_USER_AGENT \
    "NextubeRemaster/" FW_VERSION_STR " github.com/MrToast99/Nextube-Remaster"

static char *http_get(const char *url)
{
    rl_wait();      /* enforce Open-Meteo free-tier rate limits */

    esp_http_client_config_t hcfg = {
        .url               = url,
        .timeout_ms        = 5000,   /* fail fast — frees TLS heap for web server */
        .user_agent        = HTTP_USER_AGENT,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hcfg);
    if (!c) return NULL;

    if (esp_http_client_open(c, 0) != ESP_OK) {
        esp_http_client_cleanup(c);
        return NULL;
    }
    esp_http_client_fetch_headers(c);

    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d: %s", status, url);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return NULL;
    }

    char *buf = malloc(HTTP_MAX_BODY + 1);
    if (!buf) { esp_http_client_close(c); esp_http_client_cleanup(c); return NULL; }

    int total = 0, r;
    do {
        r = esp_http_client_read(c, buf + total, HTTP_MAX_BODY - total);
        if (r > 0) total += r;
    } while (r > 0 && total < HTTP_MAX_BODY);
    buf[total] = '\0';

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (total == 0) { free(buf); return NULL; }
    return buf;
}

/* ── wttr.in  (no API key required) ────────────────────────────────── */
/*
 * URL:  https://wttr.in/{city}?format=j1
 * Relevant JSON fields:
 *   { "current_condition": [{
 *       "temp_C": "15",
 *       "humidity": "65",
 *       "weatherDesc": [{"value": "Partly cloudy"}]
 *   }] }
 */
static void fetch_wttr(const wx_cfg_snap_t *cfg)
{
    if (cfg->city[0] == '\0') return;

    char url[256];
    snprintf(url, sizeof(url), "https://wttr.in/%s?format=j1", cfg->city);

    char *body = http_get(url);
    if (!body) return;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { ESP_LOGW(TAG, "wttr.in: JSON parse failed"); return; }

    cJSON *cur = cJSON_GetArrayItem(
                     cJSON_GetObjectItem(root, "current_condition"), 0);
    if (cur) {
        cJSON *tc       = cJSON_GetObjectItem(cur, "temp_C");
        cJSON *hum      = cJSON_GetObjectItem(cur, "humidity");
        cJSON *desc_arr = cJSON_GetObjectItem(cur, "weatherDesc");

        /* Parse values into locals before taking the mutex */
        float  parsed_temp = 0.0f;
        float  parsed_hum  = 0.0f;
        bool   have_temp = false, have_hum = false;
        char   parsed_condition[64] = {0};
        char   parsed_icon[32] = {0};
        bool   have_desc = false;

        if (tc  && tc->valuestring)  { parsed_temp = (float)atof(tc->valuestring);  have_temp = true; }
        if (hum && hum->valuestring) { parsed_hum  = (float)atof(hum->valuestring); have_hum  = true; }
        cJSON *desc0 = cJSON_GetArrayItem(desc_arr, 0);
        if (desc0) {
            cJSON *val = cJSON_GetObjectItem(desc0, "value");
            if (val && val->valuestring) {
                strncpy(parsed_condition, val->valuestring,
                        sizeof(parsed_condition) - 1);
                /* Map wttr.in condition string to SPIFFS icon filename. */
                const char *d = val->valuestring;
                const char *icon;
                if      (strstr(d, "Sunny")     || strstr(d, "Clear"))     icon = "sun";
                else if (strstr(d, "Thunder")   || strstr(d, "thunder"))   icon = "thunderstorm";
                else if (strstr(d, "Snow")      || strstr(d, "Blizzard"))  icon = "snow";
                else if (strstr(d, "Sleet"))                               icon = "rain";
                else if (strstr(d, "Rain")      || strstr(d, "Drizzle")
                      || strstr(d, "rain")      || strstr(d, "drizzle"))   icon = "rain";
                else if (strstr(d, "Shower"))                              icon = "squalls";
                else if (strstr(d, "Fog")       || strstr(d, "Mist")
                      || strstr(d, "fog")       || strstr(d, "mist"))      icon = "fog";
                else if (strstr(d, "Overcast"))                            icon = "overcastClouds";
                else if (strstr(d, "Partly")    || strstr(d, "Few"))       icon = "fewClouds";
                else if (strstr(d, "Cloud")     || strstr(d, "cloud"))     icon = "overcastClouds";
                else                                                        icon = "sun";
                strncpy(parsed_icon, icon, sizeof(parsed_icon) - 1);
                have_desc = true;
            }
        }

        xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
        if (have_temp) s_weather.temp_c   = parsed_temp;
        if (have_hum)  s_weather.humidity = parsed_hum;
        if (have_desc) {
            strncpy(s_weather.condition, parsed_condition, sizeof(s_weather.condition) - 1);
            strncpy(s_weather.icon, parsed_icon, sizeof(s_weather.icon) - 1);
        }
        s_weather.valid = true;
        xSemaphoreGive(s_wx_mutex);

        ESP_LOGI(TAG, "wttr.in: %.0f°C  %d%%  %s",
                 parsed_temp, (int)parsed_hum, parsed_condition);
    }
    cJSON_Delete(root);
}

/* ── OpenWeatherMap  (free-tier API key required) ───────────────────── */
static void fetch_openweather(const wx_cfg_snap_t *cfg)
{
    if (cfg->api_key[0] == '\0' || cfg->city[0] == '\0') {
        ESP_LOGW(TAG, "OpenWeatherMap: no API key or city configured");
        return;
    }

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.openweathermap.org/data/2.5/weather"
             "?q=%s&appid=%s&units=metric",
             cfg->city, cfg->api_key);

    char *body = http_get(url);
    if (!body) return;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { ESP_LOGW(TAG, "OWM: JSON parse failed"); return; }

    /* Parse into locals before taking the mutex */
    float parsed_temp = 0.0f, parsed_hum = 0.0f;
    bool  have_temp = false, have_hum = false;
    char  parsed_condition[64] = {0};
    char  parsed_icon[32] = {0};
    bool  have_condition = false, have_icon = false;

    cJSON *main_obj = cJSON_GetObjectItem(root, "main");
    if (main_obj) {
        cJSON *temp = cJSON_GetObjectItem(main_obj, "temp");
        cJSON *hum  = cJSON_GetObjectItem(main_obj, "humidity");
        if (temp) { parsed_temp = (float)temp->valuedouble; have_temp = true; }
        if (hum)  { parsed_hum  = (float)hum->valuedouble;  have_hum  = true; }
    }
    cJSON *w0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "weather"), 0);
    if (w0) {
        cJSON *desc = cJSON_GetObjectItem(w0, "main");
        cJSON *icon = cJSON_GetObjectItem(w0, "icon");
        if (desc && desc->valuestring) {
            strncpy(parsed_condition, desc->valuestring,
                    sizeof(parsed_condition) - 1);
            have_condition = true;
        }
        /* OWM returns codes like "01d","02n" etc. Map to SPIFFS filenames. */
        if (icon && icon->valuestring) {
            const char *ic = icon->valuestring;
            const char *mapped;
            if      (strncmp(ic, "01", 2) == 0) mapped = "sun";
            else if (strncmp(ic, "02", 2) == 0) mapped = "fewClouds";
            else if (strncmp(ic, "03", 2) == 0) mapped = "fewClouds";
            else if (strncmp(ic, "04", 2) == 0) mapped = "overcastClouds";
            else if (strncmp(ic, "09", 2) == 0) mapped = "squalls";
            else if (strncmp(ic, "10", 2) == 0) mapped = "rain";
            else if (strncmp(ic, "11", 2) == 0) mapped = "thunderstorm";
            else if (strncmp(ic, "13", 2) == 0) mapped = "snow";
            else if (strncmp(ic, "50", 2) == 0) mapped = "fog";
            else                                 mapped = "sun";
            strncpy(parsed_icon, mapped, sizeof(parsed_icon) - 1);
            have_icon = true;
        }
    }

    xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
    if (have_temp)      s_weather.temp_c   = parsed_temp;
    if (have_hum)       s_weather.humidity = parsed_hum;
    if (have_condition) strncpy(s_weather.condition, parsed_condition, sizeof(s_weather.condition) - 1);
    if (have_icon)      strncpy(s_weather.icon, parsed_icon, sizeof(s_weather.icon) - 1);
    s_weather.valid = true;
    xSemaphoreGive(s_wx_mutex);

    ESP_LOGI(TAG, "OWM: %.0f°C  %d%%  %s",
             parsed_temp, (int)parsed_hum, parsed_condition);
    cJSON_Delete(root);
}

/* ── Open-Meteo  (free, no API key, geocoding via city name) ────────── */
/*
 * Geocoding: https://geocoding-api.open-meteo.com/v1/search?name={city}&count=1
 * Weather:   https://api.open-meteo.com/v1/forecast
 *              ?latitude={lat}&longitude={lon}
 *              &current=temperature_2m,relative_humidity_2m,weather_code
 *
 * WMO weather codes → icon name used by display_path_weather()
 */
/* Maps WMO weather codes to SPIFFS icon filenames (no extension). */
static const char *wmo_icon(int code)
{
    if (code == 0)        return "sun";
    if (code <= 2)        return "fewClouds";
    if (code == 3)        return "overcastClouds";
    if (code <= 48)       return "fog";
    if (code <= 67)       return "rain";
    if (code <= 77)       return "snow";
    if (code <= 82)       return "squalls";
    return "thunderstorm";
}

static const char *wmo_condition(int code)
{
    if (code == 0)        return "Clear sky";
    if (code == 1)        return "Mainly clear";
    if (code == 2)        return "Partly cloudy";
    if (code == 3)        return "Overcast";
    if (code <= 48)       return "Foggy";
    if (code <= 55)       return "Drizzle";
    if (code <= 67)       return "Rain";
    if (code <= 77)       return "Snow";
    if (code <= 82)       return "Rain showers";
    return "Thunderstorm";
}

/* Geocode city → lat/lon/elevation via Open-Meteo geocoding API.
 * Result is cached; geocoding only runs again when the city changes.
 *
 * The city string may be in "City,CC" format (e.g. "Airdrie,CA") as used by
 * wttr.in.  "CC" is a two-letter ISO 3166-1 alpha-2 country code (CA=Canada,
 * GB=United Kingdom, US=United States, etc.).
 *
 * We split on the first comma to get the plain city name, then append
 * &countrycode=CC to the geocoding request so that "Airdrie,CA" resolves to
 * Airdrie, Alberta, Canada (lat≈51.3) rather than Airdrie, Scotland (lat≈55.9).
 * We also request count=5 results and pick the first whose country_code field
 * matches, falling back to the first result if none match.
 *
 * alt_m may be NULL if the caller doesn't need elevation. */
static bool geocode_open_meteo(const char *city, float *lat, float *lon, int *alt_m)
{
    /* Split "City,CC" → cityname + optional countrycode */
    char cityname[64];
    char countrycode[8] = {0};
    strncpy(cityname, city, sizeof(cityname) - 1);
    cityname[sizeof(cityname) - 1] = '\0';
    char *comma = strchr(cityname, ',');
    if (comma) {
        *comma = '\0';
        strncpy(countrycode, comma + 1, sizeof(countrycode) - 1);
        countrycode[sizeof(countrycode) - 1] = '\0';
        /* Trim whitespace */
        char *p = countrycode;
        while (*p == ' ') p++;
        memmove(countrycode, p, strlen(p) + 1);
    }

    /* Build URL: include countrycode filter when available; request 5 candidates
     * so we can verify the match even if the API returns mixed results. */
    char url[320];
    if (countrycode[0]) {
        snprintf(url, sizeof(url),
                 "https://geocoding-api.open-meteo.com/v1/search"
                 "?name=%s&count=5&countrycode=%s&format=json",
                 cityname, countrycode);
    } else {
        snprintf(url, sizeof(url),
                 "https://geocoding-api.open-meteo.com/v1/search"
                 "?name=%s&count=5&format=json", cityname);
    }

    char *body = http_get(url);
    if (!body) return false;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return false;

    bool   ok      = false;
    cJSON *results = cJSON_GetObjectItem(root, "results");
    int    n_res   = cJSON_GetArraySize(results);

    /* Prefer the first result whose country_code matches; fall back to r[0]. */
    cJSON *best = cJSON_GetArrayItem(results, 0);
    if (countrycode[0] && n_res > 1) {
        for (int i = 0; i < n_res; i++) {
            cJSON *r  = cJSON_GetArrayItem(results, i);
            cJSON *cc = cJSON_GetObjectItem(r, "country_code");
            if (cc && cc->valuestring &&
                strcasecmp(cc->valuestring, countrycode) == 0) {
                best = r;
                break;
            }
        }
    }

    if (best) {
        cJSON *la = cJSON_GetObjectItem(best, "latitude");
        cJSON *lo = cJSON_GetObjectItem(best, "longitude");
        cJSON *el = cJSON_GetObjectItem(best, "elevation");
        if (la && lo) {
            *lat = (float)la->valuedouble;
            *lon = (float)lo->valuedouble;
            if (alt_m) *alt_m = el ? (int)el->valuedouble : 0;
            ok = true;
            cJSON *nm = cJSON_GetObjectItem(best, "name");
            cJSON *cc = cJSON_GetObjectItem(best, "country_code");
            ESP_LOGI("weather", "geocoded '%s' → %s (%s) lat=%.4f lon=%.4f alt=%d m",
                     city,
                     nm  && nm->valuestring ? nm->valuestring  : "?",
                     cc  && cc->valuestring ? cc->valuestring  : "?",
                     (double)*lat, (double)*lon,
                     alt_m ? *alt_m : 0);
        }
    }
    cJSON_Delete(root);
    return ok;
}

static void fetch_open_meteo(const wx_cfg_snap_t *cfg)
{
    if (cfg->city[0] == '\0') {
        ESP_LOGW(TAG, "Open-Meteo: no city configured"); return;
    }

    /* Cache lat/lon; only re-geocode when city changes */
    static float s_lat = 0.0f, s_lon = 0.0f;
    static char  s_last_city[64] = {0};

    if (strncmp(cfg->city, s_last_city, sizeof(s_last_city)) != 0) {
        if (!geocode_open_meteo(cfg->city, &s_lat, &s_lon, NULL)) {
            ESP_LOGW(TAG, "Open-Meteo: geocoding failed for '%s'", cfg->city);
            return;
        }
        strncpy(s_last_city, cfg->city, sizeof(s_last_city) - 1);
        ESP_LOGI(TAG, "Open-Meteo: geocoded '%s' → %.4f, %.4f", cfg->city, s_lat, s_lon);
    }

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code",
             s_lat, s_lon);

    char *body = http_get(url);
    if (!body) return;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { ESP_LOGW(TAG, "Open-Meteo: JSON parse failed"); return; }

    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (cur) {
        cJSON *temp = cJSON_GetObjectItem(cur, "temperature_2m");
        cJSON *hum  = cJSON_GetObjectItem(cur, "relative_humidity_2m");
        cJSON *wc   = cJSON_GetObjectItem(cur, "weather_code");

        float parsed_temp = 0.0f, parsed_hum = 0.0f;
        bool  have_temp = false, have_hum = false;
        char  parsed_icon[32] = {0};
        char  parsed_condition[64] = {0};
        bool  have_wc = false;

        if (temp) { parsed_temp = (float)temp->valuedouble; have_temp = true; }
        if (hum)  { parsed_hum  = (float)hum->valuedouble;  have_hum  = true; }
        if (wc) {
            int code = wc->valueint;
            strncpy(parsed_icon,      wmo_icon(code),      sizeof(parsed_icon) - 1);
            strncpy(parsed_condition, wmo_condition(code),  sizeof(parsed_condition) - 1);
            have_wc = true;
        }

        xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
        if (have_temp) s_weather.temp_c   = parsed_temp;
        if (have_hum)  s_weather.humidity = parsed_hum;
        if (have_wc) {
            strncpy(s_weather.icon,      parsed_icon,      sizeof(s_weather.icon) - 1);
            strncpy(s_weather.condition, parsed_condition,  sizeof(s_weather.condition) - 1);
        }
        s_weather.valid = true;
        xSemaphoreGive(s_wx_mutex);

        ESP_LOGI(TAG, "Open-Meteo: %.0f°C  %d%%  %s",
                 parsed_temp, (int)parsed_hum, parsed_condition);
    }
    cJSON_Delete(root);
}

/* ── Met.no  (free, no API key; User-Agent required by their ToS) ───── */
/*
 * Geocoding: reuse Open-Meteo geocoder (same lat/lon cache key format).
 * Forecast:  https://api.met.no/weatherapi/locationforecast/2.0/compact
 *              ?lat={lat}&lon={lon}
 *
 * The full response is 50-100 KB which exceeds HTTP_MAX_BODY.  Only the
 * first timeseries entry is needed and it always falls within the first
 * 4 KB, so we parse the (possibly-truncated) buffer with strstr instead
 * of cJSON to avoid a heap-exhausting full parse.
 *
 * Met.no symbol codes are like "clearsky_day", "heavyrainandthunder_night",
 * etc.  We match on the stem before the first underscore.
 */
static const char *metno_icon(const char *symbol_code)
{
    /* Strip the day/night/polartwilight suffix (everything after first '_') */
    char stem[32] = {0};
    const char *u = strchr(symbol_code, '_');
    size_t slen = u ? (size_t)(u - symbol_code) : strlen(symbol_code);
    if (slen >= sizeof(stem)) slen = sizeof(stem) - 1;
    memcpy(stem, symbol_code, slen);

    /* Maps Met.no symbol stems to SPIFFS icon filenames (no extension). */
    if (strcmp(stem, "clearsky")        == 0) return "sun";
    if (strcmp(stem, "fair")            == 0) return "sun";
    if (strcmp(stem, "partlycloudy")    == 0) return "fewClouds";
    if (strcmp(stem, "cloudy")          == 0) return "overcastClouds";
    if (strcmp(stem, "fog")             == 0) return "fog";
    if (strcmp(stem, "lightdrizzle")    == 0) return "rain";
    if (strcmp(stem, "drizzle")         == 0) return "rain";
    if (strcmp(stem, "heavydrizzle")    == 0) return "rain";
    if (strcmp(stem, "lightrain")       == 0) return "rain";
    if (strcmp(stem, "rain")            == 0) return "rain";
    if (strcmp(stem, "heavyrain")       == 0) return "rain";
    if (strcmp(stem, "lightsleet")      == 0) return "rain";
    if (strcmp(stem, "sleet")           == 0) return "rain";
    if (strcmp(stem, "heavysleet")      == 0) return "rain";
    if (strcmp(stem, "lightsnow")       == 0) return "snow";
    if (strcmp(stem, "snow")            == 0) return "snow";
    if (strcmp(stem, "heavysnow")       == 0) return "snow";
    if (strncmp(stem, "rainshowers",   11) == 0) return "squalls";
    if (strncmp(stem, "sleetshowers",  12) == 0) return "squalls";
    if (strncmp(stem, "snowshowers",   11) == 0) return "squalls";
    if (strstr(stem,  "thunder")           != NULL) return "thunderstorm";
    return "overcastClouds";
}

static const char *metno_condition(const char *symbol_code)
{
    char stem[32] = {0};
    const char *u = strchr(symbol_code, '_');
    size_t slen = u ? (size_t)(u - symbol_code) : strlen(symbol_code);
    if (slen >= sizeof(stem)) slen = sizeof(stem) - 1;
    memcpy(stem, symbol_code, slen);

    if (strcmp(stem, "clearsky")     == 0) return "Clear sky";
    if (strcmp(stem, "fair")         == 0) return "Fair";
    if (strcmp(stem, "partlycloudy") == 0) return "Partly cloudy";
    if (strcmp(stem, "cloudy")       == 0) return "Cloudy";
    if (strcmp(stem, "fog")          == 0) return "Fog";
    if (strncmp(stem, "lightdrizzle",  12) == 0) return "Light drizzle";
    if (strcmp(stem, "drizzle")      == 0) return "Drizzle";
    if (strncmp(stem, "heavydrizzle", 12) == 0) return "Heavy drizzle";
    if (strcmp(stem, "lightrain")    == 0) return "Light rain";
    if (strcmp(stem, "rain")         == 0) return "Rain";
    if (strcmp(stem, "heavyrain")    == 0) return "Heavy rain";
    if (strncmp(stem, "lightsleet",  10) == 0) return "Light sleet";
    if (strcmp(stem, "sleet")        == 0) return "Sleet";
    if (strncmp(stem, "heavysleet",  10) == 0) return "Heavy sleet";
    if (strcmp(stem, "lightsnow")    == 0) return "Light snow";
    if (strcmp(stem, "snow")         == 0) return "Snow";
    if (strcmp(stem, "heavysnow")    == 0) return "Heavy snow";
    if (strncmp(stem, "rainshowers", 11) == 0) return "Rain showers";
    if (strncmp(stem, "snowshowers", 11) == 0) return "Snow showers";
    if (strstr(stem, "thunder")      != NULL) return "Thunderstorm";
    return "Overcast";
}

/* Lightweight strstr-based extraction of a JSON number value.
 * Scans forward through all occurrences of "key": and returns atof of the
 * first occurrence whose value is numeric (not a quoted string).
 *
 * This is necessary for Met.no responses which contain the key twice:
 *   "units":   { "air_temperature": "celsius" }   ← string, must skip
 *   "details": { "air_temperature": 5.5 }          ← number, use this */
static bool json_extract_number(const char *buf, const char *key, float *out)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    size_t slen = strlen(search);
    const char *p = buf;
    while ((p = strstr(p, search)) != NULL) {
        p += slen;
        while (*p == ' ' || *p == ':' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') continue;  /* string value (e.g. "celsius") — skip */
        *out = (float)atof(p);
        return true;
    }
    return false;
}

/* Lightweight extraction of a JSON string value (first match). */
static bool json_extract_string(const char *buf, const char *key,
                                char *dst, size_t dstsz)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(buf, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;  /* skip opening quote */
    size_t i = 0;
    while (*p && *p != '"' && i < dstsz - 1) dst[i++] = *p++;
    dst[i] = '\0';
    return i > 0;
}

static void fetch_met_no(const wx_cfg_snap_t *cfg)
{
    if (cfg->city[0] == '\0') {
        ESP_LOGW(TAG, "Met.no: no city configured"); return;
    }

    /* Geocode city → lat/lon/elevation (elevation is required by Met.no for
     * accurate forecasts; without it the API uses a default of 0 m which
     * produces wrong results for inland/elevated cities like Airdrie, AB). */
    static float s_lat = 0.0f, s_lon = 0.0f;
    static int   s_alt = 0;
    static char  s_last_city[64] = {0};

    if (strncmp(cfg->city, s_last_city, sizeof(s_last_city)) != 0) {
        if (!geocode_open_meteo(cfg->city, &s_lat, &s_lon, &s_alt)) {
            ESP_LOGW(TAG, "Met.no: geocoding failed for '%s'", cfg->city);
            return;
        }
        strncpy(s_last_city, cfg->city, sizeof(s_last_city) - 1);
        ESP_LOGI(TAG, "Met.no: geocoded '%s' → %.4f, %.4f  alt=%d m",
                 cfg->city, s_lat, s_lon, s_alt);
    }

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.met.no/weatherapi/locationforecast/2.0/compact"
             "?lat=%.4f&lon=%.4f&altitude=%d",
             s_lat, s_lon, s_alt);

    char *body = http_get(url);
    if (!body) return;

    /* The full response is too large for cJSON; use strstr on the first 4 KB.
     * "air_temperature" and "relative_humidity" appear in the first
     * "instant" block which is always within the first 1-2 KB. */
    float temp = 0.0f, hum = 0.0f;
    bool  got_temp = json_extract_number(body, "air_temperature",    &temp);
    bool  got_hum  = json_extract_number(body, "relative_humidity",  &hum);

    char symbol[64] = {0};
    bool got_sym = json_extract_string(body, "symbol_code", symbol, sizeof(symbol));

    free(body);

    if (!got_temp && !got_hum) {
        ESP_LOGW(TAG, "Met.no: could not parse temperature/humidity"); return;
    }

    xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
    if (got_temp) s_weather.temp_c   = temp;
    if (got_hum)  s_weather.humidity = hum;
    if (got_sym) {
        strncpy(s_weather.icon,      metno_icon(symbol),      sizeof(s_weather.icon) - 1);
        strncpy(s_weather.condition, metno_condition(symbol),  sizeof(s_weather.condition) - 1);
    }
    s_weather.valid = true;
    xSemaphoreGive(s_wx_mutex);
    ESP_LOGI(TAG, "Met.no: %.0f°C  %d%%  %s",
             temp, (int)hum, s_weather.condition);
}

/* ── Task ───────────────────────────────────────────────────────────── */
static void fetch_weather(void)
{
    wx_cfg_snap_t snap;
    char source[16];

    config_lock();
    const nextube_config_t *cfg = config_get();
    strncpy(source,        cfg->weather_source,  sizeof(source)        - 1); source[sizeof(source)              - 1] = '\0';
    strncpy(snap.city,     cfg->city,             sizeof(snap.city)     - 1); snap.city[sizeof(snap.city)        - 1] = '\0';
    strncpy(snap.api_key,  cfg->weather_api_key,  sizeof(snap.api_key)  - 1); snap.api_key[sizeof(snap.api_key)  - 1] = '\0';
    config_unlock();

    if (strcmp(source, "openmeteo") == 0) {
        fetch_open_meteo(&snap);
    } else if (strcmp(source, "metno") == 0) {
        fetch_met_no(&snap);
    } else if (strcmp(source, "openweather") == 0 && snap.api_key[0] != '\0') {
        fetch_openweather(&snap);
    } else {
        fetch_wttr(&snap);   /* default: wttr.in, no key needed */
    }
}

static void weather_task(void *arg)
{
    /* Wait until STA actually has an IP before attempting any HTTPS connection. */
    ESP_LOGI(TAG, "waiting for WiFi...");
    while (!wifi_manager_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* Give the DNS resolver ~8 s to settle after DHCP assigns an IP.
     * 3 s proved insufficient on some routers — the first geocoding lookup
     * still hits EAI_AGAIN (getaddrinfo code 202) because the lwIP resolver
     * isn't ready yet.  8 s matches the audio task's post-WPA2 delay and
     * eliminates the error in practice.  The exponential backoff below
     * (2 s → 4 s → 8 s → … capped at 5 min) handles any remaining edge cases
     * without hammering the resolver with rapid-fire failing queries. */
    vTaskDelay(pdMS_TO_TICKS(8000));

    /* First fetch: attempt immediately, then retry with exponential backoff
     * (2 s → 4 s → 8 s → … capped at 5 min) until it succeeds.
     * If weather is not configured (empty city, no API key) the fetch
     * functions return immediately without setting s_weather.valid;
     * the backoff prevents spinning at 2 s forever in that case. */
    ESP_LOGI(TAG, "WiFi ready – fetching weather");
    uint32_t retry_ms = 2000;
    while (!s_weather.valid) {
        fetch_weather();
        if (!s_weather.valid) {
            ESP_LOGW(TAG, "Weather fetch failed, retrying in %lu s",
                     (unsigned long)(retry_ms / 1000));
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            if (retry_ms < 300000)
                retry_ms = (retry_ms * 2 < 300000) ? retry_ms * 2 : 300000;
        }
    }

    /* Subsequent fetches every 10 minutes */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(600000));
        ESP_LOGI(TAG, "fetching weather...");
        fetch_weather();
    }
}

void weather_start(void)
{
    s_wx_mutex = xSemaphoreCreateMutex();
    xTaskCreate(weather_task, "weather", 8192, NULL, 3, NULL);
}

const weather_data_t *weather_get(void)
{
    static weather_data_t copy;
    if (s_wx_mutex) {
        xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
        copy = s_weather;
        xSemaphoreGive(s_wx_mutex);
    } else {
        copy = s_weather;
    }
    return &copy;
}
