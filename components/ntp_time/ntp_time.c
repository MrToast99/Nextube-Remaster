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
#include <math.h>

static const char *TAG = "ntp";
/* volatile: read by the display task (Core 1) while written by the NTP task
 * (Core 0).  Without volatile the compiler or CPU may serve a stale cached
 * value across cores; these flags are only ever set false→true so the only
 * risk is a delayed true, but volatile makes the intent explicit. */
static volatile bool s_synced         = false;
static volatile bool s_time_valid     = false;  /* true once RTC seed or NTP sync has given us a plausible time */
static volatile bool s_rtc_battery_ok = false;  /* true only if the RTC seed at boot was >= RTC_MIN_VALID_EPOCH.
                                                  * Never set true by NTP — reflects the battery/RTC state at
                                                  * power-on.  Exposed via ntp_rtc_battery_ok() for the web UI
                                                  * "dead battery" warning toast. */

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
static int64_t    s_last_ntp_us    = 0;     /* esp_timer_get_time() µs at last sync */

/* ── Oscillator drift comparison log ────────────────────────────────────────
 * Every genuine hourly re-sync logs a "DRIFT" line giving the measured drift of
 * both the ESP high-res timer (the crystal the system clock runs on) and the
 * PCF8563.  Purely diagnostic. */

/* ── Experimental between-sync disciplining (debug only) ─────────────────────
 * cfg->time_discipline_mode (set from the hidden debug panel):
 *   0 = off — reactive NTP only (default).
 *   1 = ESP — learn the crystal's drift rate and pre-compensate it every
 *             minute with small adjtime() nudges.
 *   2 = PCF — edge-synced read of the PCF8563 each minute, slew toward it.
 * NOTE: the DRIFT log measures the raw esp_timer, which mode 1 does not alter,
 * so its effect is not visible there; and it partially fights the NTP smoother.
 * Kept as an experimental knob, not a recommended default. */
#define DISCIPLINE_INTERVAL_S   60      /* ntp_task wakes every 60 s            */
#define DRIFT_EMA_ALPHA         0.30    /* smoothing for the learned drift rate */

static double s_esp_drift_ppm = 0.0;    /* learned ESP-clock rate, +ve = slow   */
static bool   s_drift_valid   = false;  /* true once >= 2 samples collected     */
static int    s_drift_samples = 0;

/* PCF-slave (mode 2) hourly summary accumulators — collapse the per-minute
 * lines into one summary at each sync.  The first tick after a sync is the
 * whole-second re-alignment (tracked separately so it doesn't skew the
 * steady-state min/avg). */
static int    s_pcf_n          = 0;     /* steady-state ticks this hour         */
static double s_pcf_sum_abs    = 0.0;   /* Σ|drift_ms| for the average          */
static double s_pcf_max_abs    = 0.0;   /* worst steady |drift_ms|              */
static double s_pcf_realign_ms = 0.0;   /* first post-sync re-align, ms         */
static bool   s_pcf_post_sync  = false; /* next tick is the post-sync re-align  */

static uint8_t discipline_mode(void)
{
    uint8_t m;
    config_lock();
    m = config_get()->time_discipline_mode;
    config_unlock();
    return m;
}

