#include "weather.h"
#include "outlook_calc.h"
#include "config_mgr.h"
#include "periodic_net_poll.h"
#include "json_util.h"
#include "url_util.h"
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
#include <math.h>
#include <time.h>

static const char *TAG = "weather";
/* cloud_cover_pct/precip_mm start at -1 ("not fetched yet") — see their
 * weather.h field comments for why that differs from wind_kph's 0 default. */
static weather_data_t s_weather = { .cloud_cover_pct = -1.0f, .precip_mm = -1.0f,
                                    .pressure_hpa = -1.0f, .wind_dir_deg = -1.0f, .wind_gust_kph = -1.0f, .rain_today_mm = -1.0f };
static SemaphoreHandle_t s_wx_mutex = NULL;

/* Location coordinates from the last successful weather fetch.
 * All four providers (wttr.in, Open-Meteo, Met.no, OpenWeatherMap) populate
 * these from the coordinates returned in their respective responses.
 * Protected by s_wx_mutex so readers always see a consistent lat/lon pair. */
static float s_wx_lat = 0.0f, s_wx_lon = 0.0f;
static bool  s_wx_location_valid = false;

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
/* wttr.in's format=j1 response is 5-30+ KB (multi-day/hourly forecast,
 * astronomy data) — far past HTTP_MAX_BODY, which silently truncates the
 * read into invalid JSON that cJSON_Parse then rejects outright (unlike
 * Met.no below, whose needed fields are extracted via strstr and tolerate
 * truncation). wttr.in is the default provider when none is configured, so
 * this reliably broke weather for most real city names out of the box.
 * malloc() here is satisfied from the PSRAM pool already folded into the
 * general heap allocator (4 MB, see boot log), not the small/scarce
 * MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA pool — an occasional 32 KB buffer for
 * an infrequent poll is cheap from that pool. */
#define WTTR_MAX_BODY (32 * 1024)
#define HTTP_USER_AGENT \
    "NextubeRemaster/" FW_VERSION_STR " github.com/MrToast99/Nextube-Remaster"

static char *http_get_sized(const char *url, size_t max_body)
{
    rl_wait();      /* enforce Open-Meteo free-tier rate limits */

    /* Serialise TLS connections: only one mbedTLS SSL context at a time.
     * Prevents internal-SRAM exhaustion when the social-counter task runs
     * an HTTPS fetch concurrently with a weather retry. */
    tls_sem_take();

    esp_http_client_config_t hcfg = {
        .url               = url,
        .timeout_ms        = 10000,  /* 5 s proved too tight for cold-boot connects:
                                      * select() timeouts on the first geocoding fetch
                                      * while DNS/gateway are cold.  10 s matches the
                                      * subscribers component (same tls_sem). */
        .user_agent        = HTTP_USER_AGENT,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    char *result = NULL;

    esp_http_client_handle_t c = esp_http_client_init(&hcfg);
    if (!c) goto done;

    if (esp_http_client_open(c, 0) != ESP_OK) {
        esp_http_client_cleanup(c); c = NULL; goto done;
    }
    esp_http_client_fetch_headers(c);

    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d: %s", status, url);
        esp_http_client_close(c);
        esp_http_client_cleanup(c); c = NULL; goto done;
    }

    result = malloc(max_body + 1);
    if (!result) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c); c = NULL; goto done;
    }

    int total = 0, r;
    do {
        r = esp_http_client_read(c, result + total, (int)max_body - total);
        if (r > 0) total += r;
    } while (r > 0 && (size_t)total < max_body);
    result[total] = '\0';

    esp_http_client_close(c);
    esp_http_client_cleanup(c); c = NULL;

    if (total == 0) { free(result); result = NULL; }

done:
    tls_sem_give();
    return result;
}

static char *http_get(const char *url)
{
    return http_get_sized(url, HTTP_MAX_BODY);
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

    /* Every provider embeds the configured city in a URL, and a raw space
     * breaks the HTTP request line, so multi-word names like "Thousand
     * Oaks" failed to resolve on all of them without url_encode_query(). */
    char enc_city[192];
    url_encode_query(cfg->city, enc_city, sizeof(enc_city));
    char url[256];
    snprintf(url, sizeof(url), "https://wttr.in/%s?format=j1", enc_city);

    char *body = http_get_sized(url, WTTR_MAX_BODY);
    if (!body) return;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { ESP_LOGW(TAG, "wttr.in: JSON parse failed"); return; }

    /* Parse values into locals before taking the mutex */
    float  parsed_temp = 0.0f;
    float  parsed_hum  = 0.0f;
    bool   have_temp = false, have_hum = false;
    char   parsed_condition[64] = {0};
    char   parsed_icon[32] = {0};
    bool   have_desc = false;

    /* wttr.in j1: nearest_area[0].latitude / .longitude (string fields).
     * Extracting these here lets the Sunrise & Sunset panel work regardless
     * of which weather provider the user has selected. */
    float  parsed_lat = 0.0f, parsed_lon = 0.0f;
    bool   have_loc = false;
    {
        cJSON *area0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "nearest_area"), 0);
        if (area0) {
            cJSON *jlat = cJSON_GetObjectItem(area0, "latitude");
            cJSON *jlon = cJSON_GetObjectItem(area0, "longitude");
            if (jlat && jlat->valuestring && jlon && jlon->valuestring) {
                parsed_lat = (float)atof(jlat->valuestring);
                parsed_lon = (float)atof(jlon->valuestring);
                have_loc = true;
            }
        }
    }

    cJSON *cur = cJSON_GetArrayItem(
                     cJSON_GetObjectItem(root, "current_condition"), 0);
    if (cur) {
        cJSON *tc       = cJSON_GetObjectItem(cur, "temp_C");
        cJSON *hum      = cJSON_GetObjectItem(cur, "humidity");
        cJSON *desc_arr = cJSON_GetObjectItem(cur, "weatherDesc");

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
    }

    xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
    if (have_temp) s_weather.temp_c   = parsed_temp;
    if (have_hum)  s_weather.humidity = parsed_hum;
    if (have_desc) {
        strncpy(s_weather.condition, parsed_condition, sizeof(s_weather.condition) - 1);
        strncpy(s_weather.icon, parsed_icon, sizeof(s_weather.icon) - 1);
    }
    s_weather.valid = true;
    if (have_loc) {
        s_wx_lat = parsed_lat; s_wx_lon = parsed_lon;
        s_wx_location_valid = true;
    }
    xSemaphoreGive(s_wx_mutex);

    ESP_LOGI(TAG, "wttr.in: %.0f°C  %d%%  %s",
             parsed_temp, (int)parsed_hum, parsed_condition);
    if (have_loc) ESP_LOGI(TAG, "wttr.in: location %.4f, %.4f", parsed_lat, parsed_lon);
    cJSON_Delete(root);
}

