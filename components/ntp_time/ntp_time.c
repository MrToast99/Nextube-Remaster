#include "ntp_time.h"
#include "config_mgr.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ntp";
static bool s_synced = false;

static void time_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "NTP time synchronised");
    s_synced = true;
}

static void ntp_task(void *arg)
{
    const nextube_config_t *cfg = config_get();

    /* Wait for WiFi */
    vTaskDelay(pdMS_TO_TICKS(5000));

    /* Set timezone */
    char tz[32];
    int hrs = cfg->time_zone / 3600;
    int mins = abs((cfg->time_zone % 3600) / 60);
    snprintf(tz, sizeof(tz), "UTC%+d:%02d", -hrs, mins);
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s (offset=%ld)", tz, (long)cfg->time_zone);

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, cfg->ntp_server);
    sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

void ntp_apply_timezone(void)
{
    const nextube_config_t *cfg = config_get();
    int hrs  = cfg->time_zone / 3600;
    int mins = abs((cfg->time_zone % 3600) / 60);
    char tz[32];
    snprintf(tz, sizeof(tz), "UTC%+d:%02d", -hrs, mins);
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone updated to %s (offset=%ld s)", tz, (long)cfg->time_zone);
}

void ntp_time_start(void)
{
    xTaskCreate(ntp_task, "ntp", 4096, NULL, 5, NULL);
}

bool ntp_time_synced(void) { return s_synced; }

void ntp_get_local(struct tm *t)
{
    time_t now;
    time(&now);
    localtime_r(&now, t);
}
