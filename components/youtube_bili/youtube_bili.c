#include "youtube_bili.h"
#include "config_mgr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_crt_bundle.h"

static const char *TAG = "yt_bili";
static sub_count_t s_sub = {0};

static char s_http_buf[2048];
static int s_http_buf_len = 0;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && !esp_http_client_is_chunked_response(evt->client)) {
        int copy = evt->data_len;
        if (s_http_buf_len + copy >= (int)sizeof(s_http_buf)) copy = sizeof(s_http_buf) - s_http_buf_len - 1;
        if (copy > 0) { memcpy(s_http_buf + s_http_buf_len, evt->data, copy); s_http_buf_len += copy; }
    }
    return ESP_OK;
}

static void fetch_youtube(void)
{
    const nextube_config_t *cfg = config_get();
    if (strlen(cfg->youtube_key) == 0 || strlen(cfg->youtube_id) == 0) return;

    char url[512];
    snprintf(url, sizeof(url),
        "https://www.googleapis.com/youtube/v3/channels?part=statistics&id=%s&key=%s",
        cfg->youtube_id, cfg->youtube_key);

    s_http_buf_len = 0;
    esp_http_client_config_t http_cfg = {
        .url = url, .event_handler = http_event, .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200) {
        s_http_buf[s_http_buf_len] = 0;
        cJSON *root = cJSON_Parse(s_http_buf);
        if (root) {
            cJSON *items = cJSON_GetObjectItem(root, "items");
            if (cJSON_IsArray(items) && cJSON_GetArraySize(items) > 0) {
                cJSON *stats = cJSON_GetObjectItem(cJSON_GetArrayItem(items, 0), "statistics");
                cJSON *sc = cJSON_GetObjectItem(stats, "subscriberCount");
                if (sc && sc->valuestring) s_sub.subscriber_count = atoi(sc->valuestring);
                s_sub.valid = true;
            }
            cJSON_Delete(root);
        }
    }
    esp_http_client_cleanup(client);
}

static void fetch_bilibili(void)
{
    const nextube_config_t *cfg = config_get();
    if (strlen(cfg->bili_uid) == 0) return;

    char url[256];
    snprintf(url, sizeof(url),
        "https://api.bilibili.com/x/web-interface/card?mid=%s", cfg->bili_uid);

    s_http_buf_len = 0;
    esp_http_client_config_t http_cfg = {
        .url = url, .event_handler = http_event, .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200) {
        s_http_buf[s_http_buf_len] = 0;
        cJSON *root = cJSON_Parse(s_http_buf);
        if (root) {
            cJSON *data = cJSON_GetObjectItem(root, "data");
            cJSON *card = cJSON_GetObjectItem(data, "card");
            cJSON *fans = cJSON_GetObjectItem(card, "fans");
            if (fans) { s_sub.subscriber_count = fans->valueint; s_sub.valid = true; }
            cJSON_Delete(root);
        }
    }
    esp_http_client_cleanup(client);
}

static void yt_bili_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(20000));
    while (1) {
        const nextube_config_t *cfg = config_get();
        if (strcmp(cfg->video_site, "bilibili") == 0) fetch_bilibili();
        else fetch_youtube();
        ESP_LOGI(TAG, "Subscriber count: %lu (valid=%d)", (unsigned long)s_sub.subscriber_count, s_sub.valid);
        vTaskDelay(pdMS_TO_TICKS(300000));  /* Every 5 minutes */
    }
}

void youtube_bili_start(void) { xTaskCreate(yt_bili_task, "yt_bili", 8192, NULL, 3, NULL); }
const sub_count_t *youtube_bili_get(void) { return &s_sub; }