/* ── OpenWeatherMap  (free-tier API key required) ───────────────────── */
static void fetch_openweather(const wx_cfg_snap_t *cfg)
{
    if (cfg->api_key[0] == '\0' || cfg->city[0] == '\0') {
        ESP_LOGW(TAG, "OpenWeatherMap: no API key or city configured");
        return;
    }

    char enc_city[192];
    url_encode_query(cfg->city, enc_city, sizeof(enc_city));
    char url[384];   /* encoded city (≤189) + key + base URL can exceed 256 */
    snprintf(url, sizeof(url),
             "https://api.openweathermap.org/data/2.5/weather"
             "?q=%s&appid=%s&units=metric",
             enc_city, cfg->api_key);

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

    /* OWM response includes "coord": {"lat": ..., "lon": ...} at the top level.
     * Extract it so the Sunrise & Sunset panel works on this provider too. */
    float parsed_lat = 0.0f, parsed_lon = 0.0f;
    bool  have_loc = false;
    {
        cJSON *coord = cJSON_GetObjectItem(root, "coord");
        if (coord) {
            cJSON *jlat = cJSON_GetObjectItem(coord, "lat");
            cJSON *jlon = cJSON_GetObjectItem(coord, "lon");
            if (jlat && jlon) {
                parsed_lat = (float)jlat->valuedouble;
                parsed_lon = (float)jlon->valuedouble;
                have_loc = true;
            }
        }
    }

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
    if (have_loc) {
        s_wx_lat = parsed_lat; s_wx_lon = parsed_lon;
        s_wx_location_valid = true;
    }
    xSemaphoreGive(s_wx_mutex);

    ESP_LOGI(TAG, "OWM: %.0f°C  %d%%  %s",
             parsed_temp, (int)parsed_hum, parsed_condition);
    if (have_loc) ESP_LOGI(TAG, "OWM: location %.4f, %.4f", parsed_lat, parsed_lon);
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
 * We split on the first comma to get the plain city name, request count=5
 * results, and pick the first whose country_code field matches the requested
 * CC (falling back to the first result if none match) — so "Airdrie,CA"
 * resolves to Airdrie, Alberta, Canada rather than Airdrie, Scotland.  The
 * Open-Meteo geocoder has no server-side country filter, so this match is done
 * client-side here.  The city name is percent-encoded so multi-word names
 * ("Thousand Oaks") resolve correctly.
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

    /* Build URL: request 5 candidates so the country_code match below can pick
     * the right one even when the API returns cities of the same name in
     * different countries.  The name MUST be percent-encoded (spaces/UTF-8). */
    char enc_name[192];
    url_encode_query(cityname, enc_name, sizeof(enc_name));
    char url[320];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search"
             "?name=%s&count=5&language=en&format=json", enc_name);

    char *body = http_get(url);
    if (!body) return false;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return false;

    bool   ok      = false;
    cJSON *results = cJSON_GetObjectItem(root, "results");
    int    n_res   = cJSON_GetArraySize(results);

    /* Prefer the first result whose country matches the requested "CC" segment,
     * accepting either the 2-letter code ("US") or the full name ("Canada") so
     * both forms the City field suggests resolve the same way the web UI's
     * validation does.  Fall back to r[0] when nothing matches. */
    cJSON *best = cJSON_GetArrayItem(results, 0);
    if (countrycode[0] && n_res > 1) {
        for (int i = 0; i < n_res; i++) {
            cJSON *r  = cJSON_GetArrayItem(results, i);
            cJSON *cc = cJSON_GetObjectItem(r, "country_code");
            cJSON *cn = cJSON_GetObjectItem(r, "country");
            if ((cc && cc->valuestring && strcasecmp(cc->valuestring, countrycode) == 0) ||
                (cn && cn->valuestring && strcasecmp(cn->valuestring, countrycode) == 0)) {
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
        /* Cache for weather_get_location() */
        xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
        s_wx_lat = s_lat; s_wx_lon = s_lon; s_wx_location_valid = true;
        xSemaphoreGive(s_wx_mutex);
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
        /* Cache for weather_get_location() */
        xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
        s_wx_lat = s_lat; s_wx_lon = s_lon; s_wx_location_valid = true;
        xSemaphoreGive(s_wx_mutex);
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

/* ── External push (POST /api/weather) ──────────────────────────────── */
/* Fields a push has supplied, so the Open-Meteo gap-fill leaves them alone while the push is fresh. */
#define EXT_WIND   0x01
#define EXT_PRECIP 0x02
#define EXT_RANGE  0x04
#define EXT_CLOUD  0x08
#define EXT_AQI_US 0x10
#define EXT_AQI_EU 0x20
#define EXT_OUTLOOK 0x40
#define EXT_PRESS  0x80
#define EXT_WINDX  0x100   /* direction and gust */
#define EXT_RAINDAY 0x200
static uint16_t s_ext_pushed     = 0;
static int64_t  s_ext_last_us    = 0;   /* time of the last push; 0 = none this boot */
static uint32_t s_ext_max_age_s  = 0;   /* 0 = readings never go stale */

/* Call with s_wx_mutex held. */
static bool ext_stale_locked(void)
{
    return s_ext_max_age_s && s_ext_last_us &&
           (esp_timer_get_time() - s_ext_last_us) > (int64_t)s_ext_max_age_s * 1000000;
}

/* True while a push supplied this field and has not gone stale. Call with s_wx_mutex held. */
static bool ext_field_fresh_locked(uint16_t bit)
{
    return (s_ext_pushed & bit) && !ext_stale_locked();
}

/* Pressure history for the Station panel's trend arrow: a sample at most every 9 min (so a 10-minute poll always adds one), kept for 3 h plus a little. Monotonic seconds, so it
 * works before the clock is set. Call with s_wx_mutex held. */
#define PTREND_N 24
static struct { int64_t t; float hpa; } s_ptrend[PTREND_N];
static int s_ptrend_n = 0;

static void pressure_note_locked(float hpa)
{
    const int64_t t = esp_timer_get_time() / 1000000;
    if (s_ptrend_n > 0 && t - s_ptrend[s_ptrend_n - 1].t < 540) {
        s_ptrend[s_ptrend_n - 1].hpa = hpa;      /* same slot: keep the newest reading */
    } else {
        if (s_ptrend_n == PTREND_N) { memmove(&s_ptrend[0], &s_ptrend[1], sizeof(s_ptrend[0]) * (PTREND_N - 1)); s_ptrend_n--; }
        s_ptrend[s_ptrend_n].t = t; s_ptrend[s_ptrend_n].hpa = hpa; s_ptrend_n++;
    }
    int old = -1;
    for (int i = 0; i < s_ptrend_n; i++) if (t - s_ptrend[i].t <= 10800) { old = i; break; }
    int trend = 0;
    if (old >= 0) {
        const int64_t span = t - s_ptrend[old].t;
        if (span >= 3600) {
            const float d3 = (hpa - s_ptrend[old].hpa) * 10800.0f / (float)span;   /* change per 3 h */
            trend = d3 >= 1.0f ? 1 : (d3 <= -1.0f ? -1 : 0);
        }
    }
    s_weather.pressure_trend = (int8_t)trend;
}

/* Opt-in setting: fill what an external push lacks from Open-Meteo. */
static bool weather_ext_fill_on(void)
{
    config_lock();
    bool on = config_get()->weather_ext_fill;
    config_unlock();
    return on;
}

/* The set of icon filenames the display knows how to render. */
static bool icon_is_known(const char *name)
{
    static const char *known[] = {
        "sun", "fewClouds", "overcastClouds", "fog",
        "rain", "snow", "squalls", "thunderstorm",
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (strcmp(name, known[i]) == 0) return true;
    return false;
}

void weather_set_external(float temp_c, float humidity,
                          const char *condition, const char *icon, int wmo_code)
{
    if (!s_wx_mutex) return;   /* weather subsystem not started */

    /* Resolve icon/condition outside the lock. */
    char res_icon[16] = {0};
    char res_cond[32] = {0};
    bool have_icon = (icon && icon[0] != '\0');
    bool have_cond = (condition && condition[0] != '\0');

    if (have_icon) {
        if (!icon_is_known(icon)) {
            ESP_LOGW(TAG, "external: unknown icon '%s' ignored", icon);
            have_icon = false;          /* keep previous icon */
        } else {
            strncpy(res_icon, icon, sizeof(res_icon) - 1);
        }
    }
    if (have_cond) strncpy(res_cond, condition, sizeof(res_cond) - 1);

    if (wmo_code >= 0) {
        if (!have_icon) { strncpy(res_icon, wmo_icon(wmo_code),      sizeof(res_icon) - 1); have_icon = true; }
        if (!have_cond) { strncpy(res_cond, wmo_condition(wmo_code), sizeof(res_cond) - 1); have_cond = true; }
    }

    xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
    if (!isnan(temp_c))   s_weather.temp_c   = temp_c;
    if (!isnan(humidity)) s_weather.humidity = humidity;
    if (have_icon) strncpy(s_weather.icon,      res_icon, sizeof(s_weather.icon)      - 1);
    if (have_cond) strncpy(s_weather.condition, res_cond, sizeof(s_weather.condition) - 1);
    s_weather.valid = true;
    s_ext_last_us = esp_timer_get_time();
    float t = s_weather.temp_c, h = s_weather.humidity;
    xSemaphoreGive(s_wx_mutex);

    ESP_LOGI(TAG, "external: %.1f°C  %d%%  %s", t, (int)h,
             have_cond ? res_cond : "(unchanged)");
}

void weather_ext_push_init(weather_ext_push_t *x)
{
    memset(x, 0, sizeof(*x));
    x->wind_kph = x->precip_mm = x->day_min_c = x->day_max_c = x->cloud_cover_pct = x->pm2_5 = NAN;
    x->us_aqi = x->european_aqi = -1;
    x->pressure_hpa = x->wind_dir_deg = x->wind_gust_kph = x->rain_today_mm = NAN;
    x->pressure_trend = 2;
    x->max_age_min = -1;
}

void weather_set_external_extras(const weather_ext_push_t *x)
{
    if (!s_wx_mutex) return;
    xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
    if (!isnan(x->wind_kph) && x->wind_kph >= 0.0f) {
        s_weather.wind_kph = x->wind_kph;
        s_ext_pushed |= EXT_WIND;
    }
    if (!isnan(x->precip_mm) && x->precip_mm >= 0.0f) {
        s_weather.precip_mm = x->precip_mm;
        s_ext_pushed |= EXT_PRECIP;
    }
    if (!isnan(x->day_min_c) && !isnan(x->day_max_c)) {
        s_weather.day_min_c = x->day_min_c <= x->day_max_c ? x->day_min_c : x->day_max_c;
        s_weather.day_max_c = x->day_min_c <= x->day_max_c ? x->day_max_c : x->day_min_c;
        s_weather.day_range_valid = true;
        s_ext_pushed |= EXT_RANGE;
    }
    if (!isnan(x->cloud_cover_pct)) {
        s_weather.cloud_cover_pct = x->cloud_cover_pct < 0.0f ? 0.0f : (x->cloud_cover_pct > 100.0f ? 100.0f : x->cloud_cover_pct);
        s_ext_pushed |= EXT_CLOUD;
    }
    if (x->us_aqi >= 0 || x->european_aqi >= 0) {
        /* The index not sent is marked -1 so the panel shows nothing rather than a stale 0 when that standard is selected. */
        if (x->us_aqi >= 0)       { s_weather.us_aqi = x->us_aqi;             s_ext_pushed |= EXT_AQI_US; }
        else if (!(s_ext_pushed & EXT_AQI_US)) s_weather.us_aqi = -1;
        if (x->european_aqi >= 0) { s_weather.european_aqi = x->european_aqi; s_ext_pushed |= EXT_AQI_EU; }
        else if (!(s_ext_pushed & EXT_AQI_EU)) s_weather.european_aqi = -1;
        s_weather.aqi_valid = true;
    }
    if (!isnan(x->pm2_5) && x->pm2_5 >= 0.0f) s_weather.pm2_5 = x->pm2_5;
    if (x->outlook_hours > 0) {
        const int n = x->outlook_hours > 24 ? 24 : x->outlook_hours;
        const time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        for (int i = 0; i < n; i++) {
            s_weather.outlook_mm[i]      = x->outlook_mm[i];
            s_weather.outlook_snow_cm[i] = x->outlook_snow_cm[i];
            s_weather.outlook_prob[i]    = x->outlook_prob[i];
        }
        s_weather.outlook_slot0 = (now > 1700000000) ? now - (time_t)(lt.tm_min * 60 + lt.tm_sec) : 0;   /* 0 = the clock is not set yet */
        s_weather.outlook_hours = (uint8_t)n;
        s_ext_pushed |= EXT_OUTLOOK;
    }
    if (!isnan(x->pressure_hpa) && x->pressure_hpa > 300.0f && x->pressure_hpa < 1200.0f) {
        s_weather.pressure_hpa = x->pressure_hpa;
        pressure_note_locked(x->pressure_hpa);
        if (x->pressure_trend >= -1 && x->pressure_trend <= 1) s_weather.pressure_trend = (int8_t)x->pressure_trend;   /* the sender knows better */
        s_ext_pushed |= EXT_PRESS;
    }
    if (!isnan(x->wind_dir_deg)) {
        float d = fmodf(x->wind_dir_deg, 360.0f);
        s_weather.wind_dir_deg = d < 0.0f ? d + 360.0f : d;
        s_ext_pushed |= EXT_WINDX;
    }
    if (!isnan(x->wind_gust_kph) && x->wind_gust_kph >= 0.0f) {
        s_weather.wind_gust_kph = x->wind_gust_kph;
        s_ext_pushed |= EXT_WINDX;
    }
    if (!isnan(x->rain_today_mm) && x->rain_today_mm >= 0.0f) {
        s_weather.rain_today_mm = x->rain_today_mm;
        s_ext_pushed |= EXT_RAINDAY;
    }
    if (x->max_age_min >= 0) s_ext_max_age_s = (uint32_t)(x->max_age_min > 10080 ? 10080 : x->max_age_min) * 60u;
    xSemaphoreGive(s_wx_mutex);
}

void weather_set_external_location(float lat, float lon)
{
    if (!s_wx_mutex) return;
    xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
    const bool moved = !s_wx_location_valid || fabsf(s_wx_lat - lat) > 0.0005f || fabsf(s_wx_lon - lon) > 0.0005f;
    s_wx_lat = lat; s_wx_lon = lon; s_wx_location_valid = true;
    xSemaphoreGive(s_wx_mutex);
    if (moved && weather_ext_fill_on()) periodic_net_poll_force("weather");   /* gap-fill for the new place now, not in 10 min */

    /* Persist to flash, but ONLY when the coordinates actually change — a fixed
     * clock's location is set once, so periodic pushes must not wear flash.
     * config_set_json merges (partial update) and saves; ~55 m epsilon. */
    bool changed;
    config_lock();
    const nextube_config_t *c = config_get();
    changed = !c->weather_ext_loc_valid ||
              fabsf(c->weather_ext_lat - lat) > 0.0005f ||
              fabsf(c->weather_ext_lon - lon) > 0.0005f;
    config_unlock();
    if (changed) {
        char j[112];
        snprintf(j, sizeof(j),
                 "{\"weather_ext_lat\":%.5f,\"weather_ext_lon\":%.5f,"
                 "\"weather_ext_loc_valid\":true}", (double)lat, (double)lon);
        config_set_json(j, strlen(j));
        ESP_LOGI(TAG, "external: location persisted %.4f, %.4f", (double)lat, (double)lon);
    } else {
        ESP_LOGI(TAG, "external: location %.4f, %.4f", (double)lat, (double)lon);
    }
}

/* True when the user has selected the external-push source.  Read live so a
 * settings change takes effect on the weather task's next loop iteration. */
static bool weather_source_is_external(void)
{
    char src[16];
    config_lock();
    strncpy(src, config_get()->weather_source, sizeof(src) - 1);
    src[sizeof(src) - 1] = '\0';
    config_unlock();
    return strcmp(src, "external") == 0;
}

/* Fetch today's forecast high/low from the keyless Open-Meteo daily endpoint,
 * using the location the active provider just geocoded (weather_get_location).
 * Provider-agnostic, so the WeatherLive tube-6 panel gets a real daily range no
 * matter which current-conditions source is selected.  No-op until a location
 * is known.
 *
 * Also pulls current wind speed, cloud cover %, and precipitation rate from
 * the same request's `current` block (the legacy `current_weather=true` flag
 * must stay off, it suppresses that block) — these feed WeatherLive's continuous cloud-density/rain-intensity rendering
 * regardless of which provider supplies the primary icon/condition text. */
static void fetch_daily_range(void)
{
    float lat, lon;
    if (!weather_get_location(&lat, &lon)) return;

    char url[448];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%.4f&longitude=%.4f"
             "&daily=temperature_2m_max,temperature_2m_min"
             "&current=wind_speed_10m,cloud_cover,precipitation,pressure_msl,wind_gusts_10m,wind_direction_10m"
             "&hourly=precipitation,snowfall,precipitation_probability&forecast_hours=24"
             "&forecast_days=1&timezone=auto",
             (double)lat, (double)lon);

    /* The hourly arrays take the answer from about 1 KB to about 2 KB; 8 KB leaves room (a body cut at HTTP_MAX_BODY would be invalid JSON
     * and lose the daily range and wind with it). */
    char *body = http_get_sized(url, 8192);
    if (!body) return;
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return;

    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    cJSON *maxa  = daily ? cJSON_GetObjectItem(daily, "temperature_2m_max") : NULL;
    cJSON *mina  = daily ? cJSON_GetObjectItem(daily, "temperature_2m_min") : NULL;
    cJSON *mx    = (maxa && cJSON_IsArray(maxa)) ? cJSON_GetArrayItem(maxa, 0) : NULL;
    cJSON *mn    = (mina && cJSON_IsArray(mina)) ? cJSON_GetArrayItem(mina, 0) : NULL;
    if (cJSON_IsNumber(mx) && cJSON_IsNumber(mn)) {
        xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
        const bool keep = ext_field_fresh_locked(EXT_RANGE);   /* a pushed range wins */
        if (!keep) {
            s_weather.day_max_c       = (float)mx->valuedouble;
            s_weather.day_min_c       = (float)mn->valuedouble;
            s_weather.day_range_valid = true;
        }
        xSemaphoreGive(s_wx_mutex);
        if (!keep)
            ESP_LOGI(TAG, "daily range: %.0f…%.0f °C",
                     (double)mn->valuedouble, (double)mx->valuedouble);
    }

    /* Current wind speed (km/h) for the WeatherLive wind animation. The request must NOT carry current_weather=true: with it Open-Meteo
     * answers with that legacy block only and leaves out the "current" block (cloud cover, precipitation, pressure, gust, direction). */
    cJSON *cur   = cJSON_GetObjectItem(root, "current");
    cJSON *ws    = cur ? cJSON_GetObjectItem(cur, "wind_speed_10m") : NULL;
    if (cJSON_IsNumber(ws)) {
        xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
        const bool keep = ext_field_fresh_locked(EXT_WIND);
        if (!keep) s_weather.wind_kph = (float)ws->valuedouble;
        xSemaphoreGive(s_wx_mutex);
        if (!keep) ESP_LOGI(TAG, "wind: %.0f km/h", (double)ws->valuedouble);
    }

    /* Cloud cover % and current precipitation rate (mm) for WeatherLive's
     * continuous sky-density rendering — see weather.h's field comments. */
    cJSON *cc    = cur ? cJSON_GetObjectItem(cur, "cloud_cover")   : NULL;
    cJSON *precp = cur ? cJSON_GetObjectItem(cur, "precipitation") : NULL;
    if (cJSON_IsNumber(cc) || cJSON_IsNumber(precp)) {
        xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
        if (cJSON_IsNumber(cc) && !ext_field_fresh_locked(EXT_CLOUD)) s_weather.cloud_cover_pct = (float)cc->valuedouble;
        if (cJSON_IsNumber(precp) && !ext_field_fresh_locked(EXT_PRECIP)) s_weather.precip_mm = (float)precp->valuedouble;
        xSemaphoreGive(s_wx_mutex);
        ESP_LOGI(TAG, "cloud cover: %.0f%%  precip: %.1f mm",
                 cJSON_IsNumber(cc)    ? cc->valuedouble    : -1.0,
                 cJSON_IsNumber(precp) ? precp->valuedouble : -1.0);
    }

    /* Station panel: sea-level pressure, gust and the direction the wind blows from. A fresh push wins. */
    cJSON *pm  = cur ? cJSON_GetObjectItem(cur, "pressure_msl")       : NULL;
    cJSON *wg  = cur ? cJSON_GetObjectItem(cur, "wind_gusts_10m")     : NULL;
    cJSON *wd  = cur ? cJSON_GetObjectItem(cur, "wind_direction_10m") : NULL;
    if (cJSON_IsNumber(pm) || cJSON_IsNumber(wg) || cJSON_IsNumber(wd)) {
        xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
        if (cJSON_IsNumber(pm) && pm->valuedouble > 300.0 && pm->valuedouble < 1200.0 && !ext_field_fresh_locked(EXT_PRESS)) {
            s_weather.pressure_hpa = (float)pm->valuedouble;
            pressure_note_locked(s_weather.pressure_hpa);
        }
        if (!ext_field_fresh_locked(EXT_WINDX)) {
            if (cJSON_IsNumber(wg)) s_weather.wind_gust_kph = (float)wg->valuedouble;
            if (cJSON_IsNumber(wd)) s_weather.wind_dir_deg  = (float)wd->valuedouble;
        }
        xSemaphoreGive(s_wx_mutex);
    }

    /* Next-24-hour precipitation outlook: slot 0 is the hour that is current now. */
    cJSON *hr = cJSON_GetObjectItem(root, "hourly");
    cJSON *hp = hr ? cJSON_GetObjectItem(hr, "precipitation") : NULL;
    xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
    const bool keep_outlook = ext_field_fresh_locked(EXT_OUTLOOK);   /* a pushed outlook wins */
    xSemaphoreGive(s_wx_mutex);
    if (cJSON_IsArray(hp) && !keep_outlook) {
        cJSON *hs = cJSON_GetObjectItem(hr, "snowfall");
        cJSON *hc = cJSON_GetObjectItem(hr, "precipitation_probability");
        int n = cJSON_GetArraySize(hp);
        if (n > 24) n = 24;
        const time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
        for (int i = 0; i < n; i++) {
            cJSON *pv = cJSON_GetArrayItem(hp, i);
            cJSON *sv = cJSON_IsArray(hs) ? cJSON_GetArrayItem(hs, i) : NULL;
            cJSON *cv = cJSON_IsArray(hc) ? cJSON_GetArrayItem(hc, i) : NULL;
            double c = cJSON_IsNumber(cv) ? cv->valuedouble : 0.0;
            if (c < 0.0) c = 0.0;
            if (c > 100.0) c = 100.0;
            s_weather.outlook_mm[i]      = cJSON_IsNumber(pv) ? (float)pv->valuedouble : 0.0f;
            s_weather.outlook_snow_cm[i] = cJSON_IsNumber(sv) ? (float)sv->valuedouble : 0.0f;
            s_weather.outlook_prob[i]    = (uint8_t)c;
        }
        s_weather.outlook_slot0  = (now > 1700000000) ? now - (time_t)(lt.tm_min * 60 + lt.tm_sec) : 0;   /* 0 = the clock is not set yet */
        /* Anchor on what the response says its first hour is, not on when it was fetched: the array need not start at the current hour, and
         * the clock's own time zone may differ from the location's. Falls back to the fetch hour above when the fields are missing. */
        cJSON *ht = cJSON_GetObjectItem(hr, "time");
        cJSON *t0 = cJSON_IsArray(ht) ? cJSON_GetArrayItem(ht, 0) : NULL;
        cJSON *uo = cJSON_GetObjectItem(root, "utc_offset_seconds");
        if (s_weather.outlook_slot0 && cJSON_IsString(t0) && cJSON_IsNumber(uo)) {
            const time_t anchored = wx_local_iso_to_epoch(t0->valuestring, (int)uo->valuedouble);
            const time_t diff = anchored > now ? anchored - now : now - anchored;
            if (anchored > 0 && diff < 36 * 3600) s_weather.outlook_slot0 = anchored;
        }
        s_weather.outlook_hours  = (uint8_t)n;
        xSemaphoreGive(s_wx_mutex);
        ESP_LOGI(TAG, "precipitation outlook: %d h", n);
    }
    cJSON_Delete(root);
}

/* Rain so far today (mm) from Open-Meteo's hourly precipitation, summed from local midnight to the current hour. Hourly values cover the hour
 * BEFORE their label, so the 00:00 entry (yesterday's last hour) is left out. It is the nearest weather model's figure, not a gauge; a
 * fresh pushed rain_today_mm wins. Same keyless host and location as fetch_daily_range(). */
static void fetch_rain_today(void)
{
    float lat, lon;
    if (!weather_get_location(&lat, &lon)) return;

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%.4f&longitude=%.4f"
             "&hourly=precipitation&past_hours=24&forecast_hours=1&timezone=auto",
             (double)lat, (double)lon);
    char *body = http_get_sized(url, 4096);
    if (!body) return;
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return;

    cJSON *hr = cJSON_GetObjectItem(root, "hourly");
    cJSON *ht = hr ? cJSON_GetObjectItem(hr, "time") : NULL;
    cJSON *hp = hr ? cJSON_GetObjectItem(hr, "precipitation") : NULL;
    const int n = (cJSON_IsArray(ht) && cJSON_IsArray(hp)) ? cJSON_GetArraySize(ht) : 0;
    cJSON *last = n > 0 ? cJSON_GetArrayItem(ht, n - 1) : NULL;
    if (cJSON_IsString(last) && strlen(last->valuestring) >= 13) {
        float sum = 0.0f;
        for (int i = 0; i < n; i++) {
            cJSON *t = cJSON_GetArrayItem(ht, i), *v = cJSON_GetArrayItem(hp, i);
            if (!cJSON_IsString(t) || !cJSON_IsNumber(v) || strlen(t->valuestring) < 13) continue;
            if (strncmp(t->valuestring, last->valuestring, 10) != 0) continue;           /* an earlier day */
            if (t->valuestring[11] == '0' && t->valuestring[12] == '0') continue;        /* 00:00 = the last hour of yesterday */
            if (v->valuedouble > 0.0) sum += (float)v->valuedouble;
        }
        xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
        const bool keep = ext_field_fresh_locked(EXT_RAINDAY);
        if (!keep) s_weather.rain_today_mm = sum;
        xSemaphoreGive(s_wx_mutex);
        if (!keep) ESP_LOGI(TAG, "rain today: %.1f mm", (double)sum);
    }
    cJSON_Delete(root);
}

/* Fetch the current US EPA (0-500) and European (0-100+) Air Quality Index plus
 * PM2.5 from the keyless Open-Meteo air-quality endpoint, using the location the
 * active provider just geocoded (weather_get_location).  Both indices come in
 * one request; weather_get_aqi() picks which to show per the configured
 * standard.  Provider-agnostic, no API key.  No-op until a location is known. */
static void fetch_air_quality(void)
{
    float lat, lon;
    if (!weather_get_location(&lat, &lon)) return;

    char url[256];
    snprintf(url, sizeof(url),
             "https://air-quality-api.open-meteo.com/v1/air-quality"
             "?latitude=%.4f&longitude=%.4f"
             "&current=us_aqi,european_aqi,pm2_5"
             "&timezone=auto",   /* align the "current" hour to local time, not GMT */
             (double)lat, (double)lon);

    /* air-quality-api.open-meteo.com is a distinct host from the forecast and
     * geocoding APIs, so its first DNS lookup is cold and can return EAI_AGAIN
     * (getaddrinfo code 202) when the lwIP resolver is briefly busy right after
     * the other fetches in this poll.  Retry a few times before giving up so the
     * panel populates on the first poll instead of waiting 10 min for the next. */
    char *body = NULL;
    for (int attempt = 0; attempt < 3 && !body; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(2000));
        body = http_get(url);
    }
    if (!body) return;
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return;

    cJSON *cur = cJSON_GetObjectItem(root, "current");
    cJSON *aqi = cur ? cJSON_GetObjectItem(cur, "us_aqi")       : NULL;
    cJSON *eaq = cur ? cJSON_GetObjectItem(cur, "european_aqi") : NULL;
    cJSON *pm  = cur ? cJSON_GetObjectItem(cur, "pm2_5")        : NULL;
    if (cJSON_IsNumber(aqi) || cJSON_IsNumber(eaq)) {
        xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
        if (cJSON_IsNumber(aqi) && !ext_field_fresh_locked(EXT_AQI_US)) s_weather.us_aqi       = (int)lroundf((float)aqi->valuedouble);
        if (cJSON_IsNumber(eaq) && !ext_field_fresh_locked(EXT_AQI_EU)) s_weather.european_aqi = (int)lroundf((float)eaq->valuedouble);
        if (cJSON_IsNumber(pm))  s_weather.pm2_5        = (float)pm->valuedouble;
        s_weather.aqi_valid = true;
        xSemaphoreGive(s_wx_mutex);
        ESP_LOGI(TAG, "air quality: US AQI %d  EU AQI %d",
                 s_weather.us_aqi, s_weather.european_aqi);
    }
    cJSON_Delete(root);
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

    /* Today's high/low for the WeatherLive tube-6 panel — keyless Open-Meteo,
     * uses the location the provider above just geocoded. */
    fetch_daily_range();

    /* Rain so far today (Station panel) — keyless Open-Meteo, same location. */
    fetch_rain_today();

    /* Air quality (US AQI) — keyless Open-Meteo, same geocoded location. */
    fetch_air_quality();
}

/* Exponential-backoff state for weather_poll_tick() below — 2 s → 4 s →
 * 8 s → … capped at 5 min, same values weather_task's own inline retry
 * loop used before periodic_net_poll.c took over owning the actual task
 * (see periodic_net_poll.h's doc comment for the merge rationale). Reset to
 * the floor once a fetch actually succeeds, so a LATER failure (provider
 * outage after hours of good fetches) starts backing off from 2 s again,
 * not from wherever the very first boot-time backoff happened to leave it. */
static uint32_t s_wx_retry_ms = 2000;

/* Called by periodic_net_poll_task() once due; returns how many ms until it
 * should be called again. Mirrors weather_task's old loop exactly: fetch,
 * and if it failed, come back sooner (backing off) instead of waiting the
 * full 10 min — the shared scheduler doesn't need to know any of this, it
 * just does whatever this function asks for next. */
static uint32_t weather_poll_tick(void)
{
    /* weather_enabled is checked live every cycle (not just once at
     * registration), so toggling it off or back on in the web UI takes
     * effect on the next tick — no restart needed in either direction.
     * weather_start() now registers this tick_fn unconditionally at boot
     * regardless of weather_enabled's value (see main.c's app_main()), so
     * there's always a registered tick_fn here to turn back on; this check
     * is what makes that no-op harmlessly instead of fetching. */
    config_lock();
    bool weather_enabled = config_get()->weather_enabled;
    config_unlock();
    if (!weather_enabled) {
        static bool logged_disabled = false;
        if (!logged_disabled) {
            ESP_LOGI(TAG, "weather_enabled=false — skipping poll");
            logged_disabled = true;
        }
        return 600000;   /* recheck in 10 min in case it's re-enabled live */
    }

    /* External-push source: data arrives via weather_set_external(); the
     * firmware must not fetch (or it would have nothing to fetch from).
     * Still checked periodically (not just once) in case weather_source
     * flips back to a real provider later without a reboot. */
    if (weather_source_is_external()) {
        static bool logged_once = false;
        if (!logged_once) {
            ESP_LOGI(TAG, "weather_source=external — awaiting POST /api/weather");
            logged_once = true;
        }
        /* Opt-in: fill what the push doesn't supply (wind, daily range, rain/snow outlook, air quality) from keyless Open-Meteo at the
         * pushed lat/lon. Both are no-ops until coordinates are known. */
        if (weather_ext_fill_on()) {
            fetch_daily_range();
            fetch_rain_today();
            fetch_air_quality();
        }
        return 600000;
    }

    /* A real provider is active: forget what an earlier push supplied so it can't shadow the provider's wind/precip/range. */
    xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
    s_ext_pushed = 0;
    s_ext_max_age_s = 0;
    xSemaphoreGive(s_wx_mutex);

    ESP_LOGI(TAG, "fetching weather...");
    fetch_weather();
    if (s_weather.valid) {
        s_wx_retry_ms = 2000;   /* reset backoff floor for any future failure */
        return 600000;          /* steady 10-minute interval */
    }

    ESP_LOGW(TAG, "Weather fetch failed, retrying in %lu s",
             (unsigned long)(s_wx_retry_ms / 1000));
    uint32_t this_retry = s_wx_retry_ms;
    if (s_wx_retry_ms < 300000)
        s_wx_retry_ms = (s_wx_retry_ms * 2 < 300000) ? s_wx_retry_ms * 2 : 300000;
    return this_retry;
}

void weather_start(void)
{
    s_wx_mutex = xSemaphoreCreateMutex();
    /* Seed location from the persisted external-push coordinates so the
     * Sunrise & Sunset panel works immediately on boot (external source),
     * before the first push of this session arrives.  Safe to write directly:
     * the task has not been created yet, so there is no concurrent reader. */
    config_lock();
    const nextube_config_t *c = config_get();
    if (c->weather_ext_loc_valid) {
        s_wx_lat = c->weather_ext_lat;
        s_wx_lon = c->weather_ext_lon;
        s_wx_location_valid = true;
        ESP_LOGI(TAG, "seeded location from saved external coords: %.4f, %.4f",
                 (double)s_wx_lat, (double)s_wx_lon);
    }
    config_unlock();
    /* No longer a dedicated task (was 6144 B, measured peak 4384 B across
     * ~2 h of uptime with every feature active) — weather_task, subscribers_
     * task, and update_check_task were three separate stacks doing the same
     * "sleep until due, do one HTTPS fetch" shape of work, already
     * serialised against each other through the shared tls_sem mutex. Now
     * one shared task (periodic_net_poll.c) does all three; this just
     * registers weather_poll_tick() with it. See periodic_net_poll.h's doc
     * comment for the full rationale — this was the task stacks-total-~143 KB-
     * against-~140-KB-usable finding that motivated the merge in the first
     * place. first_delay_ms=0: fetch immediately once the shared WiFi/DNS
     * gate clears, matching this task's original "fetch immediately on
     * WiFi ready" behavior. */
    periodic_net_poll_register(weather_poll_tick, 0, "weather");
}

/* ── Energy ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────── */
static struct {
    bool  has[4];            /* grid, solar, home, battery */
    int64_t at_us[4];        /* when each was last pushed */
    float v[5];              /* grid_w, solar_w, home_w, batt_pct, batt_w */
    bool  has_tot[4];        /* today's kWh: solar, import, export, home */
    int64_t at_tot[4];
    float tot[4];
    bool  has_batt_w;
    uint32_t max_age_s;      /* 0 = never stale */
    uint32_t seq;
} s_en;

void energy_push_init(energy_push_t *p)
{
    memset(p, 0, sizeof(*p));
    p->grid_w = p->solar_w = p->home_w = p->batt_pct = p->batt_w = NAN;
    for (int i = 0; i < 4; i++) p->tot_kwh[i] = NAN;
    p->max_age_min = -1;
}

void energy_set(const energy_push_t *p)
{
    if (!s_wx_mutex) return;
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
    const float vals[4] = { p->grid_w, p->solar_w, p->home_w, p->batt_pct };
    const bool  clr[4]  = { p->clear_grid, p->clear_solar, p->clear_home, p->clear_batt };
    for (int i = 0; i < 4; i++) {
        if (clr[i]) { s_en.has[i] = false; if (i == 3) s_en.has_batt_w = false; continue; }
        if (isnan(vals[i])) continue;
        s_en.v[i] = vals[i]; s_en.has[i] = true; s_en.at_us[i] = now;
    }
    if (!isnan(p->batt_w)) { s_en.v[4] = p->batt_w; s_en.has_batt_w = true; s_en.at_us[3] = now; s_en.has[3] = s_en.has[3] || !isnan(p->batt_pct); }
    for (int i = 0; i < 4; i++) {
        if (p->clear_tot[i]) { s_en.has_tot[i] = false; continue; }
        if (isnan(p->tot_kwh[i]) || p->tot_kwh[i] < 0.0f) continue;
        s_en.tot[i] = p->tot_kwh[i]; s_en.has_tot[i] = true; s_en.at_tot[i] = now;
    }
    if (p->clear_batt_w) s_en.has_batt_w = false;
    if (s_en.v[3] < 0.0f) s_en.v[3] = 0.0f;
    if (s_en.v[3] > 100.0f) s_en.v[3] = 100.0f;
    if (p->max_age_min >= 0) s_en.max_age_s = (uint32_t)(p->max_age_min > 10080 ? 10080 : p->max_age_min) * 60u;
    s_en.seq++;
    xSemaphoreGive(s_wx_mutex);
}

void energy_get_copy(energy_data_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_wx_mutex) return;
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
    bool st[4];
    for (int i = 0; i < 4; i++) st[i] = s_en.has[i] && s_en.max_age_s && (now - s_en.at_us[i]) > (int64_t)s_en.max_age_s * 1000000;
    out->has_grid = s_en.has[0]; out->has_solar = s_en.has[1]; out->has_home = s_en.has[2]; out->has_batt = s_en.has[3];
    out->stale_grid = st[0]; out->stale_solar = st[1]; out->stale_home = st[2]; out->stale_batt = st[3];
    out->grid_w = s_en.v[0]; out->solar_w = s_en.v[1]; out->home_w = s_en.v[2];
    out->batt_pct = s_en.v[3]; out->batt_w = s_en.v[4]; out->has_batt_w = s_en.has_batt_w;
    for (int i = 0; i < 4; i++) {
        out->has_tot[i] = s_en.has_tot[i]; out->tot_kwh[i] = s_en.tot[i];
        out->stale_tot[i] = s_en.has_tot[i] && s_en.max_age_s && (now - s_en.at_tot[i]) > (int64_t)s_en.max_age_s * 1000000;
    }
    out->seq = s_en.seq;
    xSemaphoreGive(s_wx_mutex);
}

