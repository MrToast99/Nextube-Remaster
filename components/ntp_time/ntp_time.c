#include "ntp_time.h"
#include "config_mgr.h"
#include "rtc_pcf8563.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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

/* External sync-stats listener (see ntp_register_sync_listener).  Called from
 * the SNTP callback context after each steady-state sync. */
static ntp_sync_listener_t s_sync_listener = NULL;

void ntp_register_sync_listener(ntp_sync_listener_t cb)
{
    s_sync_listener = cb;
}

/* ── SNTP reconfigure serialization ──────────────────────────────────────
 * ntp_task's boot sequence (stop -> setoperatingmode -> setservername ->
 * init) and ntp_apply_servers()'s sequence (stop -> setservername -> init)
 * can each be triggered from a different task — the latter from a settings
 * save on the httpd task, or from ntp_task's own daily DNS-refresh call.
 * The 2026-08 fix that added esp_sntp_stop() before ntp_task's
 * setoperatingmode() closed the ONE interleaving that had actually been
 * observed, but nothing stopped a settings save's own esp_sntp_init() from
 * landing between ntp_task's stop() and its later calls — the same
 * "Operating mode must not be set while SNTP client is running" assert
 * class, just via a different interleaving. This mutex makes each
 * function's whole reconfigure sequence atomic relative to the other. */
static SemaphoreHandle_t s_sntp_cfg_mutex = NULL;

static SemaphoreHandle_t sntp_cfg_mutex(void)
{
    if (!s_sntp_cfg_mutex) s_sntp_cfg_mutex = xSemaphoreCreateMutex();
    return s_sntp_cfg_mutex;
}

/* ntp_task's own handle, so time_sync_cb() (SNTP/network-task context) can
 * wake it promptly for deferred work instead of doing that work itself —
 * see time_sync_cb()'s comment. */
static TaskHandle_t s_ntp_task_handle = NULL;

/* Cached copy of cfg->time_discipline_mode, refreshed once a minute by
 * discipline_tick() (ntp_task's own context — config_lock() is safe there).
 * time_sync_cb() reads this instead of calling discipline_mode() itself —
 * see time_sync_cb()'s comment for why. Defaults to 2 (PCF), matching
 * config_mgr's own default, so a boot-time sync before the first
 * discipline_tick() behaves correctly for users who never changed it; the
 * discipline_mode() checks in time_sync_cb() only matter from the SECOND
 * sync onward (the boot sync takes the !s_boot_synced branch instead), and
 * ntp_task's loop always runs at least one discipline_tick() before then. */
static volatile uint8_t s_discipline_mode_cached = 2;

/* RTC write requested by time_sync_cb() but performed by ntp_task's own
 * loop — rtc_set_time() is a blocking I2C write, which the SNTP callback
 * context must not do (see time_sync_cb()'s comment). Single producer
 * (time_sync_cb), single consumer (ntp_task's loop), so the flag-guards-data
 * ordering below is safe without an explicit lock. */
static volatile bool s_rtc_write_pending = false;
static struct tm     s_rtc_write_tm;

/* ── Between-sync time disciplining ─────────────────────────────────────────
 * cfg->time_discipline_mode:
 *   0 = off — reactive NTP only; XTAL drift uncorrected between syncs.
 *   1 = ESP — learn the crystal's drift rate and pre-compensate each minute
 *             with small adjtime() nudges.
 *   2 = PCF — edge-sync to the PCF8563 each minute; slew toward it.
 *             Best between-sync accuracy (~1 ms). Default.              */
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

/* Runs in the SNTP/network-task context, NOT ntp_task — must not block or
 * take any mutex a network-using task can hold. Two things this used to do
 * violated that: calling discipline_mode() (config_lock()) several times,
 * and a blocking I2C RTC write. Both are now deferred — the discipline mode
 * comes from a cache ntp_task's own loop refreshes (see
 * s_discipline_mode_cached's comment), and the RTC write is handed off to
 * ntp_task via s_rtc_write_pending + a task notification so it happens
 * promptly but on ntp_task's own, safe-to-block context. */
