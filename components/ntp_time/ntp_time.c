#include "ntp_time.h"
#include "config_mgr.h"
#include "rtc_pcf8563.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "ntp";
static bool s_synced = false;

static void time_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "NTP time synchronised");
    s_synced = true;

    /* Write the freshly-synchronised time back to the battery-backed RTC so
     * it survives power cuts and acts as a warm seed on the next boot.
     * Store local time so mktime() can reconstruct time_t on boot without
     * needing extra UTC handling (TZ is always applied before both writes
     * and reads). */
    struct tm t;
    time_t now = time(NULL);
    localtime_r(&now, &t);
    if (rtc_set_time(&t)) {
        ESP_LOGI(TAG, "RTC updated: %04d-%02d-%02d %02d:%02d:%02d (local)",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        ESP_LOGW(TAG, "RTC write failed after NTP sync");
    }
}

static void ntp_task(void *arg)
{
    const nextube_config_t *cfg = config_get();

    /* Wait for WiFi */
    vTaskDelay(pdMS_TO_TICKS(5000));

    /* Apply POSIX TZ string — newlib handles DST transition rules natively.
     * The string is stored verbatim in cfg->timezone, e.g. "EST5EDT,M3.2.0,M11.1.0". */
    setenv("TZ", cfg->timezone, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set: %s", cfg->timezone);

    /* Seed the system clock from the battery-backed RTC so the display shows
     * a reasonable time immediately, before the first NTP sync completes.
     * rtc_get_time() returns local time; mktime() interprets it as local
     * (TZ is already set above), producing a correct time_t. */
    /* Minimum plausible time_t: 2024-01-01 00:00:00 UTC.
     * Anything earlier means the RTC was never set or has lost power. */
#define RTC_MIN_VALID_EPOCH  1704067200LL  /* 2024-01-01 */

    struct tm rtc_t = {0};
    if (rtc_get_time(&rtc_t)) {
        time_t seed = mktime(&rtc_t);
        if (seed >= RTC_MIN_VALID_EPOCH) {
            struct timeval tv_seed = { .tv_sec = seed, .tv_usec = 0 };
            settimeofday(&tv_seed, NULL);
            ESP_LOGI(TAG, "System clock seeded from RTC: %04d-%02d-%02d %02d:%02d:%02d (local)",
                     rtc_t.tm_year + 1900, rtc_t.tm_mon + 1, rtc_t.tm_mday,
                     rtc_t.tm_hour, rtc_t.tm_min, rtc_t.tm_sec);
        } else {
            ESP_LOGW(TAG, "RTC time too old (seed=%lld, min=%lld) — ignoring, waiting for NTP",
                     (long long)seed, (long long)RTC_MIN_VALID_EPOCH);
        }
    } else {
        ESP_LOGW(TAG, "RTC read failed — clock starts at epoch until NTP sync");
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    for (int i = 0; i < 4; i++) {
        if (cfg->ntp_servers[i][0] != '\0')
            esp_sntp_setservername(i, cfg->ntp_servers[i]);
    }
    sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

void ntp_apply_timezone(void)
{
    const nextube_config_t *cfg = config_get();
    setenv("TZ", cfg->timezone, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone updated: %s", cfg->timezone);
}

void ntp_apply_servers(void)
{
    const nextube_config_t *cfg = config_get();
    /* Stop SNTP before changing servers — lwIP setservername is not
     * thread-safe while the SNTP polling timer is live. */
    esp_sntp_stop();
    for (int i = 0; i < 4; i++) {
        esp_sntp_setservername(i,
            cfg->ntp_servers[i][0] ? cfg->ntp_servers[i] : NULL);
    }
    esp_sntp_init();
    ESP_LOGI(TAG, "NTP servers updated");
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