/* An optional "ts" (epoch seconds) lets a sender date its message; one older than 10 minutes is dropped (a retained MQTT message must not look fresh). */
static bool push_too_old(cJSON *root)
{
    cJSON *ts = cJSON_GetObjectItem(root, "ts");
    const time_t now = time(NULL);
    if (!cJSON_IsNumber(ts) || now < 1700000000) return false;     /* no date, or our own clock is not set yet */
    return (double)now - ts->valuedouble > 600.0;
}

static void energy_field(cJSON *root, const char *w_key, const char *kw_key, float *out, bool *clear)
{
    cJSON *a = cJSON_GetObjectItem(root, w_key), *b = cJSON_GetObjectItem(root, kw_key);
    if (cJSON_IsNull(a) || cJSON_IsNull(b)) { *clear = true; return; }
    if (cJSON_IsNumber(a))      *out = (float)a->valuedouble;
    else if (cJSON_IsNumber(b)) *out = (float)b->valuedouble * 1000.0f;
}

bool energy_set_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    if (push_too_old(root)) { cJSON_Delete(root); ESP_LOGW(TAG, "energy: message older than 10 min ignored"); return false; }
    energy_push_t p;
    energy_push_init(&p);
    energy_field(root, "grid_w",  "grid_kw",  &p.grid_w,  &p.clear_grid);
    energy_field(root, "solar_w", "solar_kw", &p.solar_w, &p.clear_solar);
    energy_field(root, "home_w",  "home_kw",  &p.home_w,  &p.clear_home);
    {   /* today's totals, kWh (the sensors Home Assistant resets at midnight) */
        static const char *const k[4] = { "solar_today_kwh", "grid_import_today_kwh", "grid_export_today_kwh", "home_today_kwh" };
        for (int i = 0; i < 4; i++) {
            cJSON *t = cJSON_GetObjectItem(root, k[i]);
            if (cJSON_IsNull(t)) p.clear_tot[i] = true;
            else if (cJSON_IsNumber(t)) p.tot_kwh[i] = (float)t->valuedouble;
        }
    }
    bool clear_bw = false;
    energy_field(root, "battery_w", "battery_kw", &p.batt_w, &clear_bw);
    cJSON *pc = cJSON_GetObjectItem(root, "battery_pct");
    if (cJSON_IsNull(pc)) p.clear_batt = true;
    else if (cJSON_IsNumber(pc)) p.batt_pct = (float)pc->valuedouble;
    p.clear_batt_w = clear_bw;
    cJSON *ma = cJSON_GetObjectItem(root, "max_age_min");
    if (cJSON_IsNumber(ma) && ma->valuedouble >= 0) p.max_age_min = ma->valuedouble > 10080 ? 10080 : (int)ma->valuedouble;
    cJSON_Delete(root);
    energy_set(&p);
    return true;
}

