#include "youtube_bili.h"
#include "config_mgr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include "esp_crt_bundle.h"

static const char *TAG = "yt_bili";
static sub_count_t       s_sub       = {0};
static SemaphoreHandle_t s_sub_mutex = NULL;

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
    char youtube_id[48], youtube_key[48];
    config_lock();
    const nextube_config_t *cfg = config_get();
    strncpy(youtube_id, cfg->youtube_id,  sizeof(youtube_id)  - 1); youtube_id[sizeof(youtube_id)   - 1] = '\0';
    strncpy(youtube_key, cfg->youtube_key, sizeof(youtube_key) - 1); youtube_key[sizeof(youtube_key) - 1] = '\0';
    config_unlock();
    if (youtube_key[0] == '\0' || youtube_id[0] == '\0') return;

    char url[512];
    snprintf(url, sizeof(url),
        "https://www.googleapis.com/youtube/v3/channels?part=statistics&id=%s&key=%s",
        youtube_id, youtube_key);

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
                if (sc && sc->valuestring) {
                    uint32_t count = (uint32_t)atoi(sc->valuestring);
                    xSemaphoreTake(s_sub_mutex, portMAX_DELAY);
                    s_sub.subscriber_count = count;
                    s_sub.valid = true;
                    xSemaphoreGive(s_sub_mutex);
                }
            }
            cJSON_Delete(root);
        }
    }
    esp_http_client_cleanup(client);
}

static void fetch_bilibili(void)
{
    char bili_uid[24];
    config_lock();
    strncpy(bili_uid, config_get()->bili_uid, sizeof(bili_uid) - 1);
    bili_uid[sizeof(bili_uid) - 1] = '\0';
    config_unlock();
    if (bili_uid[0] == '\0') return;

    char url[256];
    snprintf(url, sizeof(url),
        "https://api.bilibili.com/x/web-interface/card?mid=%s", bili_uid);

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
            if (fans) {
                xSemaphoreTake(s_sub_mutex, portMAX_DELAY);
                s_sub.subscriber_count = (uint32_t)fans->valueint;
                s_sub.valid = true;
                xSemaphoreGive(s_sub_mutex);
            }
            cJSON_Delete(root);
        }
    }
    esp_http_client_cleanup(client);
}

static void yt_bili_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(20000));
    while (1) {
        char video_site[16], bili_uid[24], youtube_id[48], youtube_key[48];
        config_lock();
        const nextube_config_t *cfg = config_get();
        strncpy(video_site,  cfg->video_site,  sizeof(video_site)  - 1); video_site[sizeof(video_site)   - 1] = '\0';
        strncpy(bili_uid,    cfg->bili_uid,    sizeof(bili_uid)    - 1); bili_uid[sizeof(bili_uid)       - 1] = '\0';
        strncpy(youtube_id,  cfg->youtube_id,  sizeof(youtube_id)  - 1); youtube_id[sizeof(youtube_id)   - 1] = '\0';
        strncpy(youtube_key, cfg->youtube_key, sizeof(youtube_key) - 1); youtube_key[sizeof(youtube_key) - 1] = '\0';
        config_unlock();

        bool is_bili    = (strcmp(video_site, "bilibili") == 0);
        bool configured = is_bili ? (bili_uid[0] != '\0')
                                  : (youtube_id[0] != '\0' && youtube_key[0] != '\0');
        if (configured) {
            if (is_bili) fetch_bilibili();
            else         fetch_youtube();
        } else {
            ESP_LOGD(TAG, "Not configured — skipping fetch");
        }
        vTaskDelay(pdMS_TO_TICKS(1800000)); /* Every 30 minutes */
    }
}

void youtube_bili_start(void)
{
    s_sub_mutex = xSemaphoreCreateMutex();
    xTaskCreate(yt_bili_task, "yt_bili", 8192, NULL, 3, NULL);
}

const sub_count_t *youtube_bili_get(void)
{
    static sub_count_t copy;
    if (s_sub_mutex) {
        xSemaphoreTake(s_sub_mutex, portMAX_DELAY);
        copy = s_sub;
        xSemaphoreGive(s_sub_mutex);
    } else {
        copy = s_sub;
    }
    return &copy;
}