static void time_sync_cb(struct timeval *tv)
{
    /* SNTP_SYNC_MODE_IMMED: settimeofday() was already called before this
     * callback fires, so time(NULL) == tv->tv_sec here.                   */
    time_t  ntp_sec = (tv && tv->tv_sec > 0) ? tv->tv_sec : time(NULL);
    int64_t now_us  = esp_timer_get_time();

    if (!s_boot_synced) {
        /* ── Boot sync: leave the hard settimeofday() in place ────────────
         * The device may have been off for any length of time; jumping
         * straight to the correct time is always the right thing to do.   */
        ESP_LOGI(TAG, "NTP sync: boot — hard set to %lld", (long long)ntp_sec);
        s_boot_synced = true;

    } else {
        /* ── Post-boot callback: decide hard-set vs adjtime ───────────────
         * Reconstruct elapsed real-time from esp_timer — the systimer/HRT, the
         * same XTAL-locked source gettimeofday() uses.  NOT the FreeRTOS tick:
         * xTaskGetTickCount() loses counts during long interrupt-disabled
         * windows (SPI-DMA display pushes, LED RMT, flash writes), which
         * overstated elapsed time by ~280 ppm and made the smoothing inject a
         * ~900 ms/hr sawtooth.  adjtime() does not affect esp_timer.        */
        int64_t    elapsed_ms    = (now_us - s_last_ntp_us) / 1000LL;
        time_t     elapsed_s     = (time_t)(elapsed_ms / 1000LL);
        time_t     expected      = s_last_ntp_sec + elapsed_s;
        int64_t    offset_s      = (int64_t)ntp_sec - (int64_t)expected;
        int64_t    offset_ms     = offset_s * 1000LL + (tv ? (int64_t)(tv->tv_usec / 1000) : 0LL);
        int64_t    abs_offset    = offset_s >= 0 ? offset_s : -offset_s;

        ESP_LOGI(TAG, "NTP re-sync [diag]: raw ESP timer drifted %+lld ms vs NTP over %lld ms",
                 (long long)offset_ms, (long long)elapsed_ms);

        if (elapsed_s < NTP_BOOT_WINDOW_S) {
            /* Still in the boot window — additional pool-server responses
             * arrive within seconds of the first sync.  Treat them all as
             * hard sets: the RTC seed may have been inaccurate and we want
             * the clock locked to NTP as quickly as possible.             */
            ESP_LOGI(TAG, "NTP re-sync [applied]: hard-set to NTP (boot window)");

        } else if (discipline_mode() != 0) {
            /* A between-sync disciplining mode is active (ESP rate or PCF
             * slaving).  Leave the SNTP engine's hard-set in place and DO NOT
             * slew: the per-minute discipline_tick() holds the clock to its
             * reference, and the smoother's adjtime would otherwise yank it
             * ~offset ms off that reference once an hour. */
            ESP_LOGI(TAG, "NTP re-sync [applied]: hard-set to NTP; between-sync drift handled by "
                          "discipline mode (the raw-offset line above is diagnostic only)");

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
            ESP_LOGI(TAG, "NTP re-sync [applied]: adjtime %+lld ms (smoothing)",
                     (long long)offset_ms);
        } else {
            /* Large drift — leave the hard settimeofday() in place.       */
            ESP_LOGI(TAG, "NTP re-sync [applied]: hard-set %+lld ms (exceeds %d s smooth window)",
                     (long long)offset_ms, NTP_SMOOTH_MAX_S);
        }

        /* ── Drift comparison log (genuine hourly re-syncs only) ──
         * Skip boot-window duplicates.  1 ppm = 1 µs/s = 3.6 ms/hr. */
        if (elapsed_s >= NTP_BOOT_WINDOW_S && elapsed_ms > 0) {
            double esp_ppm = (double)offset_ms / (double)elapsed_ms * 1e6;

            /* PCF8563 drift: read it BEFORE the post-sync write (below)
             * overwrites it.  It was last set to NTP truth at the previous
             * sync, so (pcf − ntp) over this interval is its own drift.
             * 1 s read resolution → ~278 ppm granularity at a 1 h interval. */
            struct tm pcf_t;
            if (rtc_get_time(&pcf_t)) {
                double pcf_ppm = (double)(((long long)mktime(&pcf_t) - (long long)ntp_sec) * 1000LL)
                                 / (double)elapsed_ms * 1e6;
                ESP_LOGI(TAG, "DRIFT [diag, not applied]  raw ESP XTAL %+.1f ms/hr (%+.1f ppm)  |  "
                              "PCF8563 %+.1f ms/hr (%+.1f ppm, 1 s res)",
                         esp_ppm * 3.6, esp_ppm, pcf_ppm * 3.6, pcf_ppm);
            } else {
                ESP_LOGI(TAG, "DRIFT [diag, not applied]  raw ESP XTAL %+.1f ms/hr (%+.1f ppm)  |  "
                              "PCF8563 read failed",
                         esp_ppm * 3.6, esp_ppm);
            }

            /* Learn the ESP crystal drift rate (EMA) for mode-1 disciplining.
             * offset_ms is esp_timer-based (adjtime-immune), so it measures the
             * raw rate with no feedback loop — a plain EMA converges to it. */
            s_esp_drift_ppm = s_drift_valid
                ? (DRIFT_EMA_ALPHA * esp_ppm + (1.0 - DRIFT_EMA_ALPHA) * s_esp_drift_ppm)
                : esp_ppm;
            if (++s_drift_samples >= 2) s_drift_valid = true;

            /* PCF-slave (mode 2): one summary line per hour instead of ~60. */
            if (s_pcf_n > 0 || s_pcf_realign_ms != 0.0) {
                ESP_LOGI(TAG, "PCF slave [diag]: last hour — %d ticks, |drift| avg %.1f ms, "
                              "max %.0f ms; post-sync re-align %+.0f ms",
                         s_pcf_n, s_pcf_n ? s_pcf_sum_abs / s_pcf_n : 0.0,
                         s_pcf_max_abs, s_pcf_realign_ms);
            }
            s_pcf_n = 0; s_pcf_sum_abs = 0.0; s_pcf_max_abs = 0.0; s_pcf_realign_ms = 0.0;
        }
    }

    s_synced     = true;
    s_time_valid = true;

    /* Save reference point for the next re-sync's offset computation.     */
    s_last_ntp_sec   = ntp_sec;
    s_last_ntp_us = now_us;

    /* Write the NTP time back to the battery-backed RTC so it survives
     * power cuts and acts as a warm seed on the next boot.  Store local
     * time so mktime() can reconstruct time_t without extra UTC handling
     * (TZ is always applied before both writes and reads).
     *
     * The PCF8563 only stores whole seconds, so ROUND to the nearest second
     * (instead of truncating tv_sec): this centres the RTC's error at ±0.5 s
     * rather than 0..−1 s, halving the worst-case post-sync re-alignment when
     * PCF slaving is active. */
    time_t rtc_sec = ntp_sec + ((tv && tv->tv_usec >= 500000) ? 1 : 0);
    struct tm t;
    localtime_r(&rtc_sec, &t);
    if (rtc_set_time(&t)) {
        ESP_LOGI(TAG, "RTC updated: %04d-%02d-%02d %02d:%02d:%02d (local)",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        ESP_LOGW(TAG, "RTC write failed after NTP sync");
    }

    /* The RTC was just rewritten (whole-second), so the next PCF-slave tick is
     * the one-off re-alignment — flag it so it's tracked separately from the
     * steady-state per-minute drift in the hourly summary.
     * Only set when mode 2 is actually active; otherwise the flag would stay
     * true indefinitely (only the mode-2 tick handler clears it), and the
     * first tick after switching to mode 2 would misclassify steady-state
     * drift as a post-sync realignment. */
    if (discipline_mode() == 2) s_pcf_post_sync = true;
}

/* ── RTC validity floor ──────────────────────────────────────────────────
 * EPOCH_JAN1(y): Unix epoch for 01-Jan of year y, 00:00:00 UTC.
 *   days  = (y − 1970) × 365  +  leap-days-between-1970-and-y
 *   leaps = ⌊(y−1)/4⌋ − ⌊(y−1)/100⌋ + ⌊(y−1)/400⌋  (relative to 1969)
 * GCC/Clang evaluate __DATE__[N] as a compile-time constant, so the
 * entire expression folds to a single integer at compile time.
 *
 * Floor = Jan 1 of (build_year − 1).  A healthy device always holds a
 * recent NTP-written date; anything older means the coin cell died before
 * a fresh sync could be stored.  Using (build_year − 1) keeps the check
 * meaningful for the entire serviceable life of a firmware build without
 * requiring a hardcoded year constant that goes stale each release.       */
#define EPOCH_JAN1(y) \
    ((time_t)( \
        ((long long)((y) - 1970) * 365LL \
        + (long long)(((y)-1)/4   - ((y)-1)/100 + ((y)-1)/400 \
                    - (1969/4     -   1969/100   +   1969/400))) \
        * 86400LL))
#define BUILD_YEAR \
    ((__DATE__[7]-'0')*1000 + (__DATE__[8]-'0')*100 + \
     (__DATE__[9]-'0')*10   + (__DATE__[10]-'0'))
#define RTC_MIN_VALID_EPOCH  EPOCH_JAN1(BUILD_YEAR - 1)

/* ── Early RTC seed ───────────────────────────────────────────────────────
 * Called from app_main() BEFORE display_task_start() so the very first
 * render tick sees a correct wall-clock time instead of the epoch-0
 * stopwatch that appeared while power-cycling between firmware builds.
 *
 * Also applies the POSIX TZ string so localtime_r() is correct from the
 * moment the display task starts.  The NTP task re-applies TZ and retries
 * the RTC read if this call failed for any reason.                        */
void ntp_seed_rtc_early(void)
{
    char tz[64];
    config_lock();
    strncpy(tz, config_get()->timezone, sizeof(tz) - 1);
    tz[sizeof(tz) - 1] = '\0';
    config_unlock();
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set: %s", tz);

    struct tm rtc_t = {0};
    if (!rtc_get_time(&rtc_t)) {
        ESP_LOGW(TAG, "early RTC seed: read failed (VL flag or I²C error)");
        return;
    }
    time_t seed = mktime(&rtc_t);
    ESP_LOGI(TAG, "RTC read OK: %04d-%02d-%02d %02d:%02d:%02d (local) → epoch %lld",
             rtc_t.tm_year + 1900, rtc_t.tm_mon + 1, rtc_t.tm_mday,
             rtc_t.tm_hour, rtc_t.tm_min, rtc_t.tm_sec, (long long)seed);
    if (seed < RTC_MIN_VALID_EPOCH) {
        ESP_LOGW(TAG, "early RTC seed: year %d < floor %d — waiting for NTP",
                 rtc_t.tm_year + 1900, BUILD_YEAR - 1);
        return;
    }
    struct timeval tv = { .tv_sec = seed, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "early RTC seed: settimeofday failed");
        return;
    }
    s_time_valid     = true;
    s_rtc_battery_ok = true;
    ESP_LOGI(TAG, "system clock seeded before display start ✓");
}