bool weather_set_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    if (push_too_old(root)) { cJSON_Delete(root); ESP_LOGW(TAG, "weather: message older than 10 min ignored"); return false; }

    /* Temperature: accept temp_c (Celsius) or temp_f (Fahrenheit).  Stored
     * internally as Celsius, so temp_f is converted on ingest.  Send only one;
     * temp_c takes precedence if both are present. */
    float temp = NAN, hum = NAN;
    cJSON *jt = cJSON_GetObjectItem(root, "temp_c");
    cJSON *jf = cJSON_GetObjectItem(root, "temp_f");
    cJSON *jh = cJSON_GetObjectItem(root, "humidity");
    if (cJSON_IsNumber(jt))      temp = (float)jt->valuedouble;
    else if (cJSON_IsNumber(jf)) temp = ((float)jf->valuedouble - 32.0f) * 5.0f / 9.0f;
    if (cJSON_IsNumber(jh)) hum  = (float)jh->valuedouble;

    const char *cond = NULL, *icon = NULL;
    cJSON *jc = cJSON_GetObjectItem(root, "condition");
    cJSON *ji = cJSON_GetObjectItem(root, "icon");
    if (cJSON_IsString(jc)) cond = jc->valuestring;
    if (cJSON_IsString(ji)) icon = ji->valuestring;

    int wmo = -1;
    cJSON *jw = cJSON_GetObjectItem(root, "weather_code");
    if (cJSON_IsNumber(jw)) wmo = jw->valueint;

    weather_set_external(temp, hum, cond, icon, wmo);

    /* Optional extras: wind (km/h, mph or m/s), rain rate (mm/h), today's min/max, cloud cover, air quality, 24-hour outlook, stale-after age. */
    weather_ext_push_t x;
    weather_ext_push_init(&x);
    cJSON *jwk = cJSON_GetObjectItem(root, "wind_kph");
    cJSON *jwm = cJSON_GetObjectItem(root, "wind_mph");
    cJSON *jws = cJSON_GetObjectItem(root, "wind_ms");
    if      (cJSON_IsNumber(jwk)) x.wind_kph = (float)jwk->valuedouble;
    else if (cJSON_IsNumber(jwm)) x.wind_kph = (float)jwm->valuedouble * 1.609344f;
    else if (cJSON_IsNumber(jws)) x.wind_kph = (float)jws->valuedouble * 3.6f;
    cJSON *jpr = cJSON_GetObjectItem(root, "precip_mm");
    if (cJSON_IsNumber(jpr)) x.precip_mm = (float)jpr->valuedouble;
    cJSON *jmn = cJSON_GetObjectItem(root, "day_min_c");
    cJSON *jmx = cJSON_GetObjectItem(root, "day_max_c");
    if (cJSON_IsNumber(jmn) && cJSON_IsNumber(jmx)) { x.day_min_c = (float)jmn->valuedouble; x.day_max_c = (float)jmx->valuedouble; }
    cJSON *jcc = cJSON_GetObjectItem(root, "cloud_cover_pct");
    if (cJSON_IsNumber(jcc)) x.cloud_cover_pct = (float)jcc->valuedouble;
    cJSON *jua = cJSON_GetObjectItem(root, "us_aqi");
    cJSON *jea = cJSON_GetObjectItem(root, "european_aqi");
    cJSON *jpm = cJSON_GetObjectItem(root, "pm2_5");
    if (cJSON_IsNumber(jua) && jua->valuedouble >= 0) x.us_aqi       = (int)lround(jua->valuedouble);
    if (cJSON_IsNumber(jea) && jea->valuedouble >= 0) x.european_aqi = (int)lround(jea->valuedouble);
    if (cJSON_IsNumber(jpm)) x.pm2_5 = (float)jpm->valuedouble;
    cJSON *jom = cJSON_GetObjectItem(root, "outlook_mm");
    if (cJSON_IsArray(jom)) {
        int cnt = cJSON_GetArraySize(jom);
        if (cnt > 24) cnt = 24;
        cJSON *jos = cJSON_GetObjectItem(root, "outlook_snow_cm");
        cJSON *jop = cJSON_GetObjectItem(root, "outlook_prob");
        for (int i = 0; i < cnt; i++) {
            cJSON *v = cJSON_GetArrayItem(jom, i);
            cJSON *sv = cJSON_IsArray(jos) ? cJSON_GetArrayItem(jos, i) : NULL;
            cJSON *pv = cJSON_IsArray(jop) ? cJSON_GetArrayItem(jop, i) : NULL;
            double mm = cJSON_IsNumber(v) ? v->valuedouble : 0.0;
            double pc = cJSON_IsNumber(pv) ? pv->valuedouble : 0.0;
            x.outlook_mm[i]      = mm < 0.0 ? 0.0f : (float)mm;
            x.outlook_snow_cm[i] = (cJSON_IsNumber(sv) && sv->valuedouble > 0.0) ? (float)sv->valuedouble : 0.0f;
            x.outlook_prob[i]    = (uint8_t)(pc < 0.0 ? 0.0 : (pc > 100.0 ? 100.0 : pc));
        }
        x.outlook_hours = (uint8_t)cnt;
    }
    /* Station panel: pressure (hPa or inHg) with an optional trend, wind direction, gust (km/h, mph or m/s), rain so far today (mm or in). */
    cJSON *jph = cJSON_GetObjectItem(root, "pressure_hpa");
    cJSON *jpi = cJSON_GetObjectItem(root, "pressure_inhg");
    cJSON *jpk = cJSON_GetObjectItem(root, "pressure_kpa");
    if      (cJSON_IsNumber(jph)) x.pressure_hpa = (float)jph->valuedouble;
    else if (cJSON_IsNumber(jpi)) x.pressure_hpa = (float)jpi->valuedouble * 33.8639f;
    else if (cJSON_IsNumber(jpk)) x.pressure_hpa = (float)jpk->valuedouble * 10.0f;
    cJSON *jpt = cJSON_GetObjectItem(root, "pressure_trend");
    if (cJSON_IsString(jpt)) {
        if      (!strcmp(jpt->valuestring, "rising"))  x.pressure_trend = 1;
        else if (!strcmp(jpt->valuestring, "falling")) x.pressure_trend = -1;
        else if (!strcmp(jpt->valuestring, "steady"))  x.pressure_trend = 0;
    }
    cJSON *jwd = cJSON_GetObjectItem(root, "wind_dir_deg");
    if (cJSON_IsNumber(jwd)) x.wind_dir_deg = (float)jwd->valuedouble;
    cJSON *jgk = cJSON_GetObjectItem(root, "wind_gust_kph");
    cJSON *jgm = cJSON_GetObjectItem(root, "wind_gust_mph");
    cJSON *jgs = cJSON_GetObjectItem(root, "wind_gust_ms");
    if      (cJSON_IsNumber(jgk)) x.wind_gust_kph = (float)jgk->valuedouble;
    else if (cJSON_IsNumber(jgm)) x.wind_gust_kph = (float)jgm->valuedouble * 1.609344f;
    else if (cJSON_IsNumber(jgs)) x.wind_gust_kph = (float)jgs->valuedouble * 3.6f;
    cJSON *jrm = cJSON_GetObjectItem(root, "rain_today_mm");
    cJSON *jri = cJSON_GetObjectItem(root, "rain_today_in");
    if      (cJSON_IsNumber(jrm)) x.rain_today_mm = (float)jrm->valuedouble;
    else if (cJSON_IsNumber(jri)) x.rain_today_mm = (float)jri->valuedouble * 25.4f;
    cJSON *jma = cJSON_GetObjectItem(root, "max_age_min");
    if (cJSON_IsNumber(jma) && jma->valuedouble >= 0) x.max_age_min = jma->valuedouble > 10080 ? 10080 : (int)jma->valuedouble;
    weather_set_external_extras(&x);

    cJSON *jlat = cJSON_GetObjectItem(root, "lat");
    cJSON *jlon = cJSON_GetObjectItem(root, "lon");
    if (cJSON_IsNumber(jlat) && cJSON_IsNumber(jlon))
        weather_set_external_location((float)jlat->valuedouble, (float)jlon->valuedouble);

    cJSON_Delete(root);
    return true;
}

