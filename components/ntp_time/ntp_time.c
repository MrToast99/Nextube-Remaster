#include "ntp_time.h"
#include "config_mgr.h"
#include "rtc_pcf8563.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
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
    /* ── Phase 1: apply TZ + seed from RTC immediately ───────────────────
     * This happens before the WiFi-wait delay so the display shows the
     * correct local time from the very first render tick after boot.
     * Reading TZ from config and seeding from RTC require neither WiFi
     * nor SNTP — they are purely local operations and are always safe to
     * run at task start.
     *
     * Minimum plausible time_t: 2024-01-01 00:00:00 UTC.
     * Anything earlier means the RTC was never set or has lost power. */
#define RTC_MIN_VALID_EPOCH  1704067200LL  /* 2024-01-01 */

    char timezone[64];
    char ntp_servers[4][64];
    config_lock();
    const nextube_config_t *cfg_boot = config_get();
    strncpy(timezone, cfg_boot->timezone, sizeof(timezone) - 1);
    timezone[sizeof(timezone) - 1] = '\0';
    for (int i = 0; i < 4; i++) {
        strncpy(ntp_servers[i], cfg_boot->ntp_servers[i], sizeof(ntp_servers[i]) - 1);
        ntp_servers[i][sizeof(ntp_servers[i]) - 1] = '\0';
    }
    config_unlock();

    /* Apply POSIX TZ string — newlib handles DST transition rules natively. */
    setenv("TZ", timezone, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set: %s", timezone);

    /* Seed the system clock from the battery-backed RTC so the display shows
     * a reasonable time immediately, before the first NTP sync completes.
     * rtc_get_time() returns local time; mktime() interprets it as local
     * (TZ is already set above), producing a correct time_t. */
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

    /* ── Phase 2: wait for WiFi, then start SNTP polling ────────────────
     * The delay gives WiFi time to connect so SNTP queries go out on the
     * first poll rather than being silently dropped.  NTP servers were
     * already read from config above (before the lock was released) so
     * there is no need to re-acquire the lock here. */
    vTaskDelay(pdMS_TO_TICKS(5000));

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    for (int i = 0; i < 4; i++) {
        if (ntp_servers[i][0] != '\0')
            esp_sntp_setservername(i, ntp_servers[i]);
    }
    sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();

/* Re-resolve pool.ntp.org DNS once per day so the cached IP stays fresh
 * as the NTP pool rotates members.  ntp_apply_servers() does the full
 * stop → setservername → init cycle which flushes lwIP's address cache. */
#define NTP_DNS_REFRESH_US  (24LL * 3600LL * 1000000LL)

    int64_t last_dns_refresh = esp_timer_get_time();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));   /* wake every minute */
        if (esp_timer_get_time() - last_dns_refresh >= NTP_DNS_REFRESH_US) {
            last_dns_refresh = esp_timer_get_time();
            ESP_LOGI(TAG, "Daily NTP pool re-resolution");
            ntp_apply_servers();
        }
    }
}

void ntp_apply_timezone(void)
{
    char tz[64];
    config_lock();
    strncpy(tz, config_get()->timezone, sizeof(tz) - 1);
    tz[sizeof(tz) - 1] = '\0';
    config_unlock();
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone updated: %s", tz);
}

void ntp_apply_servers(void)
{
    char servers[4][64];
    config_lock();
    const nextube_config_t *cfg = config_get();
    for (int i = 0; i < 4; i++) {
        strncpy(servers[i], cfg->ntp_servers[i], sizeof(servers[i]) - 1);
        servers[i][sizeof(servers[i]) - 1] = '\0';
    }
    config_unlock();
    /* Stop SNTP before changing servers — lwIP setservername is not
     * thread-safe while the SNTP polling timer is live. */
    esp_sntp_stop();
    for (int i = 0; i < 4; i++) {
        esp_sntp_setservername(i, servers[i][0] ? servers[i] : NULL);
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