/* Build a struct timeval from a signed microsecond count.
 * C99 truncates-toward-zero for integer division, so when us < 0 the
 * remainder is also ≤ 0 (e.g. -1500000 µs → tv_sec=-1, tv_usec=-500000).
 * POSIX adjtime() requires tv_usec in [0, 999999]; normalise before use. */
static void timeval_from_us(struct timeval *tv, long long us)
{
    tv->tv_sec  = (time_t)(us / 1000000LL);
    tv->tv_usec = (suseconds_t)(us % 1000000LL);
    if (tv->tv_usec < 0) { tv->tv_sec--; tv->tv_usec += 1000000; }
}

/* Apply one disciplining step.  Called once per minute from ntp_task.
 * Mode 0: nothing.  Mode 1: pre-compensate the learned ESP drift.
 * Mode 2: edge-synced read of the PCF8563 and slew the system clock to it. */
static void discipline_tick(void)
{
    if (!s_time_valid) return;
    uint8_t mode = discipline_mode();

    if (mode == 1) {
        /* ESP frequency disciplining.  +ve rate = clock slow → add time. */
        if (!s_drift_valid) return;
        double    us   = s_esp_drift_ppm * (double)DISCIPLINE_INTERVAL_S; /* ppm·s = µs */
        long long us_i = llround(us);
        if (us_i == 0) return;
        struct timeval d;
        timeval_from_us(&d, us_i);
        adjtime(&d, NULL);
        ESP_LOGD(TAG, "ESP discipline [applied]: %+lld ms/min (learned %+.1f ppm)",
                 (long long)(us_i / 1000), s_esp_drift_ppm);

    } else if (mode == 2) {
        /* PCF8563 slaving.  Edge-sync: poll the seconds register until it
         * ticks over, capturing the system time at that instant so the
         * PCF's whole-second value has a known (~0) sub-second phase. */
        /* Edge-sync: poll until the RTC seconds register ticks, then capture
         * gettimeofday() immediately — before any further I²C reads overwrite
         * the timestamp.  The previous do-while captured sysnow AFTER the
         * continue read, biasing every correction by one extra I²C round-trip
         * (~1-3 ms) that discipline could never self-cancel. */
        struct tm a, b;
        if (!rtc_get_time(&a)) return;
        struct timeval sysnow = {0};
        TickType_t t0 = xTaskGetTickCount();
        bool ticked = false;
        while ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(1100)) {
            if (!rtc_get_time(&b)) return;
            if (b.tm_sec != a.tm_sec) {
                gettimeofday(&sysnow, NULL);   /* capture at the tick edge, not after */
                ticked = true;
                break;
            }
        }
        if (!ticked) { ESP_LOGW(TAG, "PCF slave: no tick edge (skipped)"); return; }

        double    err  = ((double)sysnow.tv_sec + sysnow.tv_usec / 1e6)
                         - (double)mktime(&b);          /* +ve = ESP ahead of PCF */
        long long us_i = llround(-err * 1e6);            /* slew toward PCF        */
        if (us_i >  2000000) us_i =  2000000;            /* clamp ±2 s             */
        if (us_i < -2000000) us_i = -2000000;
        struct timeval d;
        timeval_from_us(&d, us_i);
        adjtime(&d, NULL);

        /* Fold into the hourly summary instead of logging every minute. */
        double err_ms = err * 1000.0;
        if (s_pcf_post_sync) {
            s_pcf_realign_ms = err_ms;   /* one-off whole-second re-alignment */
            s_pcf_post_sync  = false;
        } else {
            double a = err_ms < 0 ? -err_ms : err_ms;
            s_pcf_sum_abs += a;
            if (a > s_pcf_max_abs) s_pcf_max_abs = a;
            s_pcf_n++;
        }
        ESP_LOGD(TAG, "PCF slave [applied]: clock was %+0.0f ms vs RTC → corrected %+lld ms",
                 err_ms, (long long)(us_i / 1000));
    }
}