void weather_get_copy(weather_data_t *out)
{
    if (s_wx_mutex) xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
    *out = s_weather;
    if (s_wx_mutex && ext_stale_locked()) {   /* pushed readings older than max_age_min: show them as missing */
        out->valid = false;
        out->day_range_valid = false;
        out->wind_kph = 0.0f;
        out->precip_mm = -1.0f;
        out->pressure_hpa = out->wind_dir_deg = out->wind_gust_kph = out->rain_today_mm = -1.0f;
    }
    if (s_wx_mutex) xSemaphoreGive(s_wx_mutex);
}

/* The display task's view: one shared buffer, so only that task may use it (others call weather_get_copy()). */
const weather_data_t *weather_get(void)
{
    static weather_data_t copy;
    weather_get_copy(&copy);
    return &copy;
}

bool weather_get_location(float *lat, float *lon)
{
    if (!s_wx_mutex) return false;
    xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
    bool ok = s_wx_location_valid;
    if (ok) { *lat = s_wx_lat; *lon = s_wx_lon; }
    xSemaphoreGive(s_wx_mutex);
    return ok;
}

bool weather_get_outlook(weather_outlook_t *o)
{
    memset(o, 0, sizeof(*o));
    if (!s_wx_mutex) return false;
    const time_t now = time(NULL);
    time_t base = 0;   /* start of slot 0 of *o */
    xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
    const int hours = ext_stale_locked() ? 0 : s_weather.outlook_hours;
    const time_t slot0 = s_weather.outlook_slot0;
    if (hours > 0 && slot0 > 0) {
        long shift = (now > slot0) ? (long)((now - slot0) / 3600) : 0;
        if (shift < hours) {
            const int n = hours - (int)shift;
            o->valid = true;
            o->hours = (uint8_t)n;
            base = slot0 + (time_t)shift * 3600;
            for (int i = 0; i < n; i++) {
                o->mm[i]   = s_weather.outlook_mm[shift + i];
                o->snow[i] = s_weather.outlook_snow_cm[shift + i] > 0.0f;
                o->prob[i] = s_weather.outlook_prob[shift + i];
            }
        }
    }
    xSemaphoreGive(s_wx_mutex);
    if (!o->valid) return false;

    struct tm lt;
    localtime_r(&now, &lt);
    outlook_summarize(o, base, now, (long)lt.tm_hour * 3600L + lt.tm_min * 60L + lt.tm_sec);   /* leaves o->event false when it stays dry */
    return true;
}

int weather_get_aqi(bool *is_european)
{
    /* Resolve the standard: explicit "us"/"eu", or "auto" → European inside the
     * Europe bounding box (lat 34..72, lon -25..45), US elsewhere. */
    char std[8] = {0};
    config_lock();
    strncpy(std, config_get()->aqi_standard, sizeof(std) - 1);
    config_unlock();

    bool eu;
    if      (strcmp(std, "eu") == 0) eu = true;
    else if (strcmp(std, "us") == 0) eu = false;
    else {
        float lat, lon;
        eu = false;
        if (weather_get_location(&lat, &lon))
            eu = (lat >= 34.0f && lat <= 72.0f && lon >= -25.0f && lon <= 45.0f);
    }
    if (is_european) *is_european = eu;

    if (!s_wx_mutex) return -1;
    xSemaphoreTake(s_wx_mutex, portMAX_DELAY);
    bool valid = s_weather.aqi_valid && !ext_stale_locked();
    int  v     = eu ? s_weather.european_aqi : s_weather.us_aqi;
    xSemaphoreGive(s_wx_mutex);
    return valid ? v : -1;   /* v is -1 too when a push carried only the other standard */
}
