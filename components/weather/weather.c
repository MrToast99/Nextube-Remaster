#include "weather.h"
#include "config_mgr.h"
#include "fw_version.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "weather";
static weather_data_t s_weather = {0};

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
    esp_http_client_config_t hcfg = {
        .url               = url,
        .timeout_ms        = 10000,
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
static void fetch_wttr(const nextube_config_t *cfg)
{
    if (strlen(cfg->city) == 0) return;

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
        if (tc  && tc->valuestring)
            s_weather.temp_c   = (float)atof(tc->valuestring);
        if (hum && hum->valuestring)
            s_weather.humidity = (float)atof(hum->valuestring);
        cJSON *desc0 = cJSON_GetArrayItem(desc_arr, 0);
        if (desc0) {
            cJSON *val = cJSON_GetObjectItem(desc0, "value");
            if (val && val->valuestring)
                strncpy(s_weather.condition, val->valuestring,
                        sizeof(s_weather.condition) - 1);
        }
        s_weather.valid = true;
        ESP_LOGI(TAG, "wttr.in: %.1f°C  %d%%  %s",
                 s_weather.temp_c, (int)s_weather.humidity, s_weather.condition);
    }
    cJSON_Delete(root);
}

/* ── OpenWeatherMap  (free-tier API key required) ───────────────────── */
static void fetch_openweather(const nextube_config_t *cfg)
{
    if (strlen(cfg->weather_api_key) == 0 || strlen(cfg->city) == 0) {
        ESP_LOGW(TAG, "OpenWeatherMap: no API key or city configured");
        return;
    }

    char url[256];
    snprintf(url, sizeof(url),
             "http://api.openweathermap.org/data/2.5/weather"
             "?q=%s&appid=%s&units=metric",
             cfg->city, cfg->weather_api_key);

    char *body = http_get(url);
    if (!body) return;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { ESP_LOGW(TAG, "OWM: JSON parse failed"); return; }

    cJSON *main_obj = cJSON_GetObjectItem(root, "main");
    if (main_obj) {
        cJSON *temp = cJSON_GetObjectItem(main_obj, "temp");
        cJSON *hum  = cJSON_GetObjectItem(main_obj, "humidity");
        if (temp) s_weather.temp_c   = (float)temp->valuedouble;
        if (hum)  s_weather.humidity = (float)hum->valuedouble;
    }
    cJSON *w0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "weather"), 0);
    if (w0) {
        cJSON *desc = cJSON_GetObjectItem(w0, "main");
        cJSON *icon = cJSON_GetObjectItem(w0, "icon");
        if (desc && desc->valuestring)
            strncpy(s_weather.condition, desc->valuestring,
                    sizeof(s_weather.condition) - 1);
        if (icon && icon->valuestring)
            strncpy(s_weather.icon, icon->valuestring,
                    sizeof(s_weather.icon) - 1);
    }
    s_weather.valid = true;
    ESP_LOGI(TAG, "OWM: %.1f°C  %d%%  %s",
             s_weather.temp_c, (int)s_weather.humidity, s_weather.condition);
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
static const char *wmo_icon(int code)
{
    if (code == 0)        return "sunny";
    if (code <= 2)        return "partlycloudy";
    if (code == 3)        return "overcast";
    if (code <= 48)       return "foggy";
    if (code <= 57)       return "drizzle";
    if (code <= 67)       return "rainy";
    if (code <= 77)       return "snowy";
    if (code <= 82)       return "showery";
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

/* Geocode city → lat/lon via Open-Meteo geocoding API.
 * Result is cached; geocoding only runs again when the city changes.
 *
 * The city string may be in "City,CC" format (e.g. "Airdrie,CA") as used by
 * wttr.in.  Open-Meteo's geocoding API expects a plain city name; passing the
 * country code as part of the name returns zero results.  We strip everything
 * after the first comma and use only the city portion for the API call. */
static bool geocode_open_meteo(const char *city, float *lat, float *lon)
{
    /* Extract just the city name (before any comma) */
    char cityname[64];
    strncpy(cityname, city, sizeof(cityname) - 1);
    cityname[sizeof(cityname) - 1] = '\0';
    char *comma = strchr(cityname, ',');
    if (comma) *comma = '\0';

    char url[256];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search"
             "?name=%s&count=1&format=json", cityname);

    char *body = http_get(url);
    if (!body) return false;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return false;

    bool ok = false;
    cJSON *r0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "results"), 0);
    if (r0) {
        cJSON *la = cJSON_GetObjectItem(r0, "latitude");
        cJSON *lo = cJSON_GetObjectItem(r0, "longitude");
        if (la && lo) { *lat = (float)la->valuedouble; *lon = (float)lo->valuedouble; ok = true; }
    }
    cJSON_Delete(root);
    return ok;
}