static void ntp_task(void *arg)
{
    /* ── Phase 1: apply TZ; fallback RTC seed if early seed was skipped ──
     * ntp_seed_rtc_early() (called from app_main before display_task_start)
     * normally handles both TZ and the RTC seed.  Re-apply TZ here
     * unconditionally (idempotent), and retry the RTC seed only when
     * s_time_valid is still false — e.g. if pcf8563_init hadn't completed
     * at the time of the early call.                                        */
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

    setenv("TZ", timezone, 1);
    tzset();

    if (!s_time_valid) {
        /* Early seed was skipped or failed — attempt RTC read now as fallback. */
        struct tm rtc_t = {0};
        if (rtc_get_time(&rtc_t)) {
            time_t seed = mktime(&rtc_t);
            ESP_LOGI(TAG, "RTC read OK: %04d-%02d-%02d %02d:%02d:%02d (local) → epoch %lld",
                     rtc_t.tm_year + 1900, rtc_t.tm_mon + 1, rtc_t.tm_mday,
                     rtc_t.tm_hour, rtc_t.tm_min, rtc_t.tm_sec, (long long)seed);
            if (seed >= RTC_MIN_VALID_EPOCH) {
                struct timeval tv_seed = { .tv_sec = seed, .tv_usec = 0 };
                if (settimeofday(&tv_seed, NULL) == 0) {
                    s_time_valid     = true;
                    s_rtc_battery_ok = true;
                    ESP_LOGI(TAG, "System clock seeded from RTC ✓ (fallback)");
                } else {
                    ESP_LOGW(TAG, "settimeofday failed — clock not seeded from RTC");
                }
            } else {
                ESP_LOGW(TAG, "RTC time too old (year %d < floor %d) — waiting for NTP",
                         rtc_t.tm_year + 1900, BUILD_YEAR - 1);
            }
        } else {
            ESP_LOGW(TAG, "RTC read failed (VL flag set or I²C error) — waiting for NTP");
        }
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
        vTaskDelay(pdMS_TO_TICKS(DISCIPLINE_INTERVAL_S * 1000));   /* wake every minute */
        discipline_tick();   /* mode 0 = no-op; 1 = ESP rate; 2 = PCF slave */
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
    /* MUST be static — esp_sntp_setservername() (lwIP sntp_setservername) stores
     * the POINTER to each server-name string; it does NOT copy the string.  A
     * stack-local buffer here would be reclaimed the instant this function
     * returns, leaving lwIP's SNTP server table pointing at freed stack memory.
     * On the next poll the SNTP engine then resolves whatever garbage now
     * occupies that stack — firing a burst of malformed DNS queries on every
     * config save (visible as junk "<binary>.localdomain" lookups in the DNS
     * server's log, sourced from the device itself).  A static buffer keeps the
     * strings, and therefore lwIP's stored pointers, valid for the program's
     * lifetime.  (The boot path in ntp_task is safe for a different reason: its
     * stack-local copy lives as long as ntp_task, which never returns.) */
    static char servers[4][64];
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
bool ntp_rtc_battery_ok(void) { return s_rtc_battery_ok; }

void ntp_get_local(struct tm *t)
{
    time_t now;
    time(&now);
    localtime_r(&now, t);
}
