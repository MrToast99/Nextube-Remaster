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
static bool s_synced     = false;
static bool s_time_valid = false;  /* true once RTC seed or NTP sync has given us a plausible time */

/* ── Smooth-sync state ──────────────────────────────────────────────────────
 * Boot window  (any NTP packet within NTP_BOOT_WINDOW_S of the previous one):
 *   Always a hard settimeofday().  SNTP queries all configured pool servers
 *   simultaneously on startup; responses from servers 2–4 arrive within
 *   seconds of the first and must not trigger adjtime — the RTC may have
 *   drifted by any amount and a hard jump is always correct at boot.
 *
 * Periodic re-sync (every ~1 hour, well outside the boot window): if the
 *   drift is within NTP_SMOOTH_MAX_S seconds, undo the SNTP engine's
 *   settimeofday() and replace it with adjtime() so the clock slews to the
 *   correct time without ever jumping backwards.  Beyond this window the
 *   hard jump stays (e.g. after an extended power cut).
 *
 * The offset is reconstructed using the FreeRTOS tick counter (monotonic,
 * unaffected by settimeofday() or adjtime()) relative to the previous sync. */
#define NTP_SMOOTH_MAX_S     60   /* seconds: adjtime window for periodic re-syncs  */
#define NTP_BOOT_WINDOW_S   300   /* 5 min: syncs closer together than this are hard */

static bool       s_boot_synced    = false; /* set after first post-boot NTP sync  */
static time_t     s_last_ntp_sec   = 0;     /* NTP epoch at last successful sync    */
static TickType_t s_last_ntp_ticks = 0;     /* xTaskGetTickCount() at last sync     */

static void time_sync_cb(struct timeval *tv)
{
    /* SNTP_SYNC_MODE_IMMED: settimeofday() was already called before this
     * callback fires, so time(NULL) == tv->tv_sec here.                   */
    time_t     ntp_sec   = (tv && tv->tv_sec > 0) ? tv->tv_sec : time(NULL);
    TickType_t now_ticks = xTaskGetTickCount();

    if (!s_boot_synced) {
        /* ── Boot sync: leave the hard settimeofday() in place ────────────
         * The device may have been off for any length of time; jumping
         * straight to the correct time is always the right thing to do.   */
        ESP_LOGI(TAG, "NTP sync: boot — hard set to %lld", (long long)ntp_sec);
        s_boot_synced = true;

    } else {
        /* ── Post-boot callback: decide hard-set vs adjtime ───────────────
         * Reconstruct what the system clock read just before SNTP fired.
         * FreeRTOS tick counter is monotonic and unaffected by any time
         * adjustment, so elapsed real-time is accurate.                   */
        TickType_t elapsed_ticks = now_ticks - s_last_ntp_ticks;  /* wraps safely */
        time_t     elapsed_s     = (time_t)(pdTICKS_TO_MS(elapsed_ticks) / 1000UL);
        time_t     expected      = s_last_ntp_sec + elapsed_s;
        int64_t    offset_s      = (int64_t)ntp_sec - (int64_t)expected;
        int64_t    abs_offset    = offset_s >= 0 ? offset_s : -offset_s;

        ESP_LOGI(TAG, "NTP re-sync: offset %+lld s  elapsed %lld s",
                 (long long)offset_s, (long long)elapsed_s);

        if (elapsed_s < NTP_BOOT_WINDOW_S) {
            /* Still in the boot window — additional pool-server responses
             * arrive within seconds of the first sync.  Treat them all as
             * hard sets: the RTC seed may have been inaccurate and we want
             * the clock locked to NTP as quickly as possible.             */
            ESP_LOGI(TAG, "NTP re-sync: hard set (boot window, %lld s elapsed)",
                     (long long)elapsed_s);

        } else if (abs_offset <= NTP_SMOOTH_MAX_S) {
            /* Genuine hourly re-sync with small drift — slew with adjtime().
             * adjtime() at ~500 µs/s never jumps the clock backwards.
             * Indicative slew times:  1 s → ~33 min,  60 s → ~33 h.
             * The display tick clamp (CLOCK_MAX_STEP_S = 1 in display.c)
             * independently keeps the visual output smooth.               */
            struct timeval tv_old  = { .tv_sec  = expected, .tv_usec = 0 };
            struct timeval tv_corr = { .tv_sec  = (time_t)offset_s,
                                       .tv_usec = tv ? tv->tv_usec : 0 };
            settimeofday(&tv_old, NULL);   /* restore pre-correction position */
            adjtime(&tv_corr, NULL);       /* slew to NTP target gradually    */
            ESP_LOGI(TAG, "NTP re-sync: adjtime %+lld s (within %d s window)",
                     (long long)offset_s, NTP_SMOOTH_MAX_S);
        } else {
            /* Large drift — leave the hard settimeofday() in place.       */
            ESP_LOGI(TAG, "NTP re-sync: hard set (%+lld s exceeds %d s window)",
                     (long long)offset_s, NTP_SMOOTH_MAX_S);
        }
    }

    s_synced     = true;
    s_time_valid = true;

    /* Save reference point for the next re-sync's offset computation.     */
    s_last_ntp_sec   = ntp_sec;
    s_last_ntp_ticks = now_ticks;

    /* Write the NTP time back to the battery-backed RTC so it survives
     * power cuts and acts as a warm seed on the next boot.  Store local
     * time so mktime() can reconstruct time_t without extra UTC handling
     * (TZ is always applied before both writes and reads).                */
    struct tm t;
    localtime_r(&ntp_sec, &t);
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
#define RTC_MIN_VALID_EPOCH  1704067200LL  /* 2024-01-01 UTC — arbitrary but reasonable
                                             * cutoff: device was manufactured no earlier
                                             * than 2024; anything before this means the
                                             * RTC battery is dead or was never set. */

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
            s_time_valid = true;   /* RTC gave us a plausible wall-clock time */
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

    /* Use IMMED mode so the SNTP engine always calls settimeofday() before
     * our callback fires.  The callback then decides — based on the true
     * offset derived from FreeRTOS ticks — whether to leave the hard set
     * in place (large drift > NTP_SMOOTH_MAX_S) or undo it and use
     * adjtime() instead (small drift ≤ NTP_SMOOTH_MAX_S).               */
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);

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

bool ntp_time_synced(void)    { return s_synced; }
bool ntp_has_valid_time(void) { return s_time_valid; }

void ntp_get_local(struct tm *t)
{
    time_t now;
    time(&now);
    localtime_r(&now, t);
}