static void fetch_open_meteo(const nextube_config_t *cfg)
{
    if (strlen(cfg->city) == 0) {
        ESP_LOGW(TAG, "Open-Meteo: no city configured"); return;
    }

    /* Cache lat/lon; only re-geocode when city changes */
    static float s_lat = 0.0f, s_lon = 0.0f;
    static char  s_last_city[64] = {0};

    if (strncmp(cfg->city, s_last_city, sizeof(s_last_city)) != 0) {
        if (!geocode_open_meteo(cfg->city, &s_lat, &s_lon)) {
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
        if (temp) s_weather.temp_c   = (float)temp->valuedouble;
        if (hum)  s_weather.humidity = (float)hum->valuedouble;
        if (wc) {
            int code = wc->valueint;
            strncpy(s_weather.icon,      wmo_icon(code),      sizeof(s_weather.icon) - 1);
            strncpy(s_weather.condition, wmo_condition(code),  sizeof(s_weather.condition) - 1);
        }
        s_weather.valid = true;
        ESP_LOGI(TAG, "Open-Meteo: %.1f°C  %d%%  %s",
                 s_weather.temp_c, (int)s_weather.humidity, s_weather.condition);
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

    if (strcmp(stem, "clearsky")        == 0) return "sunny";
    if (strcmp(stem, "fair")            == 0) return "sunny";
    if (strcmp(stem, "partlycloudy")    == 0) return "partlycloudy";
    if (strcmp(stem, "cloudy")          == 0) return "overcast";
    if (strcmp(stem, "fog")             == 0) return "foggy";
    if (strcmp(stem, "lightdrizzle")    == 0) return "drizzle";
    if (strcmp(stem, "drizzle")         == 0) return "drizzle";
    if (strcmp(stem, "heavydrizzle")    == 0) return "drizzle";
    if (strcmp(stem, "lightrain")       == 0) return "rainy";
    if (strcmp(stem, "rain")            == 0) return "rainy";
    if (strcmp(stem, "heavyrain")       == 0) return "rainy";
    if (strcmp(stem, "lightsleet")      == 0) return "rainy";
    if (strcmp(stem, "sleet")           == 0) return "rainy";
    if (strcmp(stem, "heavysleet")      == 0) return "rainy";
    if (strcmp(stem, "lightsnow")       == 0) return "snowy";
    if (strcmp(stem, "snow")            == 0) return "snowy";
    if (strcmp(stem, "heavysnow")       == 0) return "snowy";
    if (strncmp(stem, "rainshowers",   11) == 0) return "showery";
    if (strncmp(stem, "sleetshowers",  12) == 0) return "showery";
    if (strncmp(stem, "snowshowers",   11) == 0) return "showery";
    if (strstr(stem,  "thunder")           != NULL) return "thunderstorm";
    return "overcast";
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
 * Finds the first occurrence of "key": and returns atof of the value. */
static bool json_extract_number(const char *buf, const char *key, float *out)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(buf, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (!*p) return false;
    *out = (float)atof(p);
    return true;
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

static void fetch_met_no(const nextube_config_t *cfg)
{
    if (strlen(cfg->city) == 0) {
        ESP_LOGW(TAG, "Met.no: no city configured"); return;
    }

    /* Reuse Open-Meteo geocoding cache for lat/lon */
    static float s_lat = 0.0f, s_lon = 0.0f;
    static char  s_last_city[64] = {0};

    if (strncmp(cfg->city, s_last_city, sizeof(s_last_city)) != 0) {
        if (!geocode_open_meteo(cfg->city, &s_lat, &s_lon)) {
            ESP_LOGW(TAG, "Met.no: geocoding failed for '%s'", cfg->city);
            return;
        }
        strncpy(s_last_city, cfg->city, sizeof(s_last_city) - 1);
        ESP_LOGI(TAG, "Met.no: geocoded '%s' → %.4f, %.4f", cfg->city, s_lat, s_lon);
    }

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.met.no/weatherapi/locationforecast/2.0/compact"
             "?lat=%.4f&lon=%.4f",
             s_lat, s_lon);

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

    if (got_temp) s_weather.temp_c   = temp;
    if (got_hum)  s_weather.humidity = hum;
    if (got_sym) {
        strncpy(s_weather.icon,      metno_icon(symbol),      sizeof(s_weather.icon) - 1);
        strncpy(s_weather.condition, metno_condition(symbol),  sizeof(s_weather.condition) - 1);
    }
    s_weather.valid = true;
    ESP_LOGI(TAG, "Met.no: %.1f°C  %d%%  %s",
             s_weather.temp_c, (int)s_weather.humidity, s_weather.condition);
}

/* ── Task ───────────────────────────────────────────────────────────── */
static void fetch_weather(void)
{
    const nextube_config_t *cfg = config_get();

    if (strcmp(cfg->weather_source, "openmeteo") == 0) {
        fetch_open_meteo(cfg);
    } else if (strcmp(cfg->weather_source, "metno") == 0) {
        fetch_met_no(cfg);
    } else if (strcmp(cfg->weather_source, "openweather") == 0 &&
               strlen(cfg->weather_api_key) > 0) {
        fetch_openweather(cfg);
    } else {
        fetch_wttr(cfg);   /* default: wttr.in, no key needed */
    }
}

static void weather_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(15000));   /* wait for WiFi */
    while (1) {
        fetch_weather();
        vTaskDelay(pdMS_TO_TICKS(600000));  /* every 10 minutes */
    }
}

void weather_start(void) { xTaskCreate(weather_task, "weather", 8192, NULL, 3, NULL); }
const weather_data_t *weather_get(void) { return &s_weather; }
