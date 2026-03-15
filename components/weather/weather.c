#include "weather.h"
#include "config_mgr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "weather";
static weather_data_t s_weather = {0};

/* ── HTTP helper ────────────────────────────────────────────────────── */
/* Returns heap-allocated NUL-terminated body, or NULL on error. Caller frees. */
static char *http_get(const char *url)
{
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 10000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);

    if (esp_http_client_perform(c) != ESP_OK ||
        esp_http_client_get_status_code(c) != 200) {
        ESP_LOGW(TAG, "HTTP GET failed: %s", url);
        esp_http_client_cleanup(c);
        return NULL;
    }

    int len = esp_http_client_get_content_length(c);
    if (len <= 0 || len > 4096) {
        esp_http_client_cleanup(c);
        return NULL;
    }
    char *buf = malloc(len + 1);
    esp_http_client_read(c, buf, len);
    buf[len] = '\0';
    esp_http_client_cleanup(c);
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

/* ── Task ───────────────────────────────────────────────────────────── */
static void fetch_weather(void)
{
    const nextube_config_t *cfg = config_get();

    /* Use wttr.in when:
     *   - weather_source == "wttr"  (default, no key needed), OR
     *   - source is "openweather" but no API key has been entered yet */
    bool use_wttr = (strcmp(cfg->weather_source, "openweather") != 0 ||
                     strlen(cfg->weather_api_key) == 0);

    if (use_wttr)
        fetch_wttr(cfg);
    else
        fetch_openweather(cfg);
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