static void time_sync_cb(struct timeval *tv)
{
    /* SNTP_SYNC_MODE_IMMED: settimeofday() was already called before this
     * callback fires, so time(NULL) == tv->tv_sec here.                   */
    time_t  ntp_sec = (tv && tv->tv_sec > 0) ? tv->tv_sec : time(NULL);
    int64_t now_us  = esp_timer_get_time();
    uint8_t mode    = s_discipline_mode_cached;

    if (!s_boot_synced) {
        ESP_LOGI(TAG, "NTP sync: boot — clock set");
        s_boot_synced = true;

    } else {
        /* Reconstruct elapsed real-time from esp_timer — adjtime-immune and
         * unaffected by long interrupt-disabled windows (DMA, flash writes)
         * that cause xTaskGetTickCount() to undercount.                    */
        int64_t elapsed_ms = (now_us - s_last_ntp_us) / 1000LL;
        time_t  elapsed_s  = (time_t)(elapsed_ms / 1000LL);
        time_t  expected   = s_last_ntp_sec + elapsed_s;
        int64_t offset_s   = (int64_t)ntp_sec - (int64_t)expected;
        int64_t offset_ms  = offset_s * 1000LL + (tv ? (int64_t)(tv->tv_usec / 1000) : 0LL);
        int64_t abs_offset = offset_s >= 0 ? offset_s : -offset_s;

        if (elapsed_s < NTP_BOOT_WINDOW_S) {
            /* Boot-window duplicate: extra pool responses arrive seconds
             * after the first sync.  Hard-set already applied by SNTP.   */
            ESP_LOGI(TAG, "NTP sync: %+lld ms (boot window)", (long long)offset_ms);

        } else if (mode == 2) {
            /* PCF slave active: hard-set stays; PCF edge-sync holds the
             * clock each minute.  Report worst-case error seen this hour.
             * offset_ms is reconstructed from the free-running esp_timer
             * (XTAL) timebase, so it is what the error WOULD have been
             * without discipline — the actual clock was held by the PCF. */
            if (s_pcf_n > 0) {
                ESP_LOGI(TAG, "NTP sync: XTAL (ESP) would have been %+lld ms — PCF (RTC) kept <=%.0f ms between syncs",
                         (long long)offset_ms, s_pcf_max_abs);
            } else {
                ESP_LOGI(TAG, "NTP sync: XTAL (ESP) would have been %+lld ms — PCF (RTC) slave active, no corrections recorded",
                         (long long)offset_ms);
            }

        } else if (mode == 1) {
            /* ESP rate discipline active: hard-set stays; rate nudge each
             * minute.  Show raw XTAL drift for reference (same counterfactual
             * as mode 2: the disciplined clock drifted less than this).    */
            ESP_LOGI(TAG, "NTP sync: XTAL (ESP) would have been %+lld ms — rate discipline active",
                     (long long)offset_ms);

        } else if (abs_offset <= NTP_SMOOTH_MAX_S) {
            /* Mode 0, small drift: undo hard-set and slew smoothly so the
             * clock never jumps backwards.  adjtime() at ~500 µs/s takes
             * roughly 33 min per second of offset.                        */
            struct timeval tv_old  = { .tv_sec  = expected, .tv_usec = 0 };
            struct timeval tv_corr = { .tv_sec  = (time_t)offset_s,
                                       .tv_usec = tv ? tv->tv_usec : 0 };
            settimeofday(&tv_old, NULL);
            adjtime(&tv_corr, NULL);
            long long slew_min = (long long)abs_offset * 2000LL / 60LL;
            ESP_LOGI(TAG, "NTP sync: slewing %+lld ms (~%lld min)",
                     (long long)offset_ms, slew_min);

        } else {
            /* Mode 0, large drift: hard-set stays.                        */
            ESP_LOGI(TAG, "NTP sync: %+lld ms corrected", (long long)offset_ms);
        }

        /* Notify the external sync-stats listener (ha_mqtt publishes these
         * to Home Assistant as sensors) BEFORE the accumulators reset below.
         * Boot-window duplicates are excluded — their short elapsed time
         * makes the offset meaningless as a drift measure. */
        if (elapsed_s >= NTP_BOOT_WINDOW_S && s_sync_listener) {
            float pcf_max = (mode == 2 && s_pcf_n > 0)
                          ? (float)s_pcf_max_abs : -1.0f;
            s_sync_listener((int32_t)offset_ms, pcf_max, mode);
        }

        /* Update XTAL drift EMA (silent — used by mode-1 discipline) and
         * reset PCF-slave hourly accumulators.  Skip boot-window callbacks
         * whose short elapsed time would corrupt the learned rate.         */
        if (elapsed_s >= NTP_BOOT_WINDOW_S && elapsed_ms > 0) {
            double esp_ppm = (double)offset_ms / (double)elapsed_ms * 1e6;
            s_esp_drift_ppm = s_drift_valid
                ? (DRIFT_EMA_ALPHA * esp_ppm + (1.0 - DRIFT_EMA_ALPHA) * s_esp_drift_ppm)
                : esp_ppm;
            if (++s_drift_samples >= 2) s_drift_valid = true;

            s_pcf_n = 0; s_pcf_sum_abs = 0.0; s_pcf_max_abs = 0.0; s_pcf_realign_ms = 0.0;
        }
    }

    s_synced       = true;
    s_time_valid   = true;
    s_last_ntp_sec = ntp_sec;
    s_last_ntp_us  = now_us;

    /* Write NTP time back to the battery-backed RTC so it survives power
     * cuts and provides a warm seed on the next boot.  Rounded to the
     * nearest second to centre the PCF-slave post-sync re-alignment at
     * ±0.5 s rather than 0..−1 s.  The actual I2C write happens on
     * ntp_task's own context — see s_rtc_write_pending's comment — so just
     * stage the data and wake it here. */
    time_t rtc_sec = ntp_sec + ((tv && tv->tv_usec >= 500000) ? 1 : 0);
    localtime_r(&rtc_sec, &s_rtc_write_tm);
    s_rtc_write_pending = true;
    if (s_ntp_task_handle) xTaskNotifyGive(s_ntp_task_handle);
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
    s_discipline_mode_cached = mode;   /* see s_discipline_mode_cached's comment */

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
            /* Short yield between polls: this was a zero-delay busy-spin that
             * burned CPU and saturated the shared I2C0 bus (also used by
             * SHT30) for up to 1.1 s once a minute with no correctness
             * benefit. 1-2 ms is small enough not to meaningfully affect the
             * edge-sync timing precision below — the loop just checks less
             * often — while no longer being a tight spin. */
            vTaskDelay(pdMS_TO_TICKS(2));
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

    /* Stop first — safe/idempotent even if SNTP was never started. Without
     * this, a settings save that races ahead of this fixed 5 s delay (it
     * calls ntp_apply_servers(), which already starts SNTP via its own
     * stop -> setservername -> init sequence below) leaves SNTP already
     * running by the time this line runs, and esp_sntp_setoperatingmode()
     * on an already-running client hits
     *   "assert failed: sntp_setoperatingmode ... Operating mode must not
     *    be set while SNTP client is running" — an observed boot crash.
     * The whole sequence is also serialized against ntp_apply_servers()
     * itself via sntp_cfg_mutex() — stopping first isn't enough on its own
     * if a concurrent call's esp_sntp_init() can still land in the middle
     * of this sequence; see the mutex's comment. */
    xSemaphoreTake(sntp_cfg_mutex(), portMAX_DELAY);
    esp_sntp_stop();
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
    xSemaphoreGive(s_sntp_cfg_mutex);

/* Re-resolve pool.ntp.org DNS once per day so the cached IP stays fresh
 * as the NTP pool rotates members.  ntp_apply_servers() does the full
 * stop → setservername → init cycle which flushes lwIP's address cache. */
#define NTP_DNS_REFRESH_US  (24LL * 3600LL * 1000000LL)

    int64_t last_dns_refresh = esp_timer_get_time();
    while (1) {
        /* Times out after DISCIPLINE_INTERVAL_S like the old vTaskDelay, but
         * time_sync_cb() can also wake this early via xTaskNotifyGive() to
         * get a pending RTC write serviced promptly instead of waiting up
         * to a minute — see s_rtc_write_pending's comment. */
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DISCIPLINE_INTERVAL_S * 1000));

        if (s_rtc_write_pending) {
            s_rtc_write_pending = false;
            if (!rtc_set_time(&s_rtc_write_tm)) {
                ESP_LOGW(TAG, "RTC write failed after NTP sync");
            }
            /* The RTC was just rewritten, so the next PCF-slave tick is a
             * one-off whole-second re-alignment — flag it so
             * discipline_tick() tracks it separately from the steady-state
             * per-minute accumulator. Guard: only mode 2 ever clears this
             * flag; setting it in other modes would leave it true
             * indefinitely. */
            if (s_discipline_mode_cached == 2) s_pcf_post_sync = true;
        }

        /* Woken early only for the RTC write above — don't run
         * discipline_tick()/DNS-refresh on a partial interval; both assume
         * a full DISCIPLINE_INTERVAL_S has actually elapsed. */
        if (notified) continue;

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
     * thread-safe while the SNTP polling timer is live. Serialized against
     * ntp_task's own boot-time reconfigure sequence — see sntp_cfg_mutex's
     * comment. */
    xSemaphoreTake(sntp_cfg_mutex(), portMAX_DELAY);
    esp_sntp_stop();
    for (int i = 0; i < 4; i++) {
        esp_sntp_setservername(i, servers[i][0] ? servers[i] : NULL);
    }
    esp_sntp_init();
    xSemaphoreGive(s_sntp_cfg_mutex);
    ESP_LOGI(TAG, "NTP servers updated");
}

void ntp_time_start(void)
{
    (void)sntp_cfg_mutex();   /* create before ntp_task can possibly need it */
    if (xTaskCreate(ntp_task, "ntp", 4096, NULL, 5, &s_ntp_task_handle) != pdPASS)
        ESP_LOGE(TAG, "ntp_task creation failed");
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

bool ntp_is_night_window(uint8_t start_hour, uint8_t end_hour)
{
    if (!ntp_has_valid_time()) return false;
    struct tm now_tm;
    ntp_get_local(&now_tm);
    int hr = now_tm.tm_hour;
    if (start_hour < end_hour)
        return hr >= start_hour && hr < end_hour;
    /* Wraps around midnight (e.g. 22:00 to 07:00) */
    return hr >= start_hour || hr < end_hour;
}
