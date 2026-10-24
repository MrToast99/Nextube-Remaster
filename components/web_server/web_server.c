#include "web_server.h"
#include <math.h>
#include "microphone.h"
#include "config_mgr.h"
#include "stock_files.h"   /* generated: STOCK_FILES[], STOCK_FILES_COUNT (see CMakeLists.txt) */
#include "wifi_manager.h"
#include "ntp_time.h"
#include "weather.h"
#include "periodic_net_poll.h"   /* the Theme Manager runs as a job on the shared network task */
#include "subscribers.h"
#include "ha_mqtt.h"
#include "display.h"
#include "leds.h"
#include "audio.h"
#include "sht30.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_littlefs.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "lwip/ip_addr.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"
#include "mbedtls/constant_time.h"

#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "auth.h"
#include "nvs.h"

/* Sensor and weather readings are floats: written as raw doubles they come out as 22.538108825683594. One decimal is all they carry. */
static double json_round1(float v) { return (double)lroundf(v * 10.0f) / 10.0; }

static const char *TAG = "web_srv";
/* Shared with the max_open_sockets config below AND web_server_socket_count()'s
 * httpd_get_client_list() buffer size -- httpd_get_client_list() rejects the
 * call outright if the buffer is smaller than the server's actual configured
 * socket cap, so these two uses can't be allowed to drift apart. */
#define WEB_SERVER_MAX_SOCKETS 12
static httpd_handle_t s_server = NULL;
static bool s_server_restart_pending = false;   /* set when a WiFi reconnect stops the server */

/* LittleFS usage stats (fs_total/fs_used in /api/status) — cached until
 * explicitly invalidated. esp_littlefs_info() walks every block to compute
 * used space (no cheap counter API exists), ~2.7s on this device's file
 * count — too slow to recompute on every 5s dashboard poll, or on every
 * file of a folder upload (see api_file_upload()'s space check).
 *
 * Computed lazily on first use and recomputed when fs_usage_invalidate()
 * is called after delete/hotpatch. A full LittleFS OTA always ends in
 * esp_restart(), resetting this cache for free. mkdir/rename don't
 * meaningfully change used bytes, so they're not invalidation points.
 * api_file_upload() deliberately does NOT invalidate on a successful
 * upload — it nudges s_fs_used_cached up by the bytes written instead, so
 * a many-file folder upload doesn't pay this walk once per file; see that
 * function's own comment for the resulting drift and how it's bounded.
 * Only touched from the httpd task, so no lock needed. */
static bool    s_fs_cache_valid   = false;
static size_t  s_fs_total_cached  = 0;
static size_t  s_fs_used_cached   = 0;
/* /spiffs/web/version.txt content, cached alongside the usage figures — it
 * only changes on a hotpatch / webui-pull / FS-OTA, all of which call
 * fs_usage_invalidate(). Avoids a blocking fopen() on every 5s /api/status
 * poll, which during a hotpatch's ~minute-long LittleFS write window would
 * stall behind the extraction long enough for the browser's poll to abort. */
static char    s_fs_ver_cached[32] = "unknown";
static void fs_usage_invalidate(void) { s_fs_cache_valid = false; }
static void lcd_preview_revert(void);        /* LCD tuning preview (A/B compare), defined further down */
static bool s_lcd_preview_pending = false;   /* a preview is on the tubes */
static void lcd_applied_invalidate(void);
static const char *webui_state_name(void);   /* "idle" / "running" / "done" / "error"; defined with s_webui */
static const char *webui_error_text(void);   /* why the last Web UI pull failed, "" if it did not */

/* True while a firmware/filesystem OTA or Web UI hotpatch is running. Declared
 * here (not with its doc comment further down) so api_status() can check it:
 * while an update holds the display parked via display_show_wait(),
 * api_status() must NOT run its own display_pause_for_spi()/display_unpause()
 * FS-usage walk — display_unpause() would drop the refcount to zero and
 * resume the display task mid-flash, the exact race display_show_wait()
 * exists to prevent. */
static volatile bool s_ota_active = false;

/* ── Scope guards ─────────────────────────────────────────────────────────────
 * Paired acquire / release calls (the TLS lock, the display pause) have leaked on an early return more than once. These release at
 * the end of their scope on every way out, with GCC's cleanup attribute. TLS_LOCK(g) takes the lock. DISPLAY_PAUSE(g, ms) tries to
 * pause the display and records whether it did: check g.paused, the pause can time out. Keep the guard in a block that ends where
 * the lock or pause should end. */
typedef struct { bool held; } tls_guard_t;
static inline void tls_guard_release(tls_guard_t *g) { if (g->held) tls_sem_give(); }
#define TLS_LOCK(g) tls_guard_t g __attribute__((cleanup(tls_guard_release))) = { false }; tls_sem_take(); g.held = true

typedef struct { bool paused; } disp_guard_t;
static inline void disp_guard_release(disp_guard_t *g) { if (g->paused) display_unpause(); }
#define DISPLAY_PAUSE(g, ms) disp_guard_t g __attribute__((cleanup(disp_guard_release))) = { display_pause_for_spi(ms) }

/* ── Stock-file integrity check ────────────────────────────────────────
 * STOCK_FILES (generated, see stock_files.h) is every file shipped in data/
 * at build time, except images/themes/ (the Theme Manager owns those). Detects one actually missing from LittleFS — a skipped
 * hotpatch, a manual delete, a corrupted write. Complements the
 * version-string mismatch banner (that catches "wrong overall version";
 * this catches "right version, something specific is gone"). Points the
 * user at a full LittleFS Recovery, not a hotpatch retry: a hotpatch only
 * ships the delta since some release, which is exactly the mechanism that
 * can leave a gap, while a full reflash writes every stock file
 * unconditionally.
 *
 * Existence-only (stat()) — can't tell a stale file from a healthy one;
 * that heavier job (content verification + selective re-fetch) is the
 * separate, user-triggered POST /api/stock_repair (see stock_repair_task()
 * further down this file).
 *
 * Run explicitly, off the httpd request path (once at boot, and again
 * after a hotpatch/webui-pull completes), not lazily inside api_status()
 * — ~380 stat() calls is cheaper than esp_littlefs_info()'s full walk, but
 * still risks becoming the same kind of blocking cost inside a request
 * handler. Not re-run after a plain file upload/delete — a stale reading
 * until next reboot is an acceptable trade there.
 *
 * Paused in small batches (STOCK_CHECK_BATCH files at a time), not one
 * pause for the whole scan — under concurrent WiFi/MQTT/flash traffic each
 * stat() can cost tens of ms (measured 13-15s total for 384 files on a
 * busy boot), and holding display_task parked for the whole scan froze the
 * boot splash or WeatherLive animation solid.
 *
 * BATCH=32/60ms (an earlier attempt) still read as a freeze: ~35ms/file
 * means ~1.1s paused per batch against only 60ms resumed — not long enough
 * to render a visibly different frame before the next pause. This smaller
 * batch + a gap close to one full render tick trades a longer total scan
 * for many brief, visible-motion pauses instead of one long freeze. */
#define STOCK_CHECK_BATCH 6
static int  s_stock_missing_count       = 0;
static char s_stock_missing_example[128] = "";

/* Full list backing the missing-files exception picker (GET
 * /api/stock_missing_list) — s_stock_missing_count above is the true total;
 * this is capped separately since the picker only needs enough entries to
 * build its checkbox tree, not an unbounded dump. 128 is generous headroom
 * over any real STOCK_FILES_COUNT-sized miss.
 *
 * Lazily-allocated PSRAM buffer, NOT a static internal-RAM array: this
 * table (128*96 = 12288 B) plus the two exception-snapshot copies below
 * shipped for one release as `static` internal-DRAM arrays and shrank the
 * device's permanent internal heap by ~24 KB — confirmed by comparing a
 * `heap_init` DRAM region's address/size across boot logs from before and
 * after that change (address shifted +24592 B, length shrank 24576 B —
 * matching to within alignment rounding). On this device's already-scarce
 * internal-RAM budget that was enough to cause task-creation failures and
 * lwip out-of-memory errors within 15 s of boot, eventually crashing.
 * PSRAM is plentiful and this data has no DMA need, so it belongs there —
 * same pattern as display.c's wl_fb(). */
#define STOCK_MISSING_LIST_MAX 128
static char (*s_stock_missing_paths)[96] = NULL;
static int  s_stock_missing_list_count = 0;   /* entries actually stored, <= STOCK_MISSING_LIST_MAX */

/* Returns the lazily-allocated PSRAM backing store for s_stock_missing_paths,
 * allocating it on first use and keeping it forever after (same lifetime as
 * a static array, just not carved out of internal DRAM at link time).
 * Returns NULL if PSRAM is exhausted; callers must treat that as "can't
 * record this entry" rather than dereference it. */
static char (*stock_missing_paths_buf(void))[96]
{
    if (!s_stock_missing_paths)
        s_stock_missing_paths = heap_caps_malloc(STOCK_MISSING_LIST_MAX * sizeof(*s_stock_missing_paths),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_stock_missing_paths;
}

/* True if `relpath` (a STOCK_FILES entry, no leading "/spiffs/") is covered
 * by one of the snapshotted exception entries — an exact match, or a
 * directory-prefix match when the entry ends in '/'. Shared by
 * stock_files_check() and stock_repair_task()'s verify pass so an excepted
 * file is never counted as missing AND never offered for repair. */
static bool stock_path_excepted(const char exc[][STOCK_EXCEPTION_PATH_LEN], uint8_t exc_count,
                                 const char *relpath)
{
    for (uint8_t i = 0; i < exc_count; i++) {
        size_t elen = strlen(exc[i]);
        if (elen == 0) continue;
        if (exc[i][elen - 1] == '/') {
            if (strncmp(relpath, exc[i], elen) == 0) return true;
        } else if (strcmp(relpath, exc[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void stock_files_check(void)
{
    int  missing = 0;
    char example[128] = "";
    struct stat st;

    /* Snapshot once, not per-file — hundreds of config_lock() calls across
     * this batched scan would be needless churn against a list that can't
     * change mid-scan (the web UI only saves exceptions between scans).
     *
     * PSRAM, NOT a stack local and NOT a static internal-RAM array: a plain
     * stack-local here once overflowed this task's entire 3072 B stack on
     * its own (32*96 = 3072 B, before any of this function's other locals
     * or call frames). The follow-up fix made it `static` instead, which
     * avoided the overflow but permanently carved 3072 B out of internal
     * DRAM at link time — see stock_missing_paths_buf() above for the full
     * heap_init evidence that this class of fix was itself a regression.
     * Lazily allocating from PSRAM avoids both problems. */
    static char (*exc_buf)[STOCK_EXCEPTION_PATH_LEN] = NULL;
    if (!exc_buf)
        exc_buf = heap_caps_malloc(STOCK_EXCEPTION_MAX * sizeof(*exc_buf),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t exc_count = 0;
    if (exc_buf) {
        config_lock();
        const nextube_config_t *scan_cfg = config_get();
        exc_count = scan_cfg->stock_exception_count;
        memcpy(exc_buf, scan_cfg->stock_exception_paths, STOCK_EXCEPTION_MAX * sizeof(*exc_buf));
        config_unlock();
    }

    char (*missing_buf)[96] = stock_missing_paths_buf();
    s_stock_missing_list_count = 0;

    for (size_t i = 0; i < STOCK_FILES_COUNT; i += STOCK_CHECK_BATCH) {
        size_t end = i + STOCK_CHECK_BATCH;
        if (end > STOCK_FILES_COUNT) end = STOCK_FILES_COUNT;

        if (!display_pause_for_spi(5000)) {
            ESP_LOGW(TAG, "stock_files_check: display task did not pause at file %u/%u — "
                          "stopping early this boot", (unsigned)i, (unsigned)STOCK_FILES_COUNT);
            return;   /* leave s_stock_missing_count untouched — same "skip this
                       * boot's check" behavior the old single-pause version had */
        }
        for (size_t j = i; j < end; j++) {
            if (stock_path_excepted(exc_buf, exc_count, STOCK_FILES[j])) continue;
            char vpath[320];
            snprintf(vpath, sizeof(vpath), "/spiffs/%s", STOCK_FILES[j]);
            if (stat(vpath, &st) != 0) {
                missing++;
                if (example[0] == '\0') snprintf(example, sizeof(example), "%s", STOCK_FILES[j]);
                if (missing_buf && s_stock_missing_list_count < STOCK_MISSING_LIST_MAX)
                    snprintf(missing_buf[s_stock_missing_list_count++],
                             sizeof(missing_buf[0]), "%s", STOCK_FILES[j]);
            }
        }
        display_unpause();
        vTaskDelay(pdMS_TO_TICKS(150));   /* ~one full display-task render tick —
                                            * long enough for a visibly different
                                            * frame to actually land, not just a
                                            * yield the scheduler immediately
                                            * hands back */
    }

    /* web/index.html special case — see the generation comment in the root
     * CMakeLists.txt for why this pair isn't a plain manifest entry. Small
     * enough to get its own quick pause rather than folding into the loop
     * above. Not exception-eligible — it's the web UI itself, never a
     * deliberately-removed theme. */
    if (display_pause_for_spi(5000)) {
        bool idx_missing = stat("/spiffs/web/index.html", &st) != 0 &&
                            stat("/spiffs/web/index.html.gz", &st) != 0;
        display_unpause();
        if (idx_missing) {
            missing++;
            if (example[0] == '\0') snprintf(example, sizeof(example), "web/index.html(.gz)");
            if (missing_buf && s_stock_missing_list_count < STOCK_MISSING_LIST_MAX)
                snprintf(missing_buf[s_stock_missing_list_count++],
                         sizeof(missing_buf[0]), "web/index.html(.gz)");
        }
    }

    s_stock_missing_count = missing;
    snprintf(s_stock_missing_example, sizeof(s_stock_missing_example), "%s", example);

    if (missing > 0)
        ESP_LOGW(TAG, "stock-file count check: %d missing (e.g. '%s') — LittleFS Recovery recommended",
                 missing, example);
    else
        ESP_LOGI(TAG, "stock-file count check: all %u present", (unsigned)(STOCK_FILES_COUNT + 1));
}

/* True from the moment a stock-file scan is queued until it has finished (it runs on the shared network task, see np_job_tick()).
 * stock_files_check() writes shared statics (s_stock_missing_*, the exception-snapshot buffer) and must never run twice at once;
 * every caller goes through stock_files_check_spawn(), which refuses while this is set. */
static bool s_stock_scan_active = false;
static volatile bool s_stock_scan_pending = false;   /* queued for the shared network task */
static int64_t       s_stock_scan_due_us;            /* earliest start: the 5 s grace after the trigger */

/* A Theme Manager job (catalog refresh / pack install) and a stock-file scan must never overlap: both pause the display and read or
 * write flash. s_tm_busy is set from the moment a job is queued until it ends; a scan asked for in that time is remembered and run
 * right after (see tm_job), and a job is refused while a scan is running (see tm_start). */
static volatile bool s_tm_busy = false;
static volatile bool s_stock_spawn_deferred = false;

/* Queues a stock-file scan unless one is already queued or running (a no-op then: its result will be fresh shortly anyway, and a
 * second instance would corrupt the shared state above). The scan runs on the shared network task, whose stack is allocated once at
 * boot, instead of in a task of its own: that task's 4 KB stack was carved out of the largest free internal block every time and
 * left the heap more fragmented afterwards. It starts 5 s after the trigger, so a post-update resume is not chopped up by the scan's
 * first batch pauses. Always returns true; every scan in this file should be requested through this. */
static bool stock_files_check_spawn(void)
{
    if (s_tm_busy) { s_stock_spawn_deferred = true; return true; }   /* run after the theme job */
    if (s_stock_scan_active) return true;
    heap_watch("stock scan start");
    s_stock_scan_active = true;
    s_stock_scan_due_us = esp_timer_get_time() + 5LL * 1000000;
    s_stock_scan_pending = true;
    periodic_net_poll_force("jobs");   /* wakes the shared network task, which runs np_job_tick() */
    return true;
}

/* Called after every /api/settings save, given the exception count from
 * BEFORE this save was applied (the caller reads that one field under
 * config_lock() itself — cheap, unlike snapshotting the whole 3072 B
 * exception array onto the httpd worker's stack just to diff it, which is
 * exactly the kind of stack cost this file just got bitten by elsewhere).
 *
 * The exception picker's whole point is instant feedback ("I just
 * excepted this file, the banner should clear now") — waiting for the
 * next full stock_files_check() (next boot, or a manual Verify & Repair
 * click, which itself takes 13-15s scanning hundreds of files) would make
 * the picker feel broken, and the fix wouldn't visibly do anything: the
 * count/example shown by /api/status come from
 * s_stock_missing_count/s_stock_missing_example, which a settings save
 * alone never touches.
 *
 * Cheap in the common case (count unchanged: bail immediately) and in the
 * "exceptions only grew" case, by re-filtering the already-cached missing
 * list from the last real scan instead of touching the filesystem again —
 * exact, since anything already in that cache and now excepted can only
 * need to disappear, never reappear. A *shrink* (an exception removed) is
 * different: a file dropped from the cache when it was first excepted
 * needs a real stat() to know whether it's still actually missing before
 * it can be reported again, which the cache alone can't answer. Same
 * problem if the cache was truncated (more than STOCK_MISSING_LIST_MAX
 * missing) — it doesn't hold every missing file to filter in the first
 * place. Both of those fall back to a real background rescan (the same
 * one the boot-time auto-check queues); the banner catches up once it
 * finishes. */
static void stock_missing_recompute_after_exception_change(uint8_t old_exc_count)
{
    config_lock();
    uint8_t new_exc_count = config_get()->stock_exception_count;
    config_unlock();

    if (new_exc_count == old_exc_count) return;   /* this save didn't touch exceptions */

    bool shrank          = new_exc_count < old_exc_count;
    bool cache_truncated = s_stock_missing_count > s_stock_missing_list_count;
    if (shrank || cache_truncated) {
        stock_files_check_spawn();
        return;
    }
    if (s_stock_missing_list_count == 0) return;

    char (*exc_buf)[STOCK_EXCEPTION_PATH_LEN] =
        heap_caps_malloc(STOCK_EXCEPTION_MAX * sizeof(*exc_buf), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!exc_buf) return;   /* stale count stands until the next real scan */

    uint8_t exc_count;
    config_lock();
    const nextube_config_t *cfg = config_get();
    exc_count = cfg->stock_exception_count;
    memcpy(exc_buf, cfg->stock_exception_paths, STOCK_EXCEPTION_MAX * sizeof(*exc_buf));
    config_unlock();

    char (*missing_buf)[96] = stock_missing_paths_buf();
    if (missing_buf) {
        int kept = 0;
        for (int i = 0; i < s_stock_missing_list_count; i++) {
            if (!stock_path_excepted(exc_buf, exc_count, missing_buf[i])) {
                if (kept != i) memcpy(missing_buf[kept], missing_buf[i], sizeof(missing_buf[0]));
                kept++;
            }
        }
        s_stock_missing_list_count = kept;
        s_stock_missing_count = kept;
        snprintf(s_stock_missing_example, sizeof(s_stock_missing_example), "%s", kept > 0 ? missing_buf[0] : "");
    }
    free(exc_buf);
}

/* Called after every successful /api/file/upload with the uploaded path
 * relative to /spiffs (no leading slash, matching STOCK_FILES[]/the cached
 * missing-list entries). If it exactly matches one of the currently-cached
 * missing files — e.g. the user just re-uploaded a deliberately-deleted
 * theme folder through the LittleFS Files browser — this removes it from
 * the cache and decrements the count immediately, the same "instant
 * feedback without a full rescan" reasoning as
 * stock_missing_recompute_after_exception_change() above, just triggered
 * by an upload finishing instead of a settings save. A path not found in
 * the cache (never was missing, or the cache was truncated past it) is
 * left untouched; the next real scan (boot, or a manual Verify & Repair)
 * still catches up eventually. */
static void stock_missing_mark_present(const char *relpath)
{
    for (int i = 0; i < s_stock_missing_list_count; i++) {
        if (strcmp(s_stock_missing_paths[i], relpath) != 0) continue;
        for (int j = i; j < s_stock_missing_list_count - 1; j++)
            memcpy(s_stock_missing_paths[j], s_stock_missing_paths[j + 1], sizeof(s_stock_missing_paths[0]));
        s_stock_missing_list_count--;
        if (s_stock_missing_count > 0) s_stock_missing_count--;
        snprintf(s_stock_missing_example, sizeof(s_stock_missing_example), "%s",
                 s_stock_missing_list_count > 0 ? s_stock_missing_paths[0] : "");
        return;
    }
}

/* Forward declaration — defined in the static-file section below */
static const char *content_type(const char *p);

/* ── In-RAM log ring buffer ────────────────────────────────────────── */
/* Captures all ESP_LOG* output into a circular buffer so the web UI
 * can display recent device logs without a serial connection.
 * Lines are stored in internal SRAM; nothing is written to flash.
 * The buffer holds the most recent LOG_RING_LINES entries and wraps
 * silently once full — oldest lines are overwritten. */
#define LOG_RING_LINES  200
#define LOG_LINE_LEN   160

static char              s_log_ring[LOG_RING_LINES][LOG_LINE_LEN];
static int               s_log_head  = 0;   /* next write slot */
static int               s_log_count = 0;   /* lines stored (≤ LOG_RING_LINES) */
static SemaphoreHandle_t s_log_mutex = NULL;

/* vprintf hook: intercept all ESP_LOG* output, buffer it, then forward
 * to UART via the standard vprintf so the serial monitor still works. */
static int log_vprintf_hook(const char *fmt, va_list args)
{
    /* Take a copy of the va_list BEFORE consuming it with vprintf so we
     * can format the same message a second time into our ring buffer. */
    va_list copy;
    va_copy(copy, args);

    /* Forward to UART as normal */
    int ret = vprintf(fmt, args);

    /* Buffer the formatted line — non-blocking try-lock so we never
     * stall the logging task if the HTTP handler holds the mutex. */
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, 0) == pdTRUE) {
        char line[LOG_LINE_LEN];
        vsnprintf(line, sizeof(line), fmt, copy);
        /* Strip trailing newline / carriage-return */
        int n = (int)strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n > 0) {
            memcpy(s_log_ring[s_log_head], line, LOG_LINE_LEN);
            s_log_ring[s_log_head][LOG_LINE_LEN - 1] = '\0';
            s_log_head  = (s_log_head + 1) % LOG_RING_LINES;
            if (s_log_count < LOG_RING_LINES) s_log_count++;
        }
        xSemaphoreGive(s_log_mutex);
    }

    va_end(copy);
    return ret;
}

#include "fw_version.h"
#define HW_VER "1.31"

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    /* No Access-Control-Allow-Origin header on purpose.  The web UI is
     * served from the same origin as the API (the device's IP / mDNS
     * hostname), so it doesn't need CORS to reach the /api routes.
     * Omitting the header makes browsers block any cross-origin script
     * from reading the response — defence-in-depth against information
     * leakage even on auth-open routes like /api/status. */
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

/* Logs the requesting client's IP address alongside `reason`, for the
 * reboot-triggering handlers below. These all end in esp_restart(), and a
 * user-observed reboot with no crash/panic and no config wipe can only be
 * one of a small set of authenticated HTTP calls — this makes the next
 * occurrence traceable to a specific device on the LAN instead of a guess. */
static void log_restart_request(httpd_req_t *req, const char *reason)
{
    int sock = httpd_req_to_sockfd(req);
    /* Raw buffer, not a typed struct -- big enough for either a
     * sockaddr_in or a sockaddr_in6 peer address, regardless of whether
     * this build's lwip has IPv6 compiled in. */
    char sa_buf[28] = { 0 };
    socklen_t len = sizeof(sa_buf);
    char ip[48] = "unknown";
    if (sock >= 0 && getpeername(sock, (struct sockaddr *)sa_buf, &len) == 0) {
        struct sockaddr *sa = (struct sockaddr *)sa_buf;
        const uint8_t *b = NULL;   /* 4 bytes for a plain IPv4 peer, or the
                                     * last 4 of an IPv4-mapped IPv6 peer
                                     * (::ffff:a.b.c.d) -- httpd's listen
                                     * socket is IPv6 dual-stack when
                                     * available, so LAN clients often show
                                     * up mapped. */
        if (sa->sa_family == AF_INET) {
            b = (const uint8_t *)&((struct sockaddr_in *)sa_buf)->sin_addr;
        }
#ifdef AF_INET6
        else if (sa->sa_family == AF_INET6) {
            const uint8_t *b16 = (const uint8_t *)&((struct sockaddr_in6 *)sa_buf)->sin6_addr;
            bool v4_mapped = b16[10] == 0xff && b16[11] == 0xff;
            for (int i = 0; v4_mapped && i < 10; i++) v4_mapped = (b16[i] == 0);
            if (v4_mapped) b = &b16[12];
        }
#endif
        if (b) {
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        }
    }
    ESP_LOGW(TAG, "[restart] %s requested by client %s", reason, ip);
}

/* ── Auth ———──────────────────────────────────────────────────────────
 * REQUIRE_AUTH(r) is the gate macro applied to mutation handlers.  When no
 * admin password has been set (auth is disabled), it is a no-op and every
 * caller proceeds unconditionally.  Once a password is set, it requires a
 * valid Bearer token and returns 401 on failure.
 *
 * This makes authentication opt-in: the device works without any password
 * by default, and the user can enable auth from the System tab. */
#define REQUIRE_AUTH(r) do { \
    if (auth_is_password_set() && !auth_check_request(r)) { \
        httpd_resp_set_status((r), "401 Unauthorized"); \
        httpd_resp_set_type((r), "application/json"); \
        return httpd_resp_sendstr((r), "{\"error\":\"unauthorized\"}"); \
    } \
} while (0)

static esp_err_t api_ping(httpd_req_t *r)       { return send_json(r, "{\"status\":\"ok\"}"); }
static esp_err_t api_fw_ver(httpd_req_t *r)      { return send_json(r, "{\"version\":\"" FW_VERSION_STR "\"}"); }
static esp_err_t api_hw_ver(httpd_req_t *r)      { return send_json(r, "{\"version\":\"" HW_VER "\"}"); }

/* POST /api/audio/play  { "file": "/spiffs/audio/bell.wav" }
 * Triggers a one-shot preview of the named audio file at the current volume. */
static esp_err_t api_audio_play(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char buf[256] = {0};
    int  n = httpd_req_recv(r, buf, sizeof(buf) - 1);
    if (n <= 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "No body"), ESP_FAIL;
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad JSON"), ESP_FAIL;
    cJSON *f = cJSON_GetObjectItem(root, "file");
    if (!f || !f->valuestring || f->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing file"), ESP_FAIL;
    }
    /* Validate path: must start with /spiffs/audio/ and not contain ".." */
    if (strncmp(f->valuestring, "/spiffs/audio/", 14) != 0 || strstr(f->valuestring, "..")) {
        cJSON_Delete(root);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid audio path"), ESP_FAIL;
    }
    ESP_LOGI("web_srv", "Audio test: %s", f->valuestring);
    audio_play_file(f->valuestring);
    cJSON_Delete(root);
    return send_json(r, "{\"status\":\"ok\"}");
}


/* POST /api/weather  — inject externally-sourced weather data.
 * Lets a home-automation system push its own (e.g. multi-provider averaged)
 * reading instead of the firmware fetching a single online service.
 *
 * Body — all fields optional (omitted fields keep their current value):
 *   {
 *     "temp_c":       15.3,              // °C   — send EITHER temp_c OR temp_f,
 *     "temp_f":       59.5,              // °F     not both (temp_c wins if both)
 *     "humidity":     65,                // %
 *     "condition":    "Cloudy",          // free text shown on the panel
 *     "icon":         "overcastClouds",  // one of the 8 built-in icon names
 *     "weather_code": 3,                 // WMO code; fills icon/condition if absent
 *     "lat":          51.3,              // optional — for Sunrise & Sunset panel
 *     "lon":          -114.0
 *   }
 *
 * Temperature is stored internally in Celsius (the UI °C/°F setting only affects
 * display), so temp_f is converted on ingest.  You only need to send ONE of the
 * two temperature fields in whichever unit your source produces.
 *
 * Set weather_source = "external" (Display/weather settings) so the internal
 * poller doesn't overwrite the pushed value.  Honours auth like every other
 * mutation route: with no admin password set this is open on the LAN;
 * otherwise send the Bearer token. */
static esp_err_t api_post_weather(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char buf[512] = {0};
    int  n = httpd_req_recv(r, buf, sizeof(buf) - 1);
    if (n <= 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "No body"), ESP_FAIL;
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad JSON"), ESP_FAIL;

    /* Temperature: accept temp_c (Celsius) or temp_f (Fahrenheit).  Stored
     * internally as Celsius, so temp_f is converted on ingest.  Send only one;
     * temp_c takes precedence if both are present. */
    float temp = NAN, hum = NAN;
    cJSON *jt = cJSON_GetObjectItem(root, "temp_c");
    cJSON *jf = cJSON_GetObjectItem(root, "temp_f");
    cJSON *jh = cJSON_GetObjectItem(root, "humidity");
    if (cJSON_IsNumber(jt))      temp = (float)jt->valuedouble;
    else if (cJSON_IsNumber(jf)) temp = ((float)jf->valuedouble - 32.0f) * 5.0f / 9.0f;
    if (cJSON_IsNumber(jh)) hum  = (float)jh->valuedouble;

    const char *cond = NULL, *icon = NULL;
    cJSON *jc = cJSON_GetObjectItem(root, "condition");
    cJSON *ji = cJSON_GetObjectItem(root, "icon");
    if (cJSON_IsString(jc)) cond = jc->valuestring;
    if (cJSON_IsString(ji)) icon = ji->valuestring;

    int wmo = -1;
    cJSON *jw = cJSON_GetObjectItem(root, "weather_code");
    if (cJSON_IsNumber(jw)) wmo = jw->valueint;

    weather_set_external(temp, hum, cond, icon, wmo);

    cJSON *jlat = cJSON_GetObjectItem(root, "lat");
    cJSON *jlon = cJSON_GetObjectItem(root, "lon");
    if (cJSON_IsNumber(jlat) && cJSON_IsNumber(jlon))
        weather_set_external_location((float)jlat->valuedouble, (float)jlon->valuedouble);

    cJSON_Delete(root);
    return send_json(r, "{\"status\":\"ok\"}");
}

static esp_err_t api_get_settings(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    /* config_to_json(false) omits the WiFi password and adds "has_password"
     * itself — no second cJSON_Parse/Print pass, which both halves the internal-
     * heap pressure and removes a silent failure mode that used to serve "{}"
     * (an empty config → the whole UI shows no data) when the re-parse failed
     * under heap fragmentation. */
    char *j = config_to_json(false);
    if (!j) {
        /* largest uses CAP_8BIT too — CAP_INTERNAL alone also counts
         * reclaimed IRAM, which can't hold a real allocation and inflates
         * this number (see the note on the periodic heap log in main.c). */
        ESP_LOGE(TAG, "/api/settings: config_to_json FAILED — internal heap free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "config serialize failed"), ESP_FAIL;
    }
    ESP_LOGI(TAG, "/api/settings: serving %u B", (unsigned)strlen(j));
    esp_err_t ret = send_json(r, j);
    free(j);
    return ret;
}

/* GET /api/backup — full config including WiFi password, for explicit user backup.
 * Separate from GET /api/settings so the password is not exposed on the general
 * settings endpoint but IS preserved in backup/restore round-trips. */
static esp_err_t api_backup(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char *j = config_to_json(true);   /* backup includes the WiFi password */
    esp_err_t ret = send_json(r, j ? j : "{}");
    free(j);
    return ret;
}

/* One-shot esp_timer: reconnect WiFi after the HTTP response has been sent.
 * Calling esp_wifi_disconnect() inside the HTTP handler kills the live TCP
 * connection before the response reaches the browser and can also disrupt
 * SPI DMA in-flight, blanking the displays.
 *
 * The HTTP server is stopped before reconnecting and restarted once the new
 * IP address is obtained.  Without stop/restart the httpd listening socket
 * becomes stale on the new interface and the device is unreachable until
 * the next reboot. */
#include "esp_timer.h"
static void reconnect_timer_cb(void *arg)
{
    /* Do NOT stop the server here.  If the STA fails to connect (wrong
     * password, AP out of range) the IP event never fires and the server
     * would be unreachable on BOTH STA and AP until reboot.
     * Instead we keep the server running on the AP (192.168.4.1) so the
     * user can always reach the UI to fix credentials, and we
     * stop+restart it only once a new STA IP is actually obtained. */
    s_server_restart_pending = true;
    bool reboot_needed = wifi_manager_reconnect_sta();
    if (reboot_needed) {
        /* Static IP is enabled — this no-reboot path doesn't restart DHCP or
         * reapply static-IP fields, so the change (or a static IP that was
         * just enabled/edited) won't fully take effect until next boot.
         * Not surfaced to the UI yet — just logged so the info exists at
         * the call site for a future UI affordance. */
        ESP_LOGW(TAG, "Static IP is enabled — a reboot is required for IP/DNS changes to fully apply");
    }
}
static esp_timer_handle_t s_reconnect_timer = NULL;
static void schedule_wifi_reconnect(void)
{
    if (!s_reconnect_timer) {
        esp_timer_create_args_t a = {
            .callback = reconnect_timer_cb,
            .name     = "wifi_reconnect",
        };
        esp_timer_create(&a, &s_reconnect_timer);
    }
    /* Cancel any pending timer, then fire once after 600 ms */
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, 1500 * 1000);  /* 1500 ms in µs */
}

static esp_err_t api_post_settings(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    /* Back the WeatherLive realtime animation off for the duration of this
     * save: config_set_json() writes NVS (flash) and this handler runs on the
     * httpd task, which shares core 1 with the higher-priority display task.
     * Without this, WeatherLive's every-tick render starves the save and it can
     * time out.  Self-expiring, so a crash/early-return can't wedge the clock. */
    display_busy_hint(3000);
    /* Validate on the signed content_len first (catches negative/absent header),
     * then widen to size_t so all subsequent arithmetic is unsigned.  The cap is
     * 8192 (matches the NVS backup-blob limit) — the full settings payload has
     * grown well past 4 KB as panels/fields were added, and a too-low cap here
     * silently 400s every save (config never persists). */
    if (r->content_len <= 0 || r->content_len > 8192) {
        ESP_LOGE(TAG, "/api/settings POST rejected: content_len=%d (limit 8192)",
                 (int)r->content_len);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad length"), ESP_FAIL;
    }
    size_t len = (size_t)r->content_len;
    char *buf = malloc((size_t)len + 1);
    if (!buf) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;
    size_t rx = 0;
    while (rx < len) {
        int n = httpd_req_recv(r, buf + rx, len - rx);
        if (n <= 0) { free(buf); return ESP_FAIL; }
        rx += (size_t)n;
    }
    buf[len] = '\0';

    /* Snapshot fields BEFORE applying the new config so we can detect what
     * actually changed.  We distinguish two classes:
     *
     *   1. WiFi credentials — handled with a live reconnect (no reboot).
     *   2. Boot-time feature flags — tasks are created once in app_main and
     *      cannot be started/stopped at runtime, so a reboot is required for
     *      changes to weather_enabled / youtube_enabled / mdns_enabled /
     *      mic_enabled / audio_enabled to take effect. */
    char old_ssid[64], old_pass[64], old_hostname[32];
    bool old_weather_en, old_youtube_en, old_mdns_en, old_mic_en, old_audio_en;
    char old_timezone[64];
    char old_ntp_servers[4][64];
    uint8_t old_invert_mask;
    uint8_t old_init_profile[6];
    uint8_t old_vcom[6];
    float   old_gamma[6];
    int8_t  old_col_offset[6], old_row_offset[6];
    uint8_t old_tube_brightness[6];
    uint8_t old_shadow_lift[6];
    uint8_t old_gain_r[6], old_gain_g[6], old_gain_b[6], old_black[6];
    uint8_t old_curve[3][6], old_ped[3][6];
    uint8_t old_stock_exc_count;
    if (s_lcd_preview_pending) lcd_preview_revert();   /* the tubes must show the saved values before they are compared with the new ones */
    config_lock();
    const nextube_config_t *old_cfg = config_get();
    old_stock_exc_count = old_cfg->stock_exception_count;
    strlcpy(old_ssid,     old_cfg->ssid,      sizeof(old_ssid));
    strlcpy(old_pass,     old_cfg->password,  sizeof(old_pass));
    strlcpy(old_hostname, old_cfg->hostname,  sizeof(old_hostname));
    old_weather_en   = old_cfg->weather_enabled;
    old_youtube_en   = old_cfg->youtube_enabled;
    old_mdns_en      = old_cfg->mdns_enabled;
    old_mic_en       = old_cfg->mic_enabled;
    old_audio_en     = old_cfg->audio_enabled;
    strlcpy(old_timezone, old_cfg->timezone, sizeof(old_timezone));
    memcpy(old_ntp_servers, old_cfg->ntp_servers, sizeof(old_ntp_servers));
    old_invert_mask  = old_cfg->lcd_invert_mask;
    memcpy(old_init_profile,    old_cfg->lcd_init_profile,    sizeof(old_init_profile));
    memcpy(old_vcom,            old_cfg->lcd_vcom,            sizeof(old_vcom));
    memcpy(old_gamma,           old_cfg->lcd_gamma,           sizeof(old_gamma));
    memcpy(old_col_offset,      old_cfg->lcd_col_offset,      sizeof(old_col_offset));
    memcpy(old_row_offset,      old_cfg->lcd_row_offset,      sizeof(old_row_offset));
    memcpy(old_tube_brightness, old_cfg->lcd_tube_brightness, sizeof(old_tube_brightness));
    memcpy(old_shadow_lift,     old_cfg->lcd_shadow_lift,     sizeof(old_shadow_lift));
    memcpy(old_gain_r,          old_cfg->lcd_gain_r,          sizeof(old_gain_r));
    memcpy(old_gain_g,          old_cfg->lcd_gain_g,          sizeof(old_gain_g));
    memcpy(old_gain_b,          old_cfg->lcd_gain_b,          sizeof(old_gain_b));
    memcpy(old_black,           old_cfg->lcd_black_level,     sizeof(old_black));
    memcpy(old_curve[0], old_cfg->lcd_curve_r,    6); memcpy(old_curve[1], old_cfg->lcd_curve_g,    6);
    memcpy(old_curve[2], old_cfg->lcd_curve_b,    6); memcpy(old_ped[0],   old_cfg->lcd_pedestal_r, 6);
    memcpy(old_ped[1],   old_cfg->lcd_pedestal_g, 6); memcpy(old_ped[2],   old_cfg->lcd_pedestal_b, 6);
    config_unlock();

    bool ok = config_set_json(buf, len);
    free(buf);
    display_config_changed();  /* NVS write done — lift busy backoff + force re-render of live config changes */
    stock_missing_recompute_after_exception_change(old_stock_exc_count);

    uint8_t new_brightness;
    bool    new_audio_enabled;
    uint8_t new_volume;
    char    new_ssid[64], new_pass[64], new_hostname[32];
    bool    new_weather_en, new_youtube_en, new_mdns_en, new_mic_en;
    float   new_sht30_offset;
    char    new_timezone[64];
    char    new_ntp_servers[4][64];
    uint8_t new_invert_mask;
    uint8_t new_init_profile[6];
    uint8_t new_vcom[6];
    float   new_gamma[6];
    int8_t  new_col_offset[6], new_row_offset[6];
    uint8_t new_tube_brightness[6];
    uint8_t new_shadow_lift[6];
    uint8_t new_gain_r[6], new_gain_g[6], new_gain_b[6], new_black[6];
    uint8_t new_curve[3][6], new_ped[3][6];
    config_lock();
    const nextube_config_t *new_cfg = config_get();
    new_brightness    = new_cfg->led_brightness;
    new_audio_enabled = new_cfg->audio_enabled;
    new_volume        = new_cfg->volume;
    strlcpy(new_ssid,     new_cfg->ssid,      sizeof(new_ssid));
    strlcpy(new_pass,     new_cfg->password,  sizeof(new_pass));
    strlcpy(new_hostname, new_cfg->hostname,  sizeof(new_hostname));
    new_weather_en    = new_cfg->weather_enabled;
    new_youtube_en    = new_cfg->youtube_enabled;
    new_mdns_en       = new_cfg->mdns_enabled;
    new_mic_en        = new_cfg->mic_enabled;
    new_sht30_offset  = new_cfg->sht30_temp_offset;
    strlcpy(new_timezone, new_cfg->timezone, sizeof(new_timezone));
    memcpy(new_ntp_servers, new_cfg->ntp_servers, sizeof(new_ntp_servers));
    new_invert_mask   = new_cfg->lcd_invert_mask;
    memcpy(new_init_profile,    new_cfg->lcd_init_profile,    sizeof(new_init_profile));
    memcpy(new_vcom,            new_cfg->lcd_vcom,            sizeof(new_vcom));
    memcpy(new_gamma,           new_cfg->lcd_gamma,           sizeof(new_gamma));
    memcpy(new_col_offset,      new_cfg->lcd_col_offset,      sizeof(new_col_offset));
    memcpy(new_row_offset,      new_cfg->lcd_row_offset,      sizeof(new_row_offset));
    memcpy(new_tube_brightness, new_cfg->lcd_tube_brightness, sizeof(new_tube_brightness));
    memcpy(new_shadow_lift,     new_cfg->lcd_shadow_lift,     sizeof(new_shadow_lift));
    memcpy(new_gain_r,          new_cfg->lcd_gain_r,          sizeof(new_gain_r));
    memcpy(new_gain_g,          new_cfg->lcd_gain_g,          sizeof(new_gain_g));
    memcpy(new_gain_b,          new_cfg->lcd_gain_b,          sizeof(new_gain_b));
    memcpy(new_black,           new_cfg->lcd_black_level,     sizeof(new_black));
    memcpy(new_curve[0], new_cfg->lcd_curve_r,    6); memcpy(new_curve[1], new_cfg->lcd_curve_g,    6);
    memcpy(new_curve[2], new_cfg->lcd_curve_b,    6); memcpy(new_ped[0],   new_cfg->lcd_pedestal_r, 6);
    memcpy(new_ped[1],   new_cfg->lcd_pedestal_g, 6); memcpy(new_ped[2],   new_cfg->lcd_pedestal_b, 6);
    config_unlock();

    leds_set_brightness(new_brightness);
    /* Apply the NTP settings only when they actually changed — ntp_apply_servers()
     * stops and re-inits the SNTP engine (and both it and ntp_apply_timezone()
     * log a line), which is needless churn on every save of an unrelated field. */
    if (strcmp(new_timezone, old_timezone) != 0)
        ntp_apply_timezone();
    if (memcmp(new_ntp_servers, old_ntp_servers, sizeof(new_ntp_servers)) != 0)
        ntp_apply_servers();
    /* audio_set_enabled() only logs "takes effect after reboot" — skip it (and
     * the log line) when the flag is unchanged. */
    if (new_audio_enabled != old_audio_en)
        audio_set_enabled(new_audio_enabled);
    /* Volume applies live — unlike audio_enabled, which needs a reboot. Without
     * this the slider only wrote config.json and the new level was not picked
     * up until audio_deferred_start() re-read it on the next boot. */
    audio_set_volume(new_volume);
    sht30_set_offset(new_sht30_offset);   /* live-update sensor calibration — no restart needed */
    if (new_invert_mask != old_invert_mask)
        display_apply_invert_mask(new_invert_mask);
    if (memcmp(new_init_profile, old_init_profile, sizeof(new_init_profile)) != 0)
        display_apply_init_profiles(new_init_profile);
    if (memcmp(new_vcom, old_vcom, sizeof(new_vcom)) != 0)
        display_apply_tube_vcom(new_vcom);
    {
        bool gamma_changed = false;
        for (int i = 0; i < 6; i++) {
            if (fabsf(new_gamma[i] - old_gamma[i]) > 0.005f) { gamma_changed = true; break; }
        }
        if (gamma_changed) display_apply_tube_gamma(new_gamma);
    }
    if (memcmp(new_col_offset, old_col_offset, sizeof(new_col_offset)) != 0 ||
        memcmp(new_row_offset, old_row_offset, sizeof(new_row_offset)) != 0)
        display_apply_tube_offsets(new_col_offset, new_row_offset);
    if (memcmp(new_tube_brightness, old_tube_brightness, sizeof(new_tube_brightness)) != 0)
        display_apply_tube_brightness(new_tube_brightness);
    if (memcmp(new_shadow_lift, old_shadow_lift, sizeof(new_shadow_lift)) != 0)
        display_apply_tube_shadow_lift(new_shadow_lift);
    if (memcmp(new_gain_r, old_gain_r, sizeof(new_gain_r)) != 0 ||
        memcmp(new_gain_g, old_gain_g, sizeof(new_gain_g)) != 0 ||
        memcmp(new_gain_b, old_gain_b, sizeof(new_gain_b)) != 0 ||
        memcmp(new_black,  old_black,  sizeof(new_black))  != 0)
        display_apply_tube_color(new_gain_r, new_gain_g, new_gain_b, new_black);
    if (memcmp(new_curve, old_curve, sizeof(new_curve)) != 0 ||
        memcmp(new_ped,   old_ped,   sizeof(new_ped))   != 0)
        display_apply_tube_channel_curves(new_curve[0], new_curve[1], new_curve[2],
                                          new_ped[0], new_ped[1], new_ped[2]);
    lcd_applied_invalidate();   /* the tubes now match the saved config again */

    /* Boot-time feature flags or hostname changed — reboot required.
     * Hostname is baked into LWIP netif, DHCP option 12, and mDNS at start-up;
     * changing it live is not supported.  audio_enabled gates the deferred
     * audio task at boot, so toggling it also requires a reboot.
     * Respond first so the browser gets confirmation before the TCP connection
     * drops. */
    bool needs_reboot = (strcmp(old_hostname, new_hostname) != 0) ||
                        (old_weather_en != new_weather_en) ||
                        (old_youtube_en != new_youtube_en) ||
                        (old_mdns_en    != new_mdns_en)    ||
                        (old_mic_en     != new_mic_en)     ||
                        (old_audio_en   != new_audio_enabled);
    bool ssid_changed = (strcmp(old_ssid, new_ssid) != 0);
    bool pass_changed = (strcmp(old_pass, new_pass)  != 0);

    if (needs_reboot) {
        if (ssid_changed && strlen(new_ssid) > 0) {
            /* Don't gamble this reboot on credentials nobody has tried yet
             * just to pick up an unrelated boot-time flag change (this is
             * the "restore from backup during onboarding" case — a restored
             * backup's hostname/weather/etc. fields almost always differ
             * from a factory-reset device's, forcing this branch even
             * though the SSID itself needs no reboot). Start the same live
             * reconnect a plain SSID-only change uses (setup AP stays up
             * and reachable throughout) and defer the actual reboot until
             * wifi_manager_reboot_once_connected() sees STA confirm it.
             * Falls back to the immediate mark_untested_sta()
             * reboot-with-AP-fallback if STA doesn't confirm within 45s, so
             * genuinely wrong restored credentials still can't wedge the
             * device. */
            schedule_wifi_reconnect();
            wifi_manager_reboot_once_connected(45000);
            send_json(r, ok ? "{\"status\":\"ok\",\"reboot\":true}"
                            : "{\"status\":\"error\",\"reboot\":false}");
            return ESP_OK;
        }
        /* No SSID change involved — no WiFi risk, reboot immediately as before. */
        log_restart_request(r, "settings change");
        send_json(r, ok ? "{\"status\":\"ok\",\"reboot\":true}"
                        : "{\"status\":\"error\",\"reboot\":false}");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return ESP_OK;
    }

    if (ssid_changed && strlen(new_ssid) > 0) {
        /* SSID changed — need to switch networks; full disconnect + reconnect */
        schedule_wifi_reconnect();
    } else if (pass_changed) {
        /* Password-only change — update the driver config silently so the new
         * password is used on the next natural reconnect, without dropping the
         * live connection.  Avoids ~60 s of downtime on a config restore where
         * the backup omits or blanks the password field. */
        wifi_manager_apply_sta_credentials();
    }

    return send_json(r, ok ? "{\"status\":\"ok\"}" : "{\"status\":\"error\"}");
}

/* ── Auth —— request body helper ────────────────────────────────────
 * REQUIRE_AUTH macro is defined near the top of this file (before any
 * handler that uses it).  The JSON-body reader below is only used by the
 * auth handlers immediately following, so it lives here. */

/* Helper — read a JSON POST body up to max_len bytes and parse it.  Returns
 * a cJSON object (caller must cJSON_Delete) or NULL on error.  Sends a 400
 * error response itself on failure, so callers should just return on NULL.
 * Loops on httpd_req_recv to handle TCP fragmentation cleanly. */
static cJSON *read_json_body(httpd_req_t *r, size_t max_len)
{
    /* Validate on the signed content_len first, then widen to size_t. */
    if (r->content_len <= 0 || (size_t)r->content_len > max_len) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid body length");
        return NULL;
    }
    size_t len = (size_t)r->content_len;
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return NULL;
    }
    size_t rx = 0;
    while (rx < len) {
        int n = httpd_req_recv(r, buf + rx, len - rx);
        if (n <= 0) {
            free(buf);
            httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Read error");
            return NULL;
        }
        rx += (size_t)n;
    }
    buf[rx] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return NULL;
    }
    return root;
}

/* Stack-buffer variant of read_json_body(), for the many small debug/toggle
 * handlers that just want a couple of fields out of a body well under a few
 * hundred bytes and would rather not pay for a malloc.  buf must be at least
 * buf_sz bytes; on success the received body is left null-terminated inside
 * it.  Same contract as read_json_body(): returns a cJSON object (caller
 * must cJSON_Delete) or NULL, sending its own 400 response on failure — so
 * callers should just return on NULL. */
static cJSON *read_json_body_small(httpd_req_t *r, char *buf, size_t buf_sz)
{
    /* Reserve one byte for the null terminator, same as the malloc'd path
     * (which allocates max_len+1). */
    if (r->content_len <= 0 || (size_t)r->content_len >= buf_sz) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid body length");
        return NULL;
    }
    size_t len = (size_t)r->content_len;
    size_t rx = 0;
    while (rx < len) {
        int n = httpd_req_recv(r, buf + rx, len - rx);
        if (n <= 0) {
            httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Read error");
            return NULL;
        }
        rx += (size_t)n;
    }
    buf[rx] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return NULL;
    }
    return root;
}

/* POST /api/auth/set_password — enable authentication by setting a password.
 * Allowed only while no password has been set (auth disabled), so this
 * endpoint requires no Bearer token.  Once a password is set, future
 * changes go through /api/auth/change_password; to disable auth entirely
 * use /api/auth/disable. */
static esp_err_t api_auth_set_password(httpd_req_t *r)
{
    if (auth_is_password_set()) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r,
            "{\"error\":\"password_already_set\"}");
    }
    cJSON *body = read_json_body(r, 256);
    if (!body) return ESP_FAIL;

    cJSON *jpw = cJSON_GetObjectItem(body, "password");
    char pw_buf[80] = {0};
    if (cJSON_IsString(jpw) && jpw->valuestring)
        snprintf(pw_buf, sizeof(pw_buf), "%s", jpw->valuestring);
    cJSON_Delete(body);

    if (pw_buf[0] == '\0')
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "Missing password"), ESP_FAIL;

    esp_err_t err = auth_set_password(pw_buf);
    /* Wipe the local copy regardless of outcome. */
    memset(pw_buf, 0, sizeof(pw_buf));
    if (err == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "Password must be 6-64 chars"), ESP_FAIL;
    if (err != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Storage error"), ESP_FAIL;
    return send_json(r, "{\"status\":\"ok\"}");
}

/* POST /api/auth/login — exchange password for a session bearer token. */
static esp_err_t api_auth_login(httpd_req_t *r)
{
    if (auth_is_locked_out()) {
        httpd_resp_set_status(r, "429 Too Many Requests");
        char body[80];
        snprintf(body, sizeof(body),
                 "{\"error\":\"locked_out\",\"retry_after_s\":%d}",
                 auth_lockout_remaining_s());
        return httpd_resp_sendstr(r, body);
    }
    cJSON *body = read_json_body(r, 256);
    if (!body) return ESP_FAIL;

    cJSON *jpw = cJSON_GetObjectItem(body, "password");
    char pw_buf[80] = {0};
    if (cJSON_IsString(jpw) && jpw->valuestring)
        snprintf(pw_buf, sizeof(pw_buf), "%s", jpw->valuestring);
    cJSON_Delete(body);

    char *token = auth_login(pw_buf);
    memset(pw_buf, 0, sizeof(pw_buf));

    if (!token) {
        httpd_resp_set_status(r, "401 Unauthorized");
        httpd_resp_set_type(r, "application/json");
        return httpd_resp_sendstr(r, "{\"error\":\"bad_password\"}");
    }
    char resp[200];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"token\":\"%s\"}", token);
    free(token);
    return send_json(r, resp);
}

/* GET /api/auth/check — lightweight session-validity probe.
 * Returns 200 {"status":"ok"} if the bearer token is valid, 401 if not.
 * Used by the web UI as a pre-flight before starting a large upload (OTA /
 * LittleFS) so an expired session is caught before the binary is sent,
 * avoiding a false-positive "OTA complete" toast when the server rejects
 * the upload mid-stream and the browser fires onerror with uploadPct=100%. */
static esp_err_t api_auth_check(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    return send_json(r, "{\"status\":\"ok\"}");
}

/* POST /api/auth/logout — invalidate the current session token. */
static esp_err_t api_auth_logout(httpd_req_t *r)
{
    /* Pull the token from the Authorization header.  No body required. */
    char hdr[81];
    if (httpd_req_get_hdr_value_str(r, "Authorization", hdr, sizeof(hdr)) == ESP_OK
        && strncmp(hdr, "Bearer ", 7) == 0) {
        auth_logout(hdr + 7);
    }
    return send_json(r, "{\"status\":\"ok\"}");
}

/* GET /api/wifi/ap_pin — returns the current 8-digit setup-AP PIN.
 * Auth-gated so an unauthenticated LAN attacker can't snarf the PIN and
 * connect to the AP (the AP itself, when active, is also gated by the same
 * PIN — but exposing it on the LAN would let someone in WiFi range bypass
 * that). */
static esp_err_t api_wifi_ap_pin(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char body[64];
    snprintf(body, sizeof(body), "{\"pin\":\"%s\"}",
             wifi_manager_get_ap_pin());
    return send_json(r, body);
}

/* POST /api/wifi/regen_pin — generate a new random PIN, persist it, and
 * apply to the live AP.  Existing AP clients are not kicked. */
static esp_err_t api_wifi_regen_pin(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    if (wifi_manager_regenerate_ap_pin() != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Storage error"), ESP_FAIL;
    char body[64];
    snprintf(body, sizeof(body), "{\"status\":\"ok\",\"pin\":\"%s\"}",
             wifi_manager_get_ap_pin());
    return send_json(r, body);
}

/* Forward declaration: defined further down (OTA section) but needed here
 * too — see the esp_wifi_restore() race below. */
static void ota_suspend_tasks(void);

/* POST /api/factory_reset_full — wipes admin password + AP PIN + user
 * config, then reboots.  Distinct from /api/reset which only clears user
 * config (and WiFi credentials).  Returns the device to fresh-from-box
 * state: the next boot generates a new AP PIN and the web UI prompts the
 * user to set a new admin password. */
static esp_err_t api_factory_reset_full(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    /* Order matters slightly:
     *   1. Send the response first (so the client gets a confirmation),
     *   2. Wipe state,
     *   3. Reboot.
     * If we wipe before responding, the network may still drop the
     * response if the WiFi reset path interrupts TCP. */
    log_restart_request(r, "factory reset (full)");
    send_json(r, "{\"status\":\"ok\",\"message\":\"Resetting and rebooting...\"}");
    vTaskDelay(pdMS_TO_TICKS(500));

    auth_factory_reset();
    wifi_manager_factory_reset_ap_pin();

    /* esp_wifi_restore() below tears down the WiFi driver, firing a STA
     * disconnect event — ha_mqtt's handler for that tries to gracefully
     * pause/disconnect the MQTT client over the very socket being torn out
     * at that exact moment, which can hang badly enough to trip the
     * interrupt watchdog before config_reset() ever runs (the reset then
     * looks like it silently "didn't take"). ota_suspend_tasks() already
     * solves this for the OTA flash paths by stopping ha_mqtt's client
     * task first; reusing it here is safe since this path always ends in
     * esp_restart(), so nothing it suspends needs to resume. */
    ota_suspend_tasks();
    esp_wifi_restore();        /* clear WiFi driver's NVS namespace */
    config_reset();            /* clear /spiffs/config.json */

    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

/* POST /api/auth/change_password — old password must match before set. */
static esp_err_t api_auth_change_password(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    cJSON *body = read_json_body(r, 256);
    if (!body) return ESP_FAIL;

    cJSON *jold = cJSON_GetObjectItem(body, "old_password");
    cJSON *jnew = cJSON_GetObjectItem(body, "new_password");
    char old_buf[80] = {0}, new_buf[80] = {0};
    if (cJSON_IsString(jold) && jold->valuestring)
        snprintf(old_buf, sizeof(old_buf), "%s", jold->valuestring);
    if (cJSON_IsString(jnew) && jnew->valuestring)
        snprintf(new_buf, sizeof(new_buf), "%s", jnew->valuestring);
    cJSON_Delete(body);

    esp_err_t err = auth_change_password(old_buf, new_buf);
    memset(old_buf, 0, sizeof(old_buf));
    memset(new_buf, 0, sizeof(new_buf));

    if (err == ESP_ERR_INVALID_STATE)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "Old password incorrect"), ESP_FAIL;
    if (err == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "New password must be 6-64 chars"), ESP_FAIL;
    if (err != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Storage error"), ESP_FAIL;
    /* Bonus: after a password change, kill all existing sessions so other
     * devices have to re-authenticate with the new password. */
    auth_clear_all_sessions();
    return send_json(r, "{\"status\":\"ok\"}");
}

/* POST /api/auth/disable — remove the admin password, disabling auth.
 * The current password must be supplied in the request body so a brief
 * session-hijack or XSS script cannot silently disable protection.
 * After success, auth_is_password_set() returns false, REQUIRE_AUTH
 * becomes a no-op, and the device is open until a new password is set. */
static esp_err_t api_auth_disable(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    cJSON *body = read_json_body(r, 256);
    if (!body) return ESP_FAIL;

    cJSON *jpw = cJSON_GetObjectItem(body, "password");
    char pw_buf[80] = {0};
    if (cJSON_IsString(jpw) && jpw->valuestring)
        snprintf(pw_buf, sizeof(pw_buf), "%s", jpw->valuestring);
    cJSON_Delete(body);

    if (!auth_verify_password(pw_buf)) {
        memset(pw_buf, 0, sizeof(pw_buf));
        httpd_resp_set_status(r, "401 Unauthorized");
        httpd_resp_set_type(r, "application/json");
        return httpd_resp_sendstr(r, "{\"error\":\"bad_password\"}");
    }
    memset(pw_buf, 0, sizeof(pw_buf));

    /* auth_factory_reset() erases admin_set / salt / hash / iter from NVS
     * and clears all in-RAM sessions.  ap_pin is in the same NVS namespace
     * but is NOT touched here — only the full factory reset clears it. */
    auth_factory_reset();
    ESP_LOGI(TAG, "Admin authentication disabled by user");
    return send_json(r, "{\"status\":\"ok\"}");
}

static esp_err_t api_reset(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    log_restart_request(r, "reset (config+wifi)");
    send_json(r, "{\"status\":\"ok\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    /* Same esp_wifi_restore()-vs-ha_mqtt race as api_factory_reset_full()
     * above — see its comment. Response is sent first here too, same
     * reasoning as that handler: the network can still drop it if the
     * WiFi-teardown race below interrupts TCP before the client reads it. */
    ota_suspend_tasks();
    /* Wipe the WiFi driver's own NVS namespace so the device cannot
     * reconnect to the old network after reboot.  Must be called while
     * the WiFi stack is running (before esp_restart). */
    esp_wifi_restore();
    config_reset();
    esp_restart();
    return ESP_OK;
}

/* POST /api/reboot — restart the device without touching the config */
static esp_err_t api_reboot(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    log_restart_request(r, "plain reboot");
    send_json(r, "{\"status\":\"ok\",\"message\":\"Rebooting...\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* GET /api/wl_scene — the live inputs the WeatherLive sky is drawn from
 * (render_weatherlive()): weather icon, cloud cover, precipitation, wind,
 * today's range and the resolved sunrise/sunset. Lets the web UI's Display
 * tab live preview render the same scene the tubes show. Auth'd (unlike
 * /api/status) since sunrise/sunset reveal the device's approximate
 * location. Weather fields are omitted until the first successful fetch. */
static esp_err_t api_wl_scene(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    int sunrise, sunset;
    bool south;
    display_wl_sun_events(&sunrise, &sunset, &south);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "sunrise", sunrise);
    cJSON_AddNumberToObject(root, "sunset", sunset);
    cJSON_AddBoolToObject(root, "south", south);
    float lat, lon;   /* no location: sunrise/sunset above are the 6:00/19:00 fallback */
    cJSON_AddBoolToObject(root, "has_loc", weather_get_location(&lat, &lon));
    const weather_data_t *w = weather_get();
    cJSON_AddBoolToObject(root, "valid", w && w->valid);
    if (w && w->valid) {
        cJSON_AddStringToObject(root, "icon", w->icon);
        cJSON_AddNumberToObject(root, "temp_c", json_round1(w->temp_c));
        cJSON_AddNumberToObject(root, "humidity", json_round1(w->humidity));
        cJSON_AddNumberToObject(root, "wind_kph", json_round1(w->wind_kph));
        cJSON_AddNumberToObject(root, "cloud_cover_pct", json_round1(w->cloud_cover_pct));
        cJSON_AddNumberToObject(root, "precip_mm", json_round1(w->precip_mm));
        cJSON_AddBoolToObject(root, "day_range_valid", w->day_range_valid);
        cJSON_AddNumberToObject(root, "day_min_c", json_round1(w->day_min_c));
        cJSON_AddNumberToObject(root, "day_max_c", json_round1(w->day_max_c));
    }
    bool aqi_eu;
    int aqi = weather_get_aqi(&aqi_eu);   /* -1 until the first AQI fetch */
    if (aqi >= 0) {
        cJSON_AddNumberToObject(root, "aqi", aqi);
        cJSON_AddBoolToObject(root, "aqi_eu", aqi_eu);
    }
    sht30_reading_t sensor;
    if (sht30_get(&sensor)) {   /* indoor H/T panel */
        cJSON_AddNumberToObject(root, "indoor_temp_c", json_round1(sensor.temp_c));
        cJSON_AddNumberToObject(root, "indoor_hum", json_round1(sensor.humidity));
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;
    esp_err_t ret = send_json(r, json);
    free(json);
    return ret;
}

static int ota_verify_wait_s(void);
static esp_err_t api_status(httpd_req_t *r)
{
    /* NOT REQUIRE_AUTH'd, deliberately — frontend bootAuth() does a plain
     * unauthenticated fetch('/api/status') on every page load specifically
     * to read admin_set and decide whether to show the login modal at all,
     * and onboardConnectWifi() polls this mid-onboarding before any admin
     * password necessarily exists. Gating this endpoint would make that
     * check unable to run before the user has a token — a chicken-and-egg
     * lockout. admin_set is intentionally the only auth-relevant field
     * here; the AP PIN itself is on the separate auth'd
     * /api/wifi/ap_pin route.
     *
     * Timing instrumentation (DEBUG level, silent unless the "Debug
     * logging" checkbox in System → Device Logs is on) — narrows down
     * which section of this handler is slow when this endpoint's latency
     * regresses. */
    int64_t t0 = esp_timer_get_time();
    cJSON *root = cJSON_CreateObject();
    struct tm t; ntp_get_local(&t);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &t);
    cJSON_AddStringToObject(root, "time", ts);
    cJSON_AddBoolToObject(root, "ntp_synced",      ntp_time_synced());
    cJSON_AddBoolToObject(root, "rtc_battery_ok",  ntp_rtc_battery_ok());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_manager_is_connected());
    cJSON_AddStringToObject(root, "ip", wifi_manager_get_ip());
    /* Runtime-only pause state (see display_set_rotation_paused) — not part
     * of the persisted config, so the web UI needs it here to reflect a
     * pause set from another open tab, a timed pause auto-resuming, or a
     * reset back to 0 on reboot. 0 = not paused, -1 = paused indefinitely,
     * N = seconds remaining until auto-resume. */
    cJSON_AddNumberToObject(root, "rotation_pause_remaining_s", display_rotation_pause_remaining_s());
    int64_t t_wifi = esp_timer_get_time();
    const weather_data_t *w = weather_get();
    if (w && w->valid) {
        cJSON *wj = cJSON_AddObjectToObject(root, "weather");
        cJSON_AddNumberToObject(wj, "temp_c", json_round1(w->temp_c));
        cJSON_AddNumberToObject(wj, "humidity", json_round1(w->humidity));
        cJSON_AddStringToObject(wj, "condition", w->condition);
    }
    {   /* Next-24-hour precipitation outlook (Open-Meteo hourly), independent of the current-conditions provider. */
        weather_outlook_t ol;   /* about 170 bytes on the httpd stack */
        if (weather_get_outlook(&ol)) {
            cJSON *oj = cJSON_AddObjectToObject(root, "precip_outlook");
            cJSON_AddBoolToObject(oj, "event", ol.event);
            cJSON_AddBoolToObject(oj, "now", ol.now);
            cJSON_AddStringToObject(oj, "type", ol.event ? (ol.snow_event ? "snow" : "rain") : "none");
            cJSON_AddNumberToObject(oj, "start_min", ol.event ? ol.start_min : -1);
            cJSON_AddNumberToObject(oj, "start_hour", ol.event ? ol.start_hour : -1);
            cJSON_AddNumberToObject(oj, "duration_h", ol.event ? ol.duration_h : 0);
            cJSON_AddNumberToObject(oj, "peak_mm", ol.event ? (double)((int)(ol.peak_mm * 10.0f + 0.5f)) / 10.0 : 0.0);
            cJSON_AddNumberToObject(oj, "chance_pct", ol.event ? ol.chance_pct : 0);
            cJSON *arr = cJSON_AddArrayToObject(oj, "mm");   /* hourly mm, from this hour on */
            for (int i = 0; i < ol.hours; i++)
                cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)((int)(ol.mm[i] * 10.0f + 0.5f)) / 10.0));
        }
    }
    int64_t t_weather = esp_timer_get_time();
    sht30_reading_t sensor;
    if (sht30_get(&sensor)) {
        cJSON *sj = cJSON_AddObjectToObject(root, "sensor");
        cJSON_AddNumberToObject(sj, "temp_c",   json_round1(sensor.temp_c));
        cJSON_AddNumberToObject(sj, "humidity", json_round1(sensor.humidity));
    }
    int64_t t_sht30 = esp_timer_get_time();
    const sub_count_t *s = subscribers_get();
    if (s && s->valid) cJSON_AddNumberToObject(root, "subscribers", s->subscriber_count);
    const sub_count_t *insta = instagram_get();
    if (insta && insta->valid) cJSON_AddNumberToObject(root, "instagram_followers", insta->subscriber_count);
    const sub_count_t *tt = tiktok_get();
    if (tt && tt->valid) cJSON_AddNumberToObject(root, "tiktok_followers", tt->subscriber_count);
    const sub_count_t *mt = mastodon_get();
    if (mt && mt->valid) cJSON_AddNumberToObject(root, "mastodon_followers", mt->subscriber_count);
    int64_t t_subs = esp_timer_get_time();
    /* Heap / PSRAM telemetry — surfaced in the System tab so a slowly leaking
     * build is visible long before allocations actually start failing.
     * heap_*    = INTERNAL SRAM specifically (~320 KB total on ESP32-WROVER).
     * psram_*   = PSRAM (capped at 4 MB by the ESP32 MMU).
     * heap_min  = lifetime low-water mark across all caps (combined total).
     * *_largest = largest free contiguous block (the real fragmentation signal).
     *
     * esp_get_free_heap_size() returns the COMBINED total across internal +
     * PSRAM and is misleading for diagnostics; we use heap_caps_get_*
     * with explicit cap masks instead. */
    cJSON_AddNumberToObject(root, "heap_free",
                            (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "heap_min",      (double)esp_get_minimum_free_heap_size());
    /* CAP_8BIT excludes reclaimed IRAM, which can't hold a real allocation
     * and would otherwise inflate this figure — see main.c's heap-log note. */
    cJSON_AddNumberToObject(root, "heap_largest",
                            (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "psram_free",
                            (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "psram_largest",
                            (double)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    int64_t t_heap = esp_timer_get_time();
    {
        /* Skip the FS-usage walk entirely while an OTA / hotpatch is running:
         * it holds the display parked via display_show_wait(), and the
         * display_pause_for_spi()/display_unpause() pair below would drop the
         * park refcount to zero and resume the display task mid-flash. Serve
         * whatever's cached (possibly stale/zero); the poll after the update
         * finishes refreshes it.
         *
         * Also skip while the boot splash is active: this cache is
         * invalidated every boot (so status always reflects reality), so
         * the FIRST /api/status poll after boot always pays this walk's
         * full cost — on real hardware, ~1.8s, entirely spent with the
         * display paused. Landing during the boot-wait window reads as the
         * splash freezing solid. Same defer-and-retry-later reasoning as
         * stock_files_check_timer_cb(); the next poll after boot-wait ends
         * (dashboard already polls every 5s) picks it up normally. */
        if (!s_fs_cache_valid && !s_ota_active && !display_boot_wait_active()) {
            /* This lazy first call fires on the first /api/status poll
             * after boot, right as display_task is doing its own SPI/LEDC
             * work — the block-walk's raw flash reads force ESP32's
             * flash-cache-disable path to park the other core with
             * interrupts masked, which can trip the watchdog if that core
             * is display_task mid critical section. Pausing display_task
             * first removes it from the race; on a timeout, skip the read
             * and let the next poll retry. */
            if (display_pause_for_spi(5000)) {
                esp_littlefs_info("littlefs", &s_fs_total_cached, &s_fs_used_cached);
                /* Re-read the FS version string in the same guarded window,
                 * so the /api/status hot path below never does its own
                 * blocking fopen(). */
                strcpy(s_fs_ver_cached, "unknown");
                FILE *vcf = fopen("/spiffs/web/version.txt", "r");
                if (vcf) {
                    if (fgets(s_fs_ver_cached, sizeof(s_fs_ver_cached), vcf))
                        s_fs_ver_cached[strcspn(s_fs_ver_cached, "\r\n")] = '\0';
                    fclose(vcf);
                }
                s_fs_cache_valid = true;
                display_unpause();
            } else {
                ESP_LOGW(TAG, "fs usage: display task did not pause — skipping this poll, will retry");
            }
        }
        cJSON_AddNumberToObject(root, "fs_total", (double)s_fs_total_cached);
        cJSON_AddNumberToObject(root, "fs_used",  (double)s_fs_used_cached);
    }
    int64_t t_fsinfo = esp_timer_get_time();
    cJSON_AddStringToObject(root, "firmware", FW_VERSION_STR);
    /* expected_fs: the LittleFS version this firmware binary was built against.
     * Baked in at compile time from version.json → fs_version.
     * fs_version: the version actually present on the device, from
     * /spiffs/web/version.txt (written when littlefs.bin was flashed);
     * cached in s_fs_ver_cached, re-read only on fs_usage_invalidate().
     * The UI shows a mismatch banner when these two differ — i.e. the LittleFS
     * image on the device is not the one this firmware expects. */
    cJSON_AddStringToObject(root, "expected_fs", FS_VERSION_STR);
    /* The Web UI pull's state, readable without a login: the online updater's page cannot always read /api/webui_pull_status
     * after the OTA reboot (sessions are gone and the no-password window may not be open), but it can always read this. */
    cJSON_AddStringToObject(root, "webui_state", webui_state_name());
    if (webui_error_text()[0]) cJSON_AddStringToObject(root, "webui_error", webui_error_text());
    { const int vw = ota_verify_wait_s(); if (vw > 0) cJSON_AddNumberToObject(root, "ota_verify_s", vw); }   /* firmware still being verified: flashing waits */
    /* Theme JPEG decode error — non-null when the active theme has an image
     * that couldn't be decoded (wrong size, truncated file, etc.).
     * Cleared automatically when the theme changes. */
    const char *te = display_get_theme_error();
    if (te) cJSON_AddStringToObject(root, "theme_error", te);
    else    cJSON_AddNullToObject  (root, "theme_error");
    cJSON_AddStringToObject(root, "fs_version", s_fs_ver_cached);
    /* stock_missing: count from the last stock_files_check() run (boot, or
     * most recent hotpatch/webui-pull completion) — cheap read of an
     * already-computed value, never recomputed here. See that function's
     * doc comment for why. */
    cJSON_AddNumberToObject(root, "stock_missing", s_stock_missing_count);
    if (s_stock_missing_count > 0)
        cJSON_AddStringToObject(root, "stock_missing_example", s_stock_missing_example);
    int64_t t_fsver = esp_timer_get_time();
    app_mode_t status_mode;
    bool       status_mic_cal;
    config_lock();
    const nextube_config_t *scfg = config_get();
    status_mode    = scfg->current_mode;
    status_mic_cal = scfg->mic_calibration_saved && scfg->mic_floor_top_hz == (uint16_t)(scfg->mic_top_hz | MIC_FLOOR_CHAIN_FLAG);
    config_unlock();
    int64_t t_config = esp_timer_get_time();
    cJSON_AddStringToObject(root, "mode", app_mode_name(status_mode));
    cJSON_AddBoolToObject(root, "mic_calibration_saved", status_mic_cal);
    {   /* anti burn-in session: drives the Display tab's running note + Cancel */
        uint32_t bi_left;
        bool bi_on = display_burnin_status(&bi_left);
        bool tp_on = bi_on && display_test_pattern_active();
        cJSON_AddBoolToObject(root, "burnin_active", bi_on && !tp_on);
        if (bi_on && !tp_on) cJSON_AddNumberToObject(root, "burnin_remaining_s", bi_left);
        cJSON_AddBoolToObject(root, "testpat_active", tp_on);
        if (tp_on) cJSON_AddNumberToObject(root, "testpat_remaining_s", bi_left);
    }
    /* —— auth state.  The web UI gates its first-boot setup flow on these.
     * admin_set is the only auth-related field exposed unauthenticated; the
     * AP PIN itself is on a separate auth'd route (/api/wifi/ap_pin). */
    cJSON_AddBoolToObject(root, "admin_set", auth_is_password_set());
    /* OTA rollback detection — check whether the inactive OTA slot is in
     * ABORTED state.  This means the last OTA update crashed before the new
     * firmware could call esp_ota_mark_app_valid_cancel_rollback(), and the
     * bootloader automatically reverted to this (previously-valid) slot.
     * Reading partition state is a fast metadata read; safe on every poll. */
    {
        const esp_partition_t *running  = esp_ota_get_running_partition();
        const esp_partition_t *inactive = esp_ota_get_next_update_partition(NULL);
        bool rollback = false;
        if (inactive && inactive != running) {
            esp_ota_img_states_t st;
            if (esp_ota_get_state_partition(inactive, &st) == ESP_OK &&
                    (st == ESP_OTA_IMG_ABORTED || st == ESP_OTA_IMG_INVALID)) {   /* aborted: bootloader rollback; invalid: probation rollback */
                rollback = true;
                esp_app_desc_t desc;
                if (esp_ota_get_partition_description(inactive, &desc) == ESP_OK)
                    cJSON_AddStringToObject(root, "ota_rollback_ver", desc.version);
            }
        }
        cJSON_AddBoolToObject(root, "ota_rollback", rollback);
    }
    /* Flash write-health — see config_mgr_note_flash_write_failure(). Should
     * stay 0 for the life of a healthy device; hidden-debug-panel only. */
    cJSON_AddNumberToObject(root, "flash_write_fail_count",
                            (double)config_mgr_get_flash_write_fail_count());
    int64_t t_ota = esp_timer_get_time();
    char *json = cJSON_PrintUnformatted(root);
    int64_t t_json = esp_timer_get_time();
    esp_err_t ret = send_json(r, json);
    int64_t t_send = esp_timer_get_time();
    ESP_LOGD(TAG, "api_status timing (ms): wifi=%lld weather=%lld sht30=%lld subs=%lld "
             "heap=%lld fsinfo=%lld fsver=%lld config=%lld ota=%lld json=%lld send=%lld total=%lld",
             (long long)((t_wifi   - t0)      / 1000),
             (long long)((t_weather- t_wifi)  / 1000),
             (long long)((t_sht30  - t_weather)/1000),
             (long long)((t_subs   - t_sht30) / 1000),
             (long long)((t_heap   - t_subs)  / 1000),
             (long long)((t_fsinfo - t_heap)  / 1000),
             (long long)((t_fsver  - t_fsinfo)/1000),
             (long long)((t_config - t_fsver) / 1000),
             (long long)((t_ota    - t_config)/1000),
             (long long)((t_json   - t_ota)   / 1000),
             (long long)((t_send   - t_json)  / 1000),
             (long long)((t_send   - t0)      / 1000));
    free(json); cJSON_Delete(root);
    return ret;
}

/* GET /api/network_info — WiFi diagnostics: disconnect/reconnect log +
 * link-level details.  Split out from /api/status (which is polled
 * frequently by the dashboard) since this data changes rarely; the web UI
 * fetches it only when the Network Info panel is expanded.  Auth-gated: MAC,
 * BSSID, RSSI, netmask, gateway and DNS servers are LAN-topology info an
 * unauthenticated device on the network shouldn't be able to snarf — same
 * reasoning as /api/wifi/ap_pin (see that handler's comment).  Only called
 * post-login via secureFetch(), so gating it doesn't affect any pre-auth
 * flow. */
static esp_err_t api_network_info(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "disconnect_count", wifi_manager_get_disconnect_count());
    cJSON_AddNumberToObject(root, "last_disconnect_reason", wifi_manager_get_last_disconnect_reason());
    /* Duration, not a timestamp: connected_since_us is esp_timer_get_time()
     * (monotonic, boot-relative), not wall-clock epoch time, so it can't be
     * diffed against the browser's Date.now() — compute the elapsed seconds
     * here instead, on the device's own clock. -1 = not connected. */
    {
        int64_t since = wifi_manager_get_connected_since_us();
        double conn_secs = since ? (double)(esp_timer_get_time() - since) / 1e6 : -1;
        cJSON_AddNumberToObject(root, "connected_duration_s", conn_secs);
    }
    wifi_manager_net_info_t ni;
    if (wifi_manager_get_net_info(&ni)) {
        cJSON_AddStringToObject(root, "mac",     ni.mac);
        cJSON_AddStringToObject(root, "bssid",   ni.bssid);
        cJSON_AddNumberToObject(root, "channel", ni.channel);
        cJSON_AddNumberToObject(root, "rssi",    ni.rssi);
        cJSON_AddStringToObject(root, "netmask", ni.netmask);
        cJSON_AddStringToObject(root, "gateway", ni.gateway);
        cJSON_AddStringToObject(root, "dns1",    ni.dns1);
        cJSON_AddStringToObject(root, "dns2",    ni.dns2);
        cJSON_AddBoolToObject(root,   "phy_11n", ni.phy_11n);
    }
    char *json = cJSON_PrintUnformatted(root);
    esp_err_t ret = send_json(r, json);
    free(json); cJSON_Delete(root);
    return ret;
}

/* ── OTA task suspension ─────────────────────────────────────────────────────
 * Suspend every non-essential background task before any flash write to
 * reduce CPU/bus contention during esp_ota_write()'s erase+program cycles —
 * each erase disables the ESP32 data cache, and concurrent SPI DMA/I2S/I2C/
 * HTTPS traffic competing for CPU in that window can stretch OTA transfers
 * into a recv timeout. Resolved by FreeRTOS task name, not handle, so no
 * component needs to expose one. Both OTA paths end in esp_restart(), so
 * there's no matching resume call there; webui_resume_tasks() (used by the
 * webui-pull path, which doesn't restart on success) does the resume from
 * this same list. The display task is handled separately by
 * display_show_wait(). */
static const char *const k_paused_task_names[] = {
    "net_poll",   /* HTTPS polling (weather + subscribers + update_check,
                   * merged onto one shared task) — competes with OTA TCP
                   * stream + heap. */
    "ntp",        /* SNTP/UDP      — adds lwIP load during flash writes  */
    "sht30",      /* I2C sensor    — periodic wakeups, CPU cycles        */
    "leds",       /* RMT DMA       — Core 1 bus traffic during writes    */
    "mic",        /* I2S/ADC DMA   — continuous DMA, CPU interrupts      */
    "audio_play", /* DAC/I2S       — ephemeral, only present if playing  */
    "ha_mqtt",    /* MQTT connect/publish — our wrapper task only; the
                   * separate esp-mqtt internal client task keeps running
                   * regardless (reached instead by ha_mqtt_pause()/
                   * ha_mqtt_resume(), called explicitly below). */
    NULL,
};

static void ota_suspend_tasks(void)
{
    /* ── Spectrum-mode flash guard ──────────────────────────────────────
     * In Spectrum mode the mic runs hardware capture (adc_continuous on
     * I2S0 DMA, 8 kHz stream) plus a 20 Hz analysis/render pipeline —
     * continuous bus + interrupt traffic that is exactly the concurrency
     * this PCB tolerates worst during flash erase/write windows (see the
     * mic/SPIFFS IWDT note in main.c).  Suspending the mic task below is
     * NOT enough: vTaskSuspend freezes the task mid-whatever while the DMA
     * and I2S peripheral keep running.
     *
     * Instead, force the device to Clock mode (RAM-only change) and let
     * mic_task shut its own capture down through the mode gate — it polls
     * the mode every ≤100 ms and tears down the adc_continuous handle
     * cleanly (acq_stop).  After that, suspending the task is safe. */
    config_lock();
    bool in_spectrum = (config_get()->current_mode == APP_MODE_SPECTRUM);
    config_unlock();
    if (in_spectrum) {
        ESP_LOGI(TAG, "OTA guard: Spectrum active — switching to Clock before flash");
        config_set_mode(APP_MODE_CLOCK);
        /* Gate poll (≤100 ms) + capture teardown + margin.  The display
         * switches on its next tick; display_show_wait() repaints it with
         * the wait screen moments later anyway. */
        vTaskDelay(pdMS_TO_TICKS(400));
    }

    for (int i = 0; k_paused_task_names[i]; i++) {
        TaskHandle_t h = xTaskGetHandle(k_paused_task_names[i]);
        if (h && h != xTaskGetCurrentTaskHandle()) {   /* a job running on net_poll must not suspend itself */
            vTaskSuspend(h);
            ESP_LOGI(TAG, "OTA: suspended '%s'", k_paused_task_names[i]);
        }
    }
    /* Stops esp-mqtt's own internal client task too, not just the "ha_mqtt"
     * wrapper task suspended above — see ha_mqtt_pause()'s comment. Called
     * after the loop so the wrapper task (which could otherwise race a
     * publish against the client mid-stop) is already frozen first. */
    ha_mqtt_pause();
    /* Brief yield so any task currently executing its current time-slice
     * can finish its current instruction and reach a safe stack state before
     * the flash cache is disabled by the first esp_ota_write(). */
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void webui_resume_tasks(void)
{
    for (int i = 0; k_paused_task_names[i]; i++) {
        TaskHandle_t h = xTaskGetHandle(k_paused_task_names[i]);
        if (h) {
            vTaskResume(h);
            ESP_LOGI(TAG, "[webui] resumed '%s'", k_paused_task_names[i]);
        }
    }
    ha_mqtt_resume();
}

/* ── OTA double-flash guard + clean deferred reboot ────────────────────────
 * Two protections against an OTA being applied twice (a backgrounded
 * browser tab retrying mid-flash):
 *  1. s_ota_active — rejects a second update request (HTTP 409) while one
 *     is already running; cleared only if the flash fails, since a
 *     successful flash reboots anyway.
 *  2. Deferred reboot — esp_restart() inside the handler would RST the TCP
 *     socket before a slow/backgrounded browser reads the 200, causing it
 *     to retry the POST. Instead: send the response with "Connection:
 *     close", return so httpd closes the socket cleanly, then fire
 *     esp_restart() from a one-shot timer ~1.5s later (the esp_timer task
 *     isn't suspended by ota_suspend_tasks(), so this still runs).
 * s_ota_active itself is declared near the top of this file (api_status()
 * needs to read it). */
/* Hidden debug panel's "Per-task stack log" toggle — see api_debug_stacklog()
 * and web_server_debug_stacklog_enabled() below. Runtime only, off by default;
 * resets on reboot like every other debug-panel control. */
static volatile bool s_debug_stacklog = false;
/* Set by ota_pull_task in NVS before reboot; consumed on the first api_webui_pull
 * call after that reboot.  Allows the browser to complete the webui half of an
 * online-updater without a valid session (sessions are RAM-only and are cleared by
 * the OTA reboot).  Expires after POST_OTA_AUTH_TTL_US so a device that stays up
 * indefinitely without completing Phase 4 does not leave auth bypassed forever. */
static bool          s_post_ota_auth        = false;
static int64_t       s_post_ota_auth_set_us = 0;
#define POST_OTA_AUTH_TTL_US  (5LL * 60 * 1000000LL)   /* 5 minutes */
/* true when this boot has the NVS post_ota flag set — cleared when the flag
 * is consumed by api_webui_pull_auto.  While set, api_ota_pull_status returns
 * 503 so the old Phase-2 poll loop detects the reboot via !rs.ok and
 * advances to Phase 3/4 even when no admin password is configured. */
static bool          s_post_ota_boot_pending = false;

/* True only while the post-OTA bypass is set AND has not expired. */
static bool post_ota_auth_valid(void)
{
    if (!s_post_ota_auth) return false;
    if (esp_timer_get_time() - s_post_ota_auth_set_us > POST_OTA_AUTH_TTL_US) {
        ESP_LOGW(TAG, "post_ota auth bypass expired (>5 min) — re-auth required");
        s_post_ota_auth = false;
        return false;
    }
    return true;
}

/* Returns true (and clears the NVS flag) if a post-OTA bypass is pending.
 * Result is cached in s_post_ota_auth so only one NVS read is needed. */
static bool consume_post_ota_flag(void)
{
    nvs_handle_t h;
    if (nvs_open("nextube_sec", NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t flag = 0;
    bool had = (nvs_get_u8(h, "post_ota", &flag) == ESP_OK && flag);
    if (had) {
        nvs_set_u8(h, "post_ota", 0);
        nvs_commit(h);
        s_post_ota_boot_pending = false;
        s_post_ota_auth_set_us  = esp_timer_get_time();
    }
    nvs_close(h);
    return had;
}

static void ota_reboot_timer_cb(void *arg) { esp_restart(); }

static esp_err_t ota_finish_and_reboot(httpd_req_t *r, const char *json_msg)
{
    httpd_resp_set_hdr(r, "Connection", "close");
    send_json(r, json_msg);
    static esp_timer_handle_t t = NULL;
    if (!t) {
        const esp_timer_create_args_t a = {
            .callback = ota_reboot_timer_cb, .name = "ota_reboot",
        };
        if (esp_timer_create(&a, &t) != ESP_OK) esp_restart();  /* fallback */
    }
    esp_timer_start_once(t, 1500 * 1000);   /* 1.5 s in µs */
    return ESP_OK;
}

/* Guard wrapper — auth, reject concurrent OTA, then run the flash.  s_ota_active
 * is cleared only on failure; a success path defers a reboot (stays locked). */
/* A freshly flashed image stays PENDING_VERIFY until it has been up for OTA_VALID_AFTER_S (the OTA health gate in main.c), and
 * esp_ota_begin() refuses to write while the running image is unconfirmed. That is deliberate (a bad build must still be able to roll
 * back), so the user is told why and how long, instead of "OTA begin fail". Returns the seconds left, 0 when flashing is allowed. */
static int ota_verify_wait_s(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return 0;
    const int left = OTA_VALID_AFTER_S - (int)(esp_timer_get_time() / 1000000);
    return left > 0 ? left : 1;
}

/* Text for a refused esp_ota_begin(): the wait (when that is the reason) in plain words, else a generic failure. */
static void ota_begin_fail_text(char *buf, size_t n)
{
    const int left = ota_verify_wait_s();
    if (left <= 0) { snprintf(buf, n, "Could not start the firmware write - try again, or restart the clock first."); return; }
    const unsigned mins = ((unsigned)left + 59u) / 60u % 100u;
    snprintf(buf, n, "The clock is still checking the update it just installed, and cannot take another yet. Try again in about %u minute%s, or restart the clock and flash again.",
             mins, mins == 1u ? "" : "s");
}

static esp_err_t api_ota_impl(httpd_req_t *r);
static esp_err_t api_ota(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    if (s_ota_active) {
        httpd_resp_set_status(r, "409 Conflict");
        httpd_resp_set_type(r, "application/json");
        httpd_resp_sendstr(r, "{\"error\":\"ota_in_progress\"}");
        return ESP_OK;   /* complete response already sent */
    }
    s_ota_active = true;
    esp_err_t e = api_ota_impl(r);
    if (e != ESP_OK) s_ota_active = false;   /* failed → unlock; success → reboot pending */
    return e;
}

static esp_err_t api_ota_impl(httpd_req_t *r)
{
    if (r->content_len <= 0)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Content-Length required"), ESP_FAIL;

    /* Extend the socket recv timeout for the duration of this OTA upload.
     *
     * The default httpd recv timeout (CONFIG_HTTPD_RECV_WAIT_TIMEOUT = 5 s) is
     * too short for large firmware binaries.  Each esp_ota_write() call erases
     * and programs a 4 KB flash sector (~25 ms total), which briefly disables
     * the ESP32 data cache.  Cache-disable windows can cause the WiFi driver to
     * drop packets, requiring TCP retransmission.  With multiple drops in a row,
     * the cumulative retransmission delay can exceed 5 s and cause httpd_req_recv
     * to time out mid-transfer, aborting the OTA.
     *
     * 60 s gives ample headroom for a 1.5 MB upload over a congested WiFi link
     * without requiring changes to the global CONFIG_HTTPD_RECV_WAIT_TIMEOUT. */
    {
        int sock = httpd_req_to_sockfd(r);
        if (sock >= 0) {
            struct timeval tv = { .tv_sec = 60, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }
    }

    /* Show wait screen and suspend the display task before touching flash.
     * esp_ota_write() erases 4 KB sectors while briefly disabling the data
     * cache; a concurrent SPI JPEG load from the display task would race for
     * the same bus and risk a cache-disable fault. */
    display_show_wait();

    /* Suspend all other background tasks to eliminate CPU/bus contention
     * during sector erase+program cycles (see ota_suspend_tasks comment). */
    ota_suspend_tasks();

    const esp_partition_t *upd = esp_ota_get_next_update_partition(NULL);
    if (!upd) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition"), ESP_FAIL;

    int img_len = r->content_len;
    if (img_len <= 0 || (uint32_t)img_len > upd->size)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad content length"), ESP_FAIL;

    /* ── Two-phase PSRAM-buffered OTA ───────────────────────────────────────
     * Phase 1 (network):  Receive the entire firmware image into PSRAM before
     *   touching flash.  The WiFi driver ACKs packets without interference from
     *   flash-erase cache-disable windows (~25 ms/sector); the TCP window stays
     *   open and errno 11 (EAGAIN) cannot fire mid-transfer.
     * Phase 2 (flash):    Write from PSRAM via esp_ota_write().  The network is
     *   idle so cache-disable windows cause no TCP stalls at all.
     * Falls back to the original 4 KB interleaved loop when PSRAM cannot
     * satisfy the allocation (image larger than free PSRAM). */
    uint8_t *img_buf = (uint8_t *)heap_caps_malloc((size_t)img_len, MALLOC_CAP_SPIRAM);

    if (img_buf) {
        ESP_LOGI(TAG, "OTA: receiving %d B into PSRAM buffer…", img_len);

        /* Phase 1 — receive the full image into PSRAM */
        {
            int rem = img_len;
            uint8_t *p = img_buf;
            while (rem > 0) {
                int n = httpd_req_recv(r, (char *)p, rem);
                if (n <= 0) { heap_caps_free(img_buf); return ESP_FAIL; }
                p += n; rem -= n;
            }
        }

        /* Validate magic byte: valid ESP32 app images start with 0xE9.
         * The merged full-flash binary starts with the bootloader (not 0xE9). */
        if (img_buf[0] != 0xE9) {
            heap_caps_free(img_buf);
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                "Wrong file: upload nextube-fw-ota.bin, not nextube-fw-full.bin"), ESP_FAIL;
        }

        ESP_LOGI(TAG, "OTA: receive done, flashing %d B from PSRAM…", img_len);

        /* Phase 2 — flash from PSRAM; network is idle, no TCP contention */
        esp_ota_handle_t h;
        if (esp_ota_begin(upd, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
            heap_caps_free(img_buf);
            char why[192];
            ota_begin_fail_text(why, sizeof(why));
            return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, why), ESP_FAIL;
        }
        {
            int rem = img_len;
            const uint8_t *p = img_buf;
            while (rem > 0) {
                int chunk = (rem > 4096) ? 4096 : rem;
                if (esp_ota_write(h, p, chunk) != ESP_OK) {
                    heap_caps_free(img_buf); esp_ota_abort(h); return ESP_FAIL;
                }
                p += chunk; rem -= chunk;
                vTaskDelay(1);   /* yield between writes — flash HAL disables IRQs,
                                  * starves IDLE (see ota_pull_task's identical fix) */
            }
        }
        heap_caps_free(img_buf);

        if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(upd) != ESP_OK)
            return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA finalize fail"), ESP_FAIL;

    } else {
        /* Fallback: interleaved 4 KB receive+write (PSRAM unavailable).
         * Higher EAGAIN risk — relies on task suspension + 60 s socket timeout. */
        ESP_LOGW(TAG, "OTA: PSRAM unavailable for %d B — using 4 KB chunk loop", img_len);

        esp_ota_handle_t h;
        if (esp_ota_begin(upd, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
            char why[192];
            ota_begin_fail_text(why, sizeof(why));
            return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, why), ESP_FAIL;
        }

        char *buf = malloc(4096);
        if (!buf) { esp_ota_abort(h); return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL; }

        int rem = img_len;
        bool first_chunk = true;
        while (rem > 0) {
            int n = httpd_req_recv(r, buf, rem > 4096 ? 4096 : rem);
            if (n <= 0) { free(buf); esp_ota_abort(h); return ESP_FAIL; }
            if (first_chunk) {
                first_chunk = false;
                if ((uint8_t)buf[0] != 0xE9) {
                    free(buf); esp_ota_abort(h);
                    return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                        "Wrong file: upload nextube-fw-ota.bin, not nextube-fw-full.bin"), ESP_FAIL;
                }
            }
            if (esp_ota_write(h, buf, n) != ESP_OK) { free(buf); esp_ota_abort(h); return ESP_FAIL; }
            rem -= n;
            vTaskDelay(1);   /* yield between writes — flash HAL disables IRQs,
                              * starves IDLE (see ota_pull_task's identical fix) */
        }
        free(buf);

        if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(upd) != ESP_OK)
            return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA finalize fail"), ESP_FAIL;
    }

    return ota_finish_and_reboot(r, "{\"status\":\"ok\",\"message\":\"Rebooting...\"}");
}

/* ── LittleFS (web UI) OTA ─────────────────────────────────────────── */
/* Receives a littlefs.bin image and writes it to the LittleFS partition in
 * 4 KB sectors.  Each sector is erased immediately before it is written
 * so the erase latency is interleaved with the network receive rather
 * than blocking the connection upfront.
 *
 * LittleFS is unmounted before the first write and the device reboots
 * after a successful flash.  If the upload is interrupted the partition
 * is left partially erased; a retry will always fix this since erasing
 * before writing is idempotent.
 *
 * The old /api/update_spiffs URL is kept as a backward-compatible alias
 * (see route table) so existing scripts and OTA tools continue to work. */
#define FS_SECTOR 4096
/* Guard wrapper — see api_ota above for the s_ota_active / deferred-reboot rationale. */
static esp_err_t api_fs_ota_impl(httpd_req_t *r);
static esp_err_t api_fs_ota(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    if (s_ota_active) {
        httpd_resp_set_status(r, "409 Conflict");
        httpd_resp_set_type(r, "application/json");
        httpd_resp_sendstr(r, "{\"error\":\"ota_in_progress\"}");
        return ESP_OK;   /* complete response already sent */
    }
    s_ota_active = true;
    esp_err_t e = api_fs_ota_impl(r);
    if (e != ESP_OK) s_ota_active = false;
    return e;
}

static esp_err_t api_fs_ota_impl(httpd_req_t *r)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "littlefs");
    if (!part)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "LittleFS partition not found"), ESP_FAIL;

    int content_len = r->content_len;
    if (content_len <= 0 || (uint32_t)content_len > part->size)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "Bad content length"), ESP_FAIL;

    /* Extend socket recv timeout — LittleFS images are large (4–7 MB); the
     * default 5-second per-recv timeout fires during flash-erase windows. */
    {
        int sock = httpd_req_to_sockfd(r);
        if (sock >= 0) {
            struct timeval tv = { .tv_sec = 120, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }
    }

    /* ── Two-phase PSRAM-buffered LittleFS OTA ──────────────────────────────
     * Phase 1 (network):  Receive entire image into PSRAM while LittleFS is
     *   still mounted (display_show_wait can still load wait.jpg) and TCP ACKs
     *   flow freely — no flash writes during receive means no cache-disable
     *   stalls and no EAGAIN timeouts.
     * Phase 2 (flash):    Unmount LittleFS, then erase+write from PSRAM.
     *   Network is idle — cache-disable windows cause no TCP stalls.
     * Allocation is rounded up to FS_SECTOR so the last write is always clean.
     * Falls back to original 4 KB sector loop when image is too large for PSRAM. */
    size_t alloc_len = (size_t)(((content_len + FS_SECTOR - 1) / FS_SECTOR) * FS_SECTOR);
    uint8_t *img_buf = (uint8_t *)heap_caps_malloc(alloc_len, MALLOC_CAP_SPIRAM);

    if (img_buf) {
        /* Pad the tail with 0xFF (erased-flash value) so the last sector write
         * always sees clean data even when content_len is not sector-aligned. */
        memset(img_buf + content_len, 0xFF, alloc_len - (size_t)content_len);

        ESP_LOGI(TAG, "FS OTA: receiving %d B into PSRAM…", content_len);

        /* Show wait screen BEFORE Phase 1: gives user immediate feedback AND
         * must happen before unmounting so wait.jpg is still on LittleFS.   */
        display_show_wait();
        ota_suspend_tasks();

        /* Phase 1 — receive (LittleFS still mounted, flash untouched) */
        {
            int rem = content_len;
            uint8_t *p = img_buf;
            while (rem > 0) {
                int n = httpd_req_recv(r, (char *)p, rem);
                if (n <= 0) { heap_caps_free(img_buf); return ESP_FAIL; }
                p += n; rem -= n;
            }
        }
        ESP_LOGI(TAG, "FS OTA: receive done, unmounting + flashing %zu B…", alloc_len);

        config_backup_to_nvs();
        /* Unmount LittleFS — receive is complete, safe to touch the partition. */
        esp_vfs_littlefs_unregister("littlefs");

        /* Phase 2 — erase + write from PSRAM (network idle, no TCP risk) */
        bool flash_ok = true;
        for (int off = 0; off < (int)alloc_len; off += FS_SECTOR) {
            if (esp_partition_erase_range(part, (uint32_t)off, FS_SECTOR) != ESP_OK ||
                esp_partition_write(part, (uint32_t)off, img_buf + off, FS_SECTOR) != ESP_OK) {
                config_mgr_note_flash_write_failure();
                flash_ok = false;
                break;
            }
            vTaskDelay(1);   /* yield between sectors — flash HAL disables IRQs, starves
                              * IDLE; a LittleFS image can run ~7 MB (vs. ~1.7 MB for
                              * firmware), so this loop is even more exposed to the
                              * watchdog risk fixed in ota_pull_task's write loop. */
        }
        heap_caps_free(img_buf);

        if (!flash_ok) {
            /* Attempt remount so the web server can still serve error pages. */
            esp_vfs_littlefs_conf_t _c = { .base_path = "/spiffs",
                .partition_label = "littlefs", .dont_mount = false,
                .grow_on_mount = false };
            esp_vfs_littlefs_register(&_c);
            return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Flash write failed"), ESP_FAIL;
        }
        ESP_LOGI(TAG, "FS OTA: %d bytes written (PSRAM path)", content_len);

    } else {
        /* Fallback: original interleaved 4 KB sector loop.
         * Used when the image is too large to buffer in PSRAM (>3.5 MB free).
         * Higher EAGAIN risk — relies on task suspension + 120 s socket timeout. */
        ESP_LOGW(TAG, "FS OTA: PSRAM unavailable for %zu B — 4 KB sector loop", alloc_len);

        char *buf = malloc(FS_SECTOR);
        if (!buf)
            return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Out of memory"), ESP_FAIL;

        /* Show wait screen and suspend tasks BEFORE unmounting LittleFS. */
        display_show_wait();
        ota_suspend_tasks();
        config_backup_to_nvs();
        esp_vfs_littlefs_unregister("littlefs");

#define FS_OTA_FAIL(msg) do { \
    free(buf); \
    esp_vfs_littlefs_conf_t _c = { \
        .base_path = "/spiffs", .partition_label = "littlefs", \
        .dont_mount = false, .grow_on_mount = false }; \
    esp_vfs_littlefs_register(&_c); \
    return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, (msg)), ESP_FAIL; \
} while(0)

        int written = 0;
        while (written < content_len) {
            int to_recv = content_len - written;
            if (to_recv > FS_SECTOR) to_recv = FS_SECTOR;
            memset(buf, 0xFF, FS_SECTOR);

            int rx = 0;
            while (rx < to_recv) {
                int n = httpd_req_recv(r, buf + rx, to_recv - rx);
                if (n <= 0) { FS_OTA_FAIL("Receive failed"); }
                rx += n;
            }

            if (esp_partition_erase_range(part, written, FS_SECTOR) != ESP_OK)
                { config_mgr_note_flash_write_failure(); FS_OTA_FAIL("Erase failed"); }
            if (esp_partition_write(part, written, buf, FS_SECTOR) != ESP_OK)
                { config_mgr_note_flash_write_failure(); FS_OTA_FAIL("Write failed"); }
            written += rx;
            vTaskDelay(1);   /* yield between sectors — see the PSRAM-path loop above */
        }
        free(buf);
#undef FS_OTA_FAIL
        ESP_LOGI(TAG, "FS OTA: %d bytes written (sector loop)", written);
    }

    return ota_finish_and_reboot(r, "{\"status\":\"ok\",\"message\":\"LittleFS updated, rebooting...\"}");
}

/* ── Hot Patch (ZIP → VFS) ─────────────────────────────────────────────────
 *
 * Receives a STORE-compressed ZIP (generated by CI with `zip -0`) and writes
 * each entry directly to the mounted LittleFS via standard fopen/fwrite.
 *
 * Advantages over full LittleFS OTA:
 *   • Filesystem stays mounted — no reboot required
 *   • config.json and user-uploaded files (album/, audio/) are untouched
 *   • Applies instantly — the updated index.html is live on the next page load
 *
 * Only STORE (method=0) entries are supported; DEFLATE entries are skipped with
 * a warning.  The CI step generates uncompressed ZIPs so this is never an issue
 * in practice.  config.json is explicitly protected and cannot be overwritten.
 *
 * ZIP local-file-header layout (all little-endian):
 *   [4]  signature 0x04034B50
 *   [2]  version needed  [2] flags  [2] method (0=STORE)
 *   [2]  mod_time  [2] mod_date  [4] crc32
 *   [4]  compressed_size  [4] uncompressed_size
 *   [2]  fname_len  [2] extra_len
 *   [N]  filename   [M] extra field   [C] file data
 * Stop at central-directory (0x02014B50) or EOCD (0x06054B50) signatures.    */

#define HP_MAX_ZIP   (4 * 1024 * 1024)   /* 4 MB guard — largest sane hotpatch */
#define ZIP_LFH_SIG  0x04034B50u
#define ZIP_CDH_SIG  0x02014B50u
#define ZIP_EOCD_SIG 0x06054B50u

typedef struct __attribute__((packed)) {
    uint32_t sig;
    uint16_t ver_need, flags, method, mod_time, mod_date;
    uint32_t crc32, comp_sz, uncomp_sz;
    uint16_t fname_len, extra_len;
} zip_lfh_t;

/* Create all parent directories for a /spiffs/a/b/c path. */
static void hp_mkdir_p(const char *path)
{
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *s = tmp + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            mkdir(tmp, 0755);
            *s = '/';
        }
    }
}

/* ── Hotpatch write-skip (CRC32) ──────────────────────────────────────────
 * Every ZIP local file header already carries a CRC32 of its content
 * (h->crc32, parsed but previously unused) — the exact same standard
 * reflected CRC-32 (poly 0xEDB88320, same as zlib/PKZIP) that produced the
 * checksum in the first place, so it's already the version stamp for that
 * entry's content. Computing it by hand here (rather than via a ROM helper)
 * sidesteps any question of a ROM function's seed/xor convention — this is
 * provably the exact algorithm the ZIP format itself uses, so a match here
 * is a guarantee of byte-identical content, not a probabilistic shortcut.
 *
 * hp_file_unchanged() checks size first (a plain stat(), no I/O) and only
 * reads+CRCs the existing file when sizes already match, so a genuinely
 * different file costs one stat() call, not a wasted read. Every file this
 * mechanism handles is small (largest today ~90 KB), so the unoptimized
 * byte-at-a-time loop costs well under a millisecond even on a full read.
 *
 * Point of this: hotpatch_extras.txt's stable assets (fonts, icons) ship
 * again, unchanged, in every future hotpatch — without this, that's a
 * pointless flash write (and the WDT-avoidance yield that goes with it) on
 * every single apply for content that was never going to differ. */
static uint32_t hp_crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    while (len--) {
        crc ^= *buf++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc;
}

/* Read+CRC latency here is dominated by LittleFS's own per-directory
 * metadata-traversal cost, not the byte-copying/CRC math itself: the
 * first file touched in a not-yet-open directory pays a roughly-flat
 * ~70-500ms penalty regardless of file size, while later files in the
 * same still-open directory are far cheaper. See extract_zip_to_littlefs's
 * caller for the aggregate "extraction took N ms" figure. */
static bool hp_file_unchanged(const char *vpath, uint32_t want_crc, uint32_t want_size)
{
    struct stat st;
    if (stat(vpath, &st) != 0 || (uint32_t)st.st_size != want_size)
        return false;   /* doesn't exist, or a different size — definitely changed */

    FILE *f = fopen(vpath, "rb");
    if (!f) return false;
    uint8_t buf[512];
    uint32_t crc = 0xFFFFFFFFu;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        crc = hp_crc32_update(crc, buf, n);
    fclose(f);
    return (~crc) == want_crc;
}

/* ── Cached gzipped app shell (index.html.gz) in PSRAM ──────────────────────
 * The shell is the largest, most-requested asset; reading it from LittleFS on
 * every request is the dominant httpd CPU cost.  Load it once into a PSRAM blob
 * (shell_cache_load(), lazily in serve_static) and serve it from memory with a
 * fixed Content-Length — no chunked encoding, no per-chunk yield, no repeat
 * flash reads.  shell_cache_flush() drops the blob so the next request re-reads
 * the file; it is called by hp_drop_stale_index() after every WebUI patch /
 * pull so a freshly-installed shell is picked up without a reboot. */
static uint8_t      *s_shell_buf   = NULL;
static size_t        s_shell_len   = 0;
static bool          s_shell_gz    = false;
/* Set from any task (e.g. webui_pull_task) to invalidate the cache.  The actual
 * free()+reload is DEFERRED to the next serve_static() call so it always happens
 * on the httpd task — never freeing a buffer another task might be sending. */
static volatile bool s_shell_stale = false;

static void shell_cache_flush(void) { s_shell_stale = true; }

/* After a WebUI patch, drop a stale uncompressed index.html if the new
 * gzip-only shell (index.html.gz) is now present.  serve_static prefers the
 * .gz, so a leftover plain index.html from a pre-gzip build is never served —
 * it just wastes ~308 KB of LittleFS.  Called by both extraction paths. */
static void hp_drop_stale_index(void)
{
    /* A new shell was just written to LittleFS — invalidate the PSRAM copy so
     * the next page load serves the updated shell instead of the old cached one. */
    shell_cache_flush();

    FILE *gz = fopen("/spiffs/web/index.html.gz", "rb");
    if (!gz) return;                 /* no gzip shell present — nothing to do */
    fclose(gz);
    FILE *pl = fopen("/spiffs/web/index.html", "rb");
    if (!pl) return;                 /* no stale plain copy */
    fclose(pl);
    if (remove("/spiffs/web/index.html") == 0)
        ESP_LOGI(TAG, "[webui] removed stale uncompressed index.html (superseded by .gz)");
    else
        ESP_LOGW(TAG, "[webui] could not remove stale index.html: %s", strerror(errno));
}

/* Same reasoning and pattern as hp_drop_stale_index() above, for a
 * different rename: pre-1.18.6 builds shipped marked.min.js (a minified,
 * years-old marked release); 1.18.6 replaced it outright with an
 * unminified, current marked.js rather than shipping both. A device
 * updating from an older build would otherwise keep the orphaned old file
 * forever — nothing loads it once marked.js exists, it just wastes ~36 KB
 * of LittleFS. Called from the same two sites as hp_drop_stale_index(). */
static void hp_drop_stale_marked(void)
{
    FILE *nf = fopen("/spiffs/web/marked.js", "rb");
    if (!nf) return;                 /* new file not present yet — nothing to do */
    fclose(nf);
    FILE *old = fopen("/spiffs/web/marked.min.js", "rb");
    if (!old) return;                /* no stale copy */
    fclose(old);
    if (remove("/spiffs/web/marked.min.js") == 0)
        ESP_LOGI(TAG, "[webui] removed stale marked.min.js (superseded by marked.js)");
    else
        ESP_LOGW(TAG, "[webui] could not remove stale marked.min.js: %s", strerror(errno));
}

/* Shared ZIP local-file-header extractor for both flash-write paths that
 * apply a WebUI patch ZIP: api_fs_hotpatch (whole ZIP already buffered in
 * PSRAM from an authenticated POST body) and webui_pull_task (ZIP just
 * downloaded over HTTPS from a release asset).  Walks LFH entries directly
 * — no central-directory dependency, STORE-only — writing each entry to
 * "/spiffs/<name>", creating parent dirs as needed.  Rejects path traversal
 * and protects config.json from ever being overwritten by a patch, exactly
 * as both call sites did before this was factored out. Paths on the stock-
 * file exception list (files the user removed on purpose) are skipped too, so
 * a patch can't re-create part of a deleted theme.
 *
 * zip/zip_len    : the fully-buffered ZIP (caller has already verified the
 *                   leading ZIP_LFH_SIG and owns/frees the buffer).
 * log_prefix     : short tag prepended to this function's log lines so the
 *                   two call sites stay distinguishable ("hotpatch:" vs
 *                   "[webui] ") — the only cosmetic difference between them.
 * success_at_info: hotpatch is a small, manually-triggered action, so each
 *                   written file is logged at INFO for visibility; webui
 *                   pulls can contain hundreds of files, so it logs each
 *                   success at DEBUG and relies on the one-line summary at
 *                   INFO — pass true/false to preserve that per-caller
 *                   verbosity choice.
 * files_written  : optional (NULL allowed) — if non-NULL, the name of every
 *                   successfully-written file is appended to it as a cJSON
 *                   string.  Used by api_fs_hotpatch to report the file list
 *                   back over HTTP; webui_pull_task has no HTTP response to
 *                   build for this and passes NULL.
 * only_prefix   : NULL for patches; a theme-pack install passes "images/themes/<id>/" and every entry outside it is
 *                   rejected (counted as failed) instead of written.
 * honor_exceptions: false for a theme-pack install, where the user asked for the files; patches pass true.
 * out_ok/out_skipped/out_failed/out_unchanged: caller-owned counters,
 *                   incremented (not reset) by this call — caller
 *                   zero-initializes them. out_unchanged counts entries
 *                   whose write was skipped because the file already on
 *                   LittleFS is byte-identical (see hp_file_unchanged()
 *                   above) — not counted in out_ok/files_written since
 *                   nothing was actually written.
 *
 * Caller remains responsible for shell_cache_flush() before extraction and
 * for hp_drop_stale_index() / display_theme_cache_flush() / fs_usage_invalidate()
 * afterward — those differ slightly in exactly which ones run at each call
 * site, so they stay outside this helper. */
static void extract_zip_to_littlefs(const uint8_t *zip, size_t zip_len,
                                     const char *log_prefix, bool success_at_info,
                                     cJSON *files_written,
                                     int *out_ok, int *out_skipped, int *out_failed,
                                     int *out_unchanged, const char *only_prefix, bool honor_exceptions)
{
    const uint8_t *p   = zip;
    const uint8_t *end = zip + zip_len;

    /* PSRAM snapshot (3 KB — too big for this task's stack), freed after the loop.
     * If the allocation fails nothing is excepted, i.e. the old behavior. */
    char (*exc)[STOCK_EXCEPTION_PATH_LEN] =
        heap_caps_malloc(STOCK_EXCEPTION_MAX * sizeof(*exc), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t exc_count = 0;
    if (exc) {
        config_lock();
        const nextube_config_t *cfg = config_get();
        exc_count = cfg->stock_exception_count;
        memcpy(exc, cfg->stock_exception_paths, STOCK_EXCEPTION_MAX * sizeof(*exc));
        config_unlock();
    }

    while (p + (int)sizeof(zip_lfh_t) <= end) {
        uint32_t sig = (uint32_t)(p[0] | p[1]<<8 | p[2]<<16 | p[3]<<24);

        if (sig == ZIP_CDH_SIG || sig == ZIP_EOCD_SIG) break; /* done */
        if (sig != ZIP_LFH_SIG) { p++; continue; }            /* re-sync  */

        const zip_lfh_t *h = (const zip_lfh_t *)p;
        p += sizeof(zip_lfh_t);

        /* Size-based bounds checks: compare remaining space against the field
         * lengths rather than `p + len > end`.  comp_sz is an attacker- (or
         * corruption-) controlled uint32_t, so the pointer-arithmetic form can
         * wrap on 32-bit and silently bypass the guard, leading to an OOB read
         * in the fwrite below. */
        if ((size_t)(h->fname_len + h->extra_len) > (size_t)(end - p)) break;
        char fname[256] = {0};
        int fnl = h->fname_len < 255 ? h->fname_len : 255;
        memcpy(fname, p, fnl);
        p += h->fname_len + h->extra_len;

        if (h->comp_sz > (size_t)(end - p)) break;
        const uint8_t *data = p;
        p += h->comp_sz;

        /* Skip directory entries and zero-length filenames. */
        if (fnl == 0) continue;
        if (fname[fnl - 1] == '/') {
            ESP_LOGD(TAG, "%s dir  %s", log_prefix, fname);
            continue;
        }

        /* Reject path traversal. */
        if (strstr(fname, "..") || strstr(fname, "//")) {
            ESP_LOGW(TAG, "%s rejected unsafe path '%s'", log_prefix, fname);
            (*out_failed)++;
            continue;
        }

        if (only_prefix && strncmp(fname, only_prefix, strlen(only_prefix)) != 0) {
            ESP_LOGW(TAG, "%s rejected '%s' (outside %s)", log_prefix, fname, only_prefix);
            (*out_failed)++;
            continue;
        }

        /* Protect user config — it must never be overwritten by a patch. */
        if (strcmp(fname, "config.json") == 0) {
            ESP_LOGI(TAG, "%s skipped protected file config.json", log_prefix);
            (*out_skipped)++;
            continue;
        }

        /* Files the user removed on purpose stay removed. */
        if (honor_exceptions && exc && stock_path_excepted(exc, exc_count, fname)) {
            if (success_at_info)
                ESP_LOGI(TAG, "%s skipped '%s' (on the stock-file exception list)", log_prefix, fname);
            else
                ESP_LOGD(TAG, "%s skipped '%s' (on the stock-file exception list)", log_prefix, fname);
            (*out_skipped)++;
            continue;
        }

        /* Only STORE (uncompressed) entries are supported. */
        if (h->method != 0) {
            ESP_LOGW(TAG, "%s skipped '%s' (method=%u; only STORE supported — "
                          "regenerate ZIP with 'zip -0')", log_prefix, fname, h->method);
            (*out_skipped)++;
            continue;
        }

        /* Build VFS path and create any missing parent dirs. */
        char vpath[320];
        snprintf(vpath, sizeof(vpath), "/spiffs/%s", fname);

        if (hp_file_unchanged(vpath, h->crc32, h->uncomp_sz)) {
            ESP_LOGD(TAG, "%s unchanged '%s' (%u B) — write skipped",
                     log_prefix, fname, (unsigned)h->uncomp_sz);
            (*out_unchanged)++;
            vTaskDelay(1);
            continue;
        }

        hp_mkdir_p(vpath);

        FILE *f = fopen(vpath, "wb");
        if (!f) {
            ESP_LOGE(TAG, "%s fopen failed for '%s': %s", log_prefix, vpath, strerror(errno));
            (*out_failed)++;
            continue;
        }
        size_t written = fwrite(data, 1, h->comp_sz, f);
        fclose(f);

        if (written != h->comp_sz) {
            ESP_LOGE(TAG, "%s short write '%s' (%u/%u)", log_prefix, fname,
                     (unsigned)written, (unsigned)h->comp_sz);
            (*out_failed)++;
        } else {
            if (success_at_info)
                ESP_LOGI(TAG, "%s wrote '%s' (%u B)", log_prefix, fname, (unsigned)h->comp_sz);
            else
                ESP_LOGD(TAG, "%s wrote '%s' (%u B)", log_prefix, fname, (unsigned)h->comp_sz);
            if (files_written) cJSON_AddItemToArray(files_written, cJSON_CreateString(fname));
            (*out_ok)++;
        }
        vTaskDelay(1);   /* yield between writes — flash HAL disables IRQs, starves IDLE */
    }
    free(exc);
}

/* Guard wrapper — same s_ota_active reject-concurrent-OTA pattern as
 * api_ota/api_fs_ota (see api_ota's own comment). Needed here for two
 * reasons, not just consistency: it stops a hotpatch from racing an
 * in-progress OTA/webUI-pull/stock-repair on the same shared flash/
 * LittleFS resources, and — since s_ota_active is checked by
 * stock_files_check_timer_cb()'s own guard — it's what lets that
 * boot-time deferred scan correctly back off for the whole duration of a
 * hotpatch too, instead of only for the async task-based flows. Cleared
 * on every exit from the impl below (unlike api_ota/api_fs_ota, which
 * only clear on failure): this path never reboots, so nothing else will
 * ever clear it for us. */
static esp_err_t api_fs_hotpatch_impl(httpd_req_t *r);
static esp_err_t api_fs_hotpatch(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    if (s_ota_active) {
        httpd_resp_set_status(r, "409 Conflict");
        httpd_resp_set_type(r, "application/json");
        httpd_resp_sendstr(r, "{\"error\":\"ota_in_progress\"}");
        return ESP_OK;   /* complete response already sent */
    }
    s_ota_active = true;
    esp_err_t e = api_fs_hotpatch_impl(r);
    s_ota_active = false;
    return e;
}

static esp_err_t api_fs_hotpatch_impl(httpd_req_t *r)
{
    int content_len = r->content_len;
    if (content_len <= 0)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "Content-Length required"), ESP_FAIL;
    if (content_len > HP_MAX_ZIP)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "ZIP too large (4 MB max)"), ESP_FAIL;

    /* Receive entire ZIP into PSRAM so we can parse it in one pass. */
    uint8_t *zip = heap_caps_malloc(content_len,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!zip)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of PSRAM"), ESP_FAIL;

    int rx = 0;
    while (rx < content_len) {
        int n = httpd_req_recv(r, (char *)zip + rx, content_len - rx);
        if (n <= 0) { free(zip); return ESP_FAIL; }
        rx += n;
    }

    /* Validate ZIP magic. */
    if (rx < 4 || (uint32_t)(zip[0] | zip[1]<<8 | zip[2]<<16 | zip[3]<<24) != ZIP_LFH_SIG) {
        free(zip);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "Not a valid ZIP file"), ESP_FAIL;
    }

    /* Pause background tasks for the extraction — webui_pull_task() (this
     * function's sibling: same extract_zip_to_littlefs() work, just from a
     * device-driven HTTPS download instead of a browser upload) already
     * does this; this handler never did, despite doing the identical
     * LittleFS-writing work. Worth keeping regardless of the paragraph
     * below: even once contention is fully ruled out as the dominant cost
     * (see extraction_ms below), an unsuspended weather/update_check TLS
     * handshake or an animated WeatherLive frame landing mid-extraction is
     * still real, avoidable interference this closes off.
     *
     * tls_sem_take() BEFORE ota_suspend_tasks(), never after — same
     * ordering rule as every other flash-write path in this file:
     * ota_suspend_tasks() suspends net_poll, which independently takes
     * this same semaphore for its own HTTPS fetches; suspending it while
     * it might be mid-handshake holding the semaphore would freeze it
     * there forever without releasing it. */
    display_show_wait();
    tls_sem_take();
    ota_suspend_tasks();

    /* Invalidate the shell cache NOW so any request arriving during extraction
     * gets a cache-miss and re-reads from LittleFS after the new file lands,
     * not the stale pre-extraction version. */
    shell_cache_flush();

    cJSON *files_arr = cJSON_CreateArray();
    int ok = 0, skipped = 0, failed = 0, unchanged = 0;

    /* Timed: extraction can take far longer than task contention alone
     * explains, even with every other task already suspended above.
     * hp_file_unchanged() separately logs any single file whose read+CRC
     * pass exceeds 50 ms, for comparing outlier-file vs. fopen/fclose
     * overhead on this LittleFS partition. */
    int64_t extract_t0 = esp_timer_get_time();
    extract_zip_to_littlefs(zip, (size_t)rx, "hotpatch:", /*success_at_info=*/true,
                             files_arr, &ok, &skipped, &failed, &unchanged, NULL, true);
    int64_t extraction_ms = (esp_timer_get_time() - extract_t0) / 1000;

    free(zip);
    hp_drop_stale_index();
    hp_drop_stale_marked();
    display_theme_cache_flush();   /* re-probe PNG format on next render */
    fs_usage_invalidate();
    /* Async, not a direct call: stock_files_check() paces itself in batches
     * (see its own doc comment) specifically so a 384-file scan doesn't
     * block the display for the ~13-15s+ it takes — but called
     * synchronously here it blocked THIS function's own completion for
     * that same stretch, holding every suspended background task
     * (net_poll/ntp/sht30/leds/mic/ha_mqtt) suspended and the tubes stuck
     * on the wait screen the whole time, well past the extraction time this
     * log line's own timing (extraction_ms) already accounts for — a real
     * boot log showed ~34s between suspend and resume for a ~2.6s
     * extraction, almost all of it this scan. Spawning it lets the
     * response, task-resume, and display-resume below happen immediately;
     * the missing-files banner just catches up a little later, same as it
     * already tolerates elsewhere (exception saves, uploads). Safe right
     * after extraction: nothing is still writing to the flash bus at this
     * point. Goes through stock_files_check_spawn(), not a raw xTaskCreate:
     * this now runs before tls_sem_give() below releases the lock that used
     * to keep this scan single-instance for free — see that function's own
     * comment. A creation failure here just means the banner doesn't
     * refresh this cycle — no retry; the next boot or manual Verify & Repair
     * still catches it. */
    stock_files_check_spawn();
    ESP_LOGI(TAG, "hotpatch complete: %d written, %d skipped, %d failed, %d unchanged, extraction took %lld ms",
             ok, skipped, failed, unchanged, (long long)extraction_ms);

    /* This flow doesn't reboot — unlike the OTA/firmware paths, so unlike
     * them the display needs to be explicitly un-parked here or the tubes
     * stay stuck on wait.jpg forever with no reboot ever coming to fix it
     * (same reasoning as webui_pull_task's identical call). */
    webui_resume_tasks();
    tls_sem_give();
    display_resume_after_wait();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", failed == 0 ? "ok" : "partial");
    cJSON_AddNumberToObject(resp, "written",   ok);
    cJSON_AddNumberToObject(resp, "skipped",   skipped);
    cJSON_AddNumberToObject(resp, "failed",    failed);
    cJSON_AddNumberToObject(resp, "unchanged", unchanged);
    cJSON_AddItemToObject(resp, "files", files_arr);
    char *js = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    esp_err_t ret = httpd_resp_set_type(r, "application/json") == ESP_OK
                  ? httpd_resp_sendstr(r, js)
                  : ESP_FAIL;
    free(js);
    return ret;
}

/* ── ESP-pull OTA ──────────────────────────────────────────────────────────
 *
 * The browser fetches release metadata from api.github.com, extracts the
 * direct asset URLs, then asks the ESP to pull the binaries itself.  This
 * avoids the CORS problem (release assets redirect to Azure Blob Storage
 * which does not send Access-Control-Allow-Origin) and removes any proxy
 * dependency.
 *
 * Endpoints:
 *   POST /api/ota_pull        {"url":"https://...nextube-fw-vX-ota.bin"}
 *   GET  /api/ota_pull_status → {"state":"idle|downloading|flashing|done|error","progress":0-100}
 *   POST /api/webui_pull      {"url":"https://...nextube-WebUI-vX.zip"}
 */

typedef enum {
    OTA_PULL_IDLE        = 0,
    OTA_PULL_DOWNLOADING = 1,
    OTA_PULL_FLASHING    = 2,
    OTA_PULL_DONE        = 3,
    OTA_PULL_ERROR       = 4,
} ota_pull_state_t;

static struct {
    ota_pull_state_t state;
    int  progress;
    char error[128];
    char url[512];
    char sha256[65];        /* expected SHA-256 hex (64 chars + NUL), empty = skip */
    char webui_url[512];    /* WebUI ZIP URL to apply after firmware reboot */
    char webui_sha256[65];  /* SHA-256 of WebUI ZIP, empty = skip */
    int  bytes_received;
    int  bytes_total;
    int  bytes_flashed;
} s_pull;

/* Compute SHA-256 over buf and compare against a 64-char lowercase hex string.
 * Returns true if hashes match or expected_hex is empty (verification skipped).
 * Returns false on mismatch or if expected_hex is malformed. */
static bool sha256_verify(const uint8_t *buf, size_t len, const char *expected_hex)
{
    if (!expected_hex || expected_hex[0] == '\0') return true;

    /* Validate: must be exactly 64 lowercase hex chars */
    if (strlen(expected_hex) != 64) return false;
    for (int i = 0; i < 64; i++) {
        char c = expected_hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }

    /* Compute digest */
    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);   /* 0 = SHA-256, not SHA-224 */
    mbedtls_sha256_update(&ctx, buf, len);
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    /* Convert expected hex to bytes, then compare in constant time (same
     * discipline as auth.c token/hash comparisons). */
    uint8_t expected[32];
    for (int i = 0; i < 32; i++) {
        int hi = expected_hex[i * 2];
        int lo = expected_hex[i * 2 + 1];
        hi = (hi <= '9') ? hi - '0' : hi - 'a' + 10;
        lo = (lo <= '9') ? lo - '0' : lo - 'a' + 10;
        expected[i] = (uint8_t)((hi << 4) | lo);
    }
    return mbedtls_ct_memcmp(digest, expected, sizeof(digest)) == 0;
}

#define OTA_PULL_TIMEOUT_MS 120000
#define HTTP_UA_BASE "NextubeRemaster/" FW_VERSION_STR " github.com/"
#define HTTP_UA_REPO_DEFAULT "MrToast99/Nextube-Remaster"

/* ── Shared HTTPS-fetch machinery ─────────────────────────────────────────
 * ota_pull_task, webui_pull_task, and stock_repair_task each fetch
 * something over HTTPS from GitHub on the device's own initiative — a
 * firmware image, a webUI ZIP, or one stock file respectively. Before this,
 * each had its own near-identical ~50-80 line copy of: resolve the
 * update_repo override, build a user-agent, open the URL following
 * redirects, fetch/validate headers, and read the body. That's now these
 * two functions; what still varies per caller — the allocation strategy
 * (ota_pull_task falls back to internal RAM if PSRAM is unavailable, since
 * a firmware image can't be chunked; the other two just fail), the
 * size cap, live progress reporting, and everything that happens to the
 * bytes afterward (partition write vs. ZIP extraction vs. per-file
 * SHA-256-gated write) — stays in each task, on purpose: those differences
 * are real, not incidental duplication, and forcing them into one generic
 * function would trade clarity for a smaller line count on exactly the
 * code paths (firmware OTA, webUI update) where a bug has the highest cost. */

/* Resolves the owner/repo string used for every GitHub URL a device builds
 * on its own: the config's update_repo override if set, else the hardcoded
 * default — so a self-hosted fork isn't hardcoded out of any pull path. */
static void resolve_update_repo(char *out, size_t out_sz)
{
    config_lock();
    strlcpy(out, config_get()->update_repo, out_sz);
    config_unlock();
    if (!out[0]) snprintf(out, out_sz, "%s", HTTP_UA_REPO_DEFAULT);
}

/* Hard backstop for esp_http_client calls that can apparently hang past
 * their own configured .timeout_ms without ever returning, holding tls_sem
 * the whole time — which starves every other HTTPS-using task (weather,
 * subscribers, update_check, MQTT) sharing that same semaphore. A
 * deliberate, bounded reboot here is a strictly better outcome than an
 * indefinite hang that silently breaks every other HTTPS task on the
 * device.
 *
 * Reuses ota_reboot_timer_cb() rather than a near-duplicate callback —
 * both just need esp_restart() with nothing to clean up first. Deadline is
 * the caller's own timeout_ms plus 30s margin, so a well-behaved timeout
 * resolves through the normal error path first in the ordinary case; this
 * only fires once that mechanism has already failed to return control at
 * all. A NULL return (esp_timer_create failed) is survivable — the caller
 * just proceeds without a backstop for that one call. Used by both
 * hp_https_open() and hp_https_read_body() below. */
static esp_timer_handle_t hp_arm_hard_deadline(int caller_timeout_ms)
{
    esp_timer_handle_t t = NULL;
    const esp_timer_create_args_t a = { .callback = ota_reboot_timer_cb, .name = "hp_deadline" };
    if (esp_timer_create(&a, &t) == ESP_OK)
        esp_timer_start_once(t, ((uint64_t)caller_timeout_ms + 30000) * 1000ULL);
    return t;
}

static void hp_disarm_hard_deadline(esp_timer_handle_t t)
{
    if (!t) return;
    esp_timer_stop(t);      /* harmless if it already fired — a reboot is already underway */
    esp_timer_delete(t);
}

/* Opens `url`, following up to 5 redirect hops (GitHub release assets
 * commonly redirect once to Azure Blob Storage), and fetches response
 * headers. On success, *client_out is an OPEN client positioned to read the
 * body and *content_len_out is the validated Content-Length (already
 * checked against max_len — pass 0 for no cap); the caller must go on to
 * call hp_https_read_body() to actually read the body and release the
 * client. On failure, the client (if any) has already been closed and
 * cleaned up — nothing left for the caller to release — and a human-
 * readable reason is written to err. log_tag prefixes every log line
 * ("ota"/"webui"/"stock_repair") so the three callers stay distinguishable
 * in the device log, same purpose as extract_zip_to_littlefs's log_prefix. */
static bool hp_https_open(const char *url, const char *user_agent, int timeout_ms, int max_len,
                           esp_http_client_handle_t *client_out, int *content_len_out,
                           char *err, size_t err_sz, const char *log_tag)
{
    esp_timer_handle_t deadline = hp_arm_hard_deadline(timeout_ms);
    esp_http_client_config_t hcfg = {
        .url                   = url,
        .timeout_ms            = timeout_ms,
        .user_agent            = user_agent,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .max_redirection_count = 3,
        .buffer_size           = 16384,   /* Azure Blob returns many x-ms-* headers */
        /* Sizes the OUTGOING request write (buffer_size above sizes the
         * incoming response read) — esp_http_client_request_send() builds
         * the whole "GET <url> HTTP/1.1\r\n" + headers into one buffer this
         * size. Azure Blob's SAS-signed redirect URLs run several hundred
         * bytes of query string alone, so a smaller budget here leaves no
         * room for our own headers and forces an extra write+parse round
         * trip. 4096 covers a long SAS URL plus our header set in one
         * pass; transient, freed on esp_http_client_cleanup(). */
        .buffer_size_tx        = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) {
        snprintf(err, err_sz, "HTTP client init failed");
        ESP_LOGE(TAG, "[%s] %s", log_tag, err);
        hp_disarm_hard_deadline(deadline);
        return false;
    }

    int content_len = -1, status_code = -1;
    for (int rd = 0; rd <= 5; rd++) {
        ESP_LOGD(TAG, "[%s] HTTP open hop %d…", log_tag, rd);
        if (esp_http_client_open(client, 0) != ESP_OK) {
            snprintf(err, err_sz, "HTTP connect failed (hop %d)", rd);
            ESP_LOGE(TAG, "[%s] %s", log_tag, err);
            esp_http_client_cleanup(client);
            hp_disarm_hard_deadline(deadline);
            return false;
        }
        content_len = esp_http_client_fetch_headers(client);
        status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "[%s] hop %d → HTTP %d, content-length=%d", log_tag, rd, status_code, content_len);
        if (status_code == 200) break;
        if (status_code != 301 && status_code != 302 &&
            status_code != 307 && status_code != 308) break;
        ESP_LOGD(TAG, "[%s] following redirect…", log_tag);
        esp_http_client_set_redirection(client);
    }

    if (status_code != 200 || content_len <= 0 || (max_len > 0 && content_len > max_len)) {
        snprintf(err, err_sz, "HTTP %d (content-length=%d)", status_code, content_len);
        ESP_LOGE(TAG, "[%s] %s", log_tag, err);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        hp_disarm_hard_deadline(deadline);
        return false;
    }

    hp_disarm_hard_deadline(deadline);
    *client_out = client;
    *content_len_out = content_len;
    return true;
}

/* Reads exactly up to content_len bytes of the (already-open, via
 * hp_https_open()) client's response body into buf, then ALWAYS closes and
 * cleans up the client — caller must not touch it again either way.
 * Returns the number of bytes actually received; compare against
 * content_len to detect a short read. progress_cb, if non-NULL, is called
 * after each chunk with (received, content_len) — used by ota_pull_task to
 * drive its live download-percentage UI; the other two callers pass NULL.
 *
 * timeout_ms is the same value the caller passed to hp_https_open() for
 * this same fetch — used here only to size the hard deadline (see
 * hp_arm_hard_deadline()) alongside a data-proportional allowance (20 KB/s
 * minimum-throughput assumption — real observed throughput on this device
 * was ~130 KB/s, so this has real margin without being so generous that a
 * genuine hang lingers for minutes before the backstop resolves it). The
 * observed field failure was inside hp_https_open()'s fetch_headers() call,
 * not here, but esp_http_client_read() is the same class of "can it
 * actually be trusted to respect its own .timeout_ms" call — worth the same
 * backstop rather than assuming this one's fine because it hasn't been
 * caught hanging yet. */
/* Cap on each esp_http_client_read() request below — NOT a transport/socket
 * limit, a progress-reporting one. esp_http_client_read() internally loops
 * until it has satisfied the full length requested (or hits EOF/error), so
 * requesting the entire remaining content_len in one call — the previous
 * behaviour — made it block for the WHOLE download before returning even
 * once: received jumped straight from 0 to content_len in a single step,
 * skipping every 10% log line in between, and (more than cosmetic)
 * progress_cb — which drives s_pull.progress, the value the web UI polls
 * live during an "Update Now" download — only ever fired once too, so the
 * on-screen progress bar jumped straight to the download phase's ceiling
 * instead of animating. Matches buffer_size below it in spirit — no need
 * for a separate magic number — and comfortably yields well over one
 * callback per 10% step for anything this firmware downloads (~11 calls
 * for the smallest realistic webUI ZIP, 100+ for a full firmware image). */
#define HP_READ_CHUNK 16384

static int hp_https_read_body(esp_http_client_handle_t client, uint8_t *buf, int content_len,
                               void (*progress_cb)(int received, int total), const char *log_tag,
                               int timeout_ms)
{
    esp_timer_handle_t deadline = hp_arm_hard_deadline(timeout_ms + (content_len / 20480) * 1000);
    int received = 0, last_pct = -1;
    while (received < content_len) {
        int want = content_len - received;
        if (want > HP_READ_CHUNK) want = HP_READ_CHUNK;
        int n = esp_http_client_read(client, (char *)buf + received, want);
        if (n <= 0) break;
        received += n;
        if (progress_cb) progress_cb(received, content_len);
        int pct = (received * 100) / content_len;
        if (pct / 10 != last_pct / 10) {
            last_pct = pct;
            ESP_LOGI(TAG, "[%s] download %d%% (%d/%d B)", log_tag, pct, received, content_len);
        }
    }
    hp_disarm_hard_deadline(deadline);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return received;
}


/* hp_https_read_body()'s progress_cb for ota_pull_task specifically — drives
 * the live download-percentage the web UI polls via /api/ota_pull_status.
 * Scaled to 0-60%: flashing (not modeled here) takes the remaining 60-100%. */
static void ota_pull_progress_cb(int received, int total)
{
    s_pull.bytes_received = received;
    s_pull.progress = (received * 60) / total;
}

static void ota_pull_task(void *arg)
{
    ESP_LOGI(TAG, "[ota] task started on core %d", xPortGetCoreID());
    ESP_LOGI(TAG, "[ota] url: %.120s", s_pull.url);
    if (s_pull.sha256[0])
        ESP_LOGD(TAG, "[ota] sha256: %.16s…", s_pull.sha256);
    if (s_pull.webui_url[0])
        ESP_LOGI(TAG, "[ota] webui_url: %.120s", s_pull.webui_url);

    s_pull.state    = OTA_PULL_DOWNLOADING;
    s_pull.progress = 0;

    /* Wait for a valid wall-clock before opening any TLS connection.  MUST run
     * BEFORE ota_suspend_tasks() below — that suspends the "ntp" task itself,
     * so if it ran first and time wasn't valid yet, this loop would spin the
     * full 30 s waiting on a task that can no longer ever sync, and every
     * pull attempted before the first NTP sync would fail outright. */
    if (!ntp_has_valid_time()) {
        ESP_LOGW(TAG, "[ota] time not valid — waiting up to 30 s for NTP/RTC…");
        for (int i = 0; i < 30 && !ntp_has_valid_time(); i++) {
            ESP_LOGD(TAG, "[ota] time sync wait %d/30", i + 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (!ntp_has_valid_time()) {
        snprintf(s_pull.error, sizeof(s_pull.error),
                 "No valid time after 30 s — NTP/RTC unavailable");
        ESP_LOGE(TAG, "[ota] %s", s_pull.error);
        goto pull_err;
    }
    ESP_LOGI(TAG, "[ota] time valid — heap free %lu B", (unsigned long)esp_get_free_heap_size());

    /* Park the display BEFORE the download starts, not just before the flash
     * write — matching api_ota_impl (manual upload), which has always parked
     * immediately.  The animated display task alone runs at up to ~50-90%
     * CPU (WeatherLive); leaving it fully active for the whole multi-second
     * download meant httpd's single worker task could be slow enough to
     * answer /api/ota_pull_status polls that the web UI missed the brief
     * download→flashing transition, despite the two 1.5 s holds further down
     * being sized for exactly the opposite assumption (a quiet, unsuspended
     * device stalls this handoff).  display_show_wait() only touches the
     * display task — safe to call this early with no effect on tls_sem
     * below. */
    display_show_wait();

    ESP_LOGD(TAG, "[ota] waiting for TLS semaphore…");
    tls_sem_take();
    ESP_LOGD(TAG, "[ota] TLS semaphore acquired");

    /* ota_suspend_tasks() suspends net_poll (weather + subscribers +
     * update_check's shared task — see periodic_net_poll.h), which
     * independently takes this SAME tls_sem for its own HTTPS fetches — it
     * must run AFTER we've already acquired the semaphore above, never
     * before.  If it ran first and net_poll happened to be mid-handshake
     * (holding tls_sem) at that instant, vTaskSuspend would freeze it there
     * forever without releasing the semaphore, and our tls_sem_take() above
     * would then burn its full 30 s timeout and proceed UNSERIALIZED —
     * reintroducing the internal-RAM-fragmentation risk tls_sem exists to
     * prevent.  Suspending it here, once we already hold the semaphore
     * ourselves, closes that window entirely while still keeping it off the bus for the entire
     * download loop below (not just the flash write, as before). */
    ota_suspend_tasks();

    char s_ota_ua[128];
    { char repo[64];
      resolve_update_repo(repo, sizeof(repo));
      snprintf(s_ota_ua, sizeof(s_ota_ua), HTTP_UA_BASE "%s", repo); }

    esp_http_client_handle_t client;
    int content_len;
    if (!hp_https_open(s_pull.url, s_ota_ua, OTA_PULL_TIMEOUT_MS, 0,
                        &client, &content_len, s_pull.error, sizeof(s_pull.error), "ota"))
        goto pull_err_notls;
    ESP_LOGI(TAG, "[ota] download starting: %d B, heap free %lu B",
             content_len, (unsigned long)esp_get_free_heap_size());

    s_pull.bytes_total    = content_len;
    s_pull.bytes_received = 0;
    s_pull.bytes_flashed  = 0;

    uint8_t *img = (uint8_t *)heap_caps_malloc((size_t)content_len, MALLOC_CAP_SPIRAM);
    if (!img) img = (uint8_t *)malloc((size_t)content_len);
    if (!img) {
        snprintf(s_pull.error, sizeof(s_pull.error),
                 "OOM: can't buffer %d B", content_len);
        ESP_LOGE(TAG, "[ota] %s", s_pull.error);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto pull_err_notls;
    }
    ESP_LOGD(TAG, "[ota] buffer allocated (%s), reading…",
             heap_caps_check_integrity_all(false) ? "PSRAM" : "DRAM");

    int received = hp_https_read_body(client, img, content_len, ota_pull_progress_cb, "ota", OTA_PULL_TIMEOUT_MS);
    tls_sem_give();
    ESP_LOGI(TAG, "[ota] download complete: %d/%d B", received, content_len);

    if (received != content_len) {
        snprintf(s_pull.error, sizeof(s_pull.error),
                 "Short download: %d/%d B", received, content_len);
        ESP_LOGE(TAG, "[ota] %s", s_pull.error);
        free(img);
        goto pull_err;
    }

    ESP_LOGD(TAG, "[ota] magic byte: 0x%02X (expect 0xE9)", img[0]);
    if (img[0] != 0xE9) {
        snprintf(s_pull.error, sizeof(s_pull.error),
                 "Bad magic 0x%02X — wrong file?", img[0]);
        ESP_LOGE(TAG, "[ota] %s", s_pull.error);
        free(img);
        goto pull_err;
    }

    ESP_LOGI(TAG, "[ota] verifying SHA-256…");
    if (!sha256_verify(img, (size_t)received, s_pull.sha256)) {
        snprintf(s_pull.error, sizeof(s_pull.error),
                 "SHA-256 mismatch — download may be corrupt or tampered");
        ESP_LOGE(TAG, "[ota] SHA-256 verification FAILED");
        free(img);
        goto pull_err;
    }
    ESP_LOGI(TAG, "[ota] SHA-256 OK%s", s_pull.sha256[0] ? "" : " (no hash provided, skipped)");

    /* Hold the DOWNLOADING state at 100% so the UI has at least two poll
     * cycles to show the download bar full before the phase transition.  */
    vTaskDelay(pdMS_TO_TICKS(1500));

    s_pull.state    = OTA_PULL_FLASHING;
    s_pull.progress = 60;
    ESP_LOGI(TAG, "[ota] state → FLASHING");

    /* Let the UI catch the FLASHING state and flip its step indicator
     * before httpd load increases during flash erase/write cycles.      */
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* display_show_wait()/ota_suspend_tasks() already ran at task start —
     * no need to repeat them here now that suspension covers the download
     * too, not just the flash write. */

    const esp_partition_t *upd = esp_ota_get_next_update_partition(NULL);
    if (!upd) {
        snprintf(s_pull.error, sizeof(s_pull.error), "No OTA partition");
        ESP_LOGE(TAG, "[ota] %s", s_pull.error);
        free(img);
        goto pull_err;
    }
    ESP_LOGI(TAG, "[ota] writing to partition '%s' at 0x%08" PRIx32 " (%lu B)",
             upd->label, upd->address, (unsigned long)upd->size);

    if ((size_t)received > upd->size) {
        snprintf(s_pull.error, sizeof(s_pull.error),
                 "Image too large (%d B) for OTA partition (%lu B)",
                 received, (unsigned long)upd->size);
        ESP_LOGE(TAG, "[ota] %s", s_pull.error);
        free(img);
        goto pull_err;
    }

    esp_ota_handle_t h;
    if (esp_ota_begin(upd, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
        ota_begin_fail_text(s_pull.error, sizeof(s_pull.error));
        ESP_LOGE(TAG, "[ota] %s", s_pull.error);
        free(img);
        goto pull_err;
    }
    ESP_LOGD(TAG, "[ota] esp_ota_begin OK — flashing %d B…", received);

    const uint8_t *p = img;
    int rem = received;
    int last_pct = -1;   /* independent from hp_https_read_body()'s own — that one tracked download %, this tracks flash-write % */
    while (rem > 0) {
        int chunk = rem > 4096 ? 4096 : rem;
        if (esp_ota_write(h, p, chunk) != ESP_OK) {
            snprintf(s_pull.error, sizeof(s_pull.error), "OTA write failed");
            ESP_LOGE(TAG, "[ota] %s", s_pull.error);
            esp_ota_abort(h);
            free(img);
            goto pull_err;
        }
        p   += chunk;
        rem -= chunk;
        s_pull.bytes_flashed  = received - rem;
        s_pull.progress = 60 + ((received - rem) * 35) / received;
        int pct = ((received - rem) * 100) / received;
        if (pct / 10 != last_pct / 10) {
            last_pct = pct;
            ESP_LOGI(TAG, "[ota] flash %d%% (%d/%d B)", pct, received - rem, received);
        }
        vTaskDelay(1);   /* yield between writes — flash HAL disables IRQs, starves IDLE
                          * (same fix already applied to the webui ZIP-extraction loop;
                          * missing here let IDLE0 go unfed long enough across this loop
                          * + esp_ota_end()'s own read-back verify to trip the 30 s task
                          * watchdog — observed in the field, non-fatal only because
                          * CONFIG_ESP_TASK_WDT_PANIC is off, but right at the edge). */
    }
    free(img);
    ESP_LOGI(TAG, "[ota] flash write done");

    if (esp_ota_end(h) != ESP_OK) {
        snprintf(s_pull.error, sizeof(s_pull.error), "OTA finalise failed");
        ESP_LOGE(TAG, "[ota] esp_ota_end FAILED");
        goto pull_err;
    }
    ESP_LOGD(TAG, "[ota] esp_ota_end OK");

    if (esp_ota_set_boot_partition(upd) != ESP_OK) {
        snprintf(s_pull.error, sizeof(s_pull.error), "OTA finalise failed");
        ESP_LOGE(TAG, "[ota] esp_ota_set_boot_partition FAILED");
        goto pull_err;
    }
    ESP_LOGI(TAG, "[ota] boot partition set to '%s'", upd->label);

    s_pull.state    = OTA_PULL_DONE;
    s_pull.progress = 100;

    /* Flag that the next webui_pull call may bypass session auth — the reboot
     * will clear RAM sessions so the browser can't re-authenticate mid-flow. */
    {
        nvs_handle_t nvs_h;
        if (nvs_open("nextube_sec", NVS_READWRITE, &nvs_h) == ESP_OK) {
            esp_err_t e1 = nvs_set_u8(nvs_h, "post_ota", 1);
            ESP_LOGI(TAG, "[ota] NVS: post_ota=1 (%s)", esp_err_to_name(e1));
            if (s_pull.webui_url[0]) {
                esp_err_t e2 = nvs_set_str(nvs_h, "webui_url", s_pull.webui_url);
                ESP_LOGI(TAG, "[ota] NVS: webui_url %s (%s)",
                         e2 == ESP_OK ? "saved" : "FAILED", esp_err_to_name(e2));
            }
            if (s_pull.webui_sha256[0]) {
                esp_err_t e3 = nvs_set_str(nvs_h, "webui_sha256", s_pull.webui_sha256);
                ESP_LOGI(TAG, "[ota] NVS: webui_sha256 %s (%s)",
                         e3 == ESP_OK ? "saved" : "FAILED", esp_err_to_name(e3));
            }
            esp_err_t ec = nvs_commit(nvs_h);
            nvs_close(nvs_h);
            ESP_LOGI(TAG, "[ota] NVS committed (%s)", esp_err_to_name(ec));
        } else {
            ESP_LOGE(TAG, "[ota] NVS open failed — post_ota flag NOT set");
        }
    }
    ESP_LOGI(TAG, "[ota] complete — rebooting in 1.5 s");
    ESP_LOGI(TAG, "[ota] stack high-water mark: %u B unused (of %u allocated)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)PERIODIC_NET_POLL_STACK_SIZE);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return;

pull_err_notls:
    tls_sem_give();
pull_err:
    ESP_LOGE(TAG, "[ota] FAILED: %s", s_pull.error);
    ESP_LOGI(TAG, "[ota] stack high-water mark: %u B unused (of %u allocated)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)PERIODIC_NET_POLL_STACK_SIZE);
    s_pull.state = OTA_PULL_ERROR;
    s_ota_active = false;
}

/* Network jobs (firmware pull, Web UI pull, stock repair) run on the shared net_poll task. They used to get a fresh task each time,
 * which failed with "Task create failed" once the internal heap was fragmented enough that no free block was as big as their
 * stack. net_poll's stack is allocated once at boot, so starting an update allocates nothing. The job runs at the priority the
 * old tasks had; only one runs at a time (s_np_job stays set until it returns), and a job needs the network to be up. Their stack
 * need is measured at the end of every job (high-water mark logs): ota_pull ~4.1 KB, webui_pull ~4.3 KB, stock_repair ~4.5 KB. */
#define NP_JOB_PRIORITY 5
static void (*volatile s_np_job)(void *);

static bool np_job_start(void (*fn)(void *))
{
    if (s_np_job || !wifi_manager_is_connected()) return false;
    s_np_job = fn;
    periodic_net_poll_force("jobs");   /* wakes the shared network task, which runs np_job_tick() */
    return true;
}

/* Appends `item` to the bounded, comma-joined list at buf (current length
 * tracked via *len_io) — shared by webui_pull_task and stock_repair_task to
 * report which files a pull/repair actually changed, in the same shape, on
 * both /api/webui_pull_status and /api/stock_repair_status. `remaining` is
 * how many not-yet-tried items (including this one) the caller still has —
 * once `item` wouldn't fit, this appends "+<remaining> more" once and every
 * later call for that same list becomes a silent no-op (buf is left alone,
 * *len_io pinned at buf_sz so the "already full" check above catches it). */
static void append_bounded_list(char *buf, size_t buf_sz, size_t *len_io,
                                 const char *item, int remaining)
{
    if (*len_io >= buf_sz) return;   /* already full/truncated */
    size_t need = strlen(item) + (*len_io ? 2 : 0);
    if (*len_io + need + 24 >= buf_sz) {   /* +24: headroom for "+N more" */
        *len_io += snprintf(buf + *len_io, buf_sz - *len_io, "%s+%d more",
                             *len_io ? ", " : "", remaining);
        *len_io = buf_sz;   /* sentinel: stop accepting further items */
        return;
    }
    *len_io += (size_t)snprintf(buf + *len_io, buf_sz - *len_io, "%s%s",
                                 *len_io ? ", " : "", item);
}

static esp_err_t api_ota_pull_status(httpd_req_t *r)
{
    /* Post-OTA firmware reboot: the NVS post_ota flag is still set, meaning
     * the webui update (Phase 4) hasn't fired yet.  Return 503 so the
     * Phase-2 poll loop (old or new HTML) sees !rs.ok, increments failStreak,
     * and after 8 consecutive 503s exits to Phase 3/4 — even when no admin
     * password is configured (REQUIRE_AUTH would be a no-op in that case and
     * the normal 200+idle response would trap Phase 2 forever). */
    if (s_post_ota_boot_pending && s_pull.state == OTA_PULL_IDLE) {
        httpd_resp_set_status(r, "503 Service Unavailable");
        return httpd_resp_sendstr(r, "");
    }
    REQUIRE_AUTH(r);
    static const char *const names[] = { "idle","downloading","flashing","done","error" };
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"progress\":%d,"
             "\"bytes_received\":%d,\"bytes_total\":%d,\"bytes_flashed\":%d,"
             "\"error\":\"%s\"}",
             names[s_pull.state], s_pull.progress,
             s_pull.bytes_received, s_pull.bytes_total, s_pull.bytes_flashed,
             s_pull.error);
    return send_json(r, buf);
}

static int64_t s_last_ota_pull_us = 0;
#define OTA_PULL_RATE_LIMIT_US  (60LL * 1000000LL)   /* 60 seconds between pulls */

/* Shared body-read-and-parse sequence for the two pull-trigger endpoints
 * (api_ota_pull, api_webui_pull): both accept a POST body capped at 1024
 * bytes of {"url":"...", "sha256":"<64-char hex, optional>", ...}.  Reads
 * the body into a heap buffer, parses it, and extracts "url" (required,
 * non-empty string — copied into url_out, truncated to url_cap like the
 * original strncpy did) and "sha256" (copied into sha_out only if present
 * AND exactly 64 chars, else sha_out[0] is left '\0' — same "hash_ok" test
 * both callers used inline before this was factored out).
 *
 * Deliberately does NOT enforce any policy about whether sha256 is actually
 * required — api_webui_pull has its own post-OTA-auth-bypass-window rule
 * for that (sha256 becomes mandatory only when auth was bypassed) and must
 * apply it itself using the returned sha_out, not have it baked in here.
 *
 * out_root, if non-NULL, receives the still-live parsed cJSON root instead
 * of having this function delete it — api_ota_pull needs it afterward to
 * also pull "webui_url"/"webui_sha256" out of the same body.  Caller then
 * owns it and must cJSON_Delete() it.  Pass NULL to have it deleted here.
 *
 * Returns true on success.  On failure, sends its own 400/500 response (or,
 * for a genuine socket read error, no response — same as before, matching
 * the original bare `return ESP_FAIL`) and returns false; callers should
 * just `return ESP_FAIL` on false. */
static bool read_pull_url_body(httpd_req_t *r, char *url_out, size_t url_cap,
                                char *sha_out, size_t sha_cap, cJSON **out_root)
{
    int len = r->content_len;
    if (len <= 0 || len > 1024) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad body");
        return false;
    }

    char *body = malloc((size_t)len + 1);
    if (!body) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return false;
    }

    int rx = 0;
    while (rx < len) {
        int n = httpd_req_recv(r, body + rx, len - rx);
        if (n <= 0) { free(body); return false; }
        rx += n;
    }
    body[len] = '\0';

    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return false;
    }

    cJSON *url_j = cJSON_GetObjectItem(j, "url");
    if (!cJSON_IsString(url_j) || !url_j->valuestring || !url_j->valuestring[0]) {
        cJSON_Delete(j);
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing url");
        return false;
    }
    snprintf(url_out, url_cap, "%s", url_j->valuestring);

    sha_out[0] = '\0';
    cJSON *hash_j = cJSON_GetObjectItem(j, "sha256");
    if (cJSON_IsString(hash_j) && hash_j->valuestring && strlen(hash_j->valuestring) == 64)
        snprintf(sha_out, sha_cap, "%s", hash_j->valuestring);

    if (out_root) *out_root = j;
    else          cJSON_Delete(j);
    return true;
}

static esp_err_t api_ota_pull(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    if (s_ota_active) {
        httpd_resp_set_status(r, "409 Conflict");
        return send_json(r, "{\"error\":\"ota_in_progress\"}");
    }
    {
        int64_t now = esp_timer_get_time();
        if (s_last_ota_pull_us > 0 && now - s_last_ota_pull_us < OTA_PULL_RATE_LIMIT_US) {
            httpd_resp_set_status(r, "429 Too Many Requests");
            return send_json(r, "{\"error\":\"rate_limited\",\"retry_after_s\":60}");
        }
        s_last_ota_pull_us = now;
    }

    cJSON *j = NULL;
    if (!read_pull_url_body(r, s_pull.url, sizeof(s_pull.url),
                             s_pull.sha256, sizeof(s_pull.sha256), &j))
        return ESP_FAIL;

    /* Optional: webui ZIP URL + hash to apply after firmware reboot.
     * Stored in NVS so the post-reboot handler can start the pull without
     * receiving a POST body (which fails on some connections after a reboot). */
    s_pull.webui_url[0] = s_pull.webui_sha256[0] = '\0';
    cJSON *wurl_j = cJSON_GetObjectItem(j, "webui_url");
    if (cJSON_IsString(wurl_j) && wurl_j->valuestring && wurl_j->valuestring[0]) {
        strncpy(s_pull.webui_url, wurl_j->valuestring, sizeof(s_pull.webui_url) - 1);
        s_pull.webui_url[sizeof(s_pull.webui_url) - 1] = '\0';
    }
    cJSON *wsha_j = cJSON_GetObjectItem(j, "webui_sha256");
    if (cJSON_IsString(wsha_j) && wsha_j->valuestring && strlen(wsha_j->valuestring) == 64) {
        strncpy(s_pull.webui_sha256, wsha_j->valuestring, 64);
        s_pull.webui_sha256[64] = '\0';
    }
    cJSON_Delete(j);

    if (s_pull.sha256[0])
        ESP_LOGI(TAG, "OTA pull: will verify SHA-256 after download");
    else
        ESP_LOGW(TAG, "OTA pull: no SHA-256 provided — integrity check skipped");

    s_pull.state          = OTA_PULL_IDLE;
    s_pull.progress       = 0;
    s_pull.bytes_received = 0;
    s_pull.bytes_total    = 0;
    s_pull.bytes_flashed  = 0;
    s_pull.error[0]       = '\0';
    s_ota_active          = true;

    if (!np_job_start(ota_pull_task)) {
        s_ota_active = false;
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not start (busy or no network)"), ESP_FAIL;
    }
    return send_json(r, "{\"status\":\"started\"}");
}

/* ── WebUI pull background task ─────────────────────────────────────────── */

typedef enum { WEBUI_IDLE=0, WEBUI_RUNNING, WEBUI_DONE, WEBUI_ERROR } webui_pull_state_t;

static struct {
    webui_pull_state_t state;
    char error[128];
    char url[512];
    char sha256[65];
    char written_files[512];   /* bounded, comma-joined list of files this pull
                                 * actually wrote — see append_bounded_list() */
} s_webui;

static const char *webui_state_name(void)
{
    static const char *const names[] = { "idle", "running", "done", "error" };
    return names[s_webui.state];
}

static const char *webui_error_text(void) { return s_webui.error; }


static void webui_pull_task(void *arg)
{
    ESP_LOGI(TAG, "[webui] task started on core %d", xPortGetCoreID());
    ESP_LOGI(TAG, "[webui] url: %.120s", s_webui.url);
    if (s_webui.sha256[0])
        ESP_LOGD(TAG, "[webui] sha256: %.16s…", s_webui.sha256);

    s_webui.state = WEBUI_RUNNING;
    bool tasks_suspended = false;

    /* TLS certificate validation requires a correct wall-clock.  After an OTA
     * reboot the device may not have synced yet (dead RTC battery).  Wait up
     * to 30 s for either an RTC seed or a first NTP response. */
    if (!ntp_has_valid_time()) {
        ESP_LOGW(TAG, "[webui] time not valid — waiting up to 30 s for NTP/RTC…");
        for (int i = 0; i < 30 && !ntp_has_valid_time(); i++) {
            ESP_LOGD(TAG, "[webui] time sync wait %d/30", i + 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (!ntp_has_valid_time()) {
        snprintf(s_webui.error, sizeof(s_webui.error),
                 "No valid time after 30 s — NTP/RTC unavailable");
        ESP_LOGE(TAG, "[webui] %s", s_webui.error);
        goto webui_err;
    }
    ESP_LOGI(TAG, "[webui] time valid — heap free %lu B", (unsigned long)esp_get_free_heap_size());

    /* Show the wait screen BEFORE any of this: gives the user immediate
     * feedback, and — critically — this task extracts files DIRECTLY onto
     * the SAME LittleFS partition the display task reads theme/font/image
     * assets from (see the fopen/fwrite loop below).  Without parking the
     * display first, it can be mid-read on a file this task is concurrently
     * overwriting (partial/torn read), plus the SPI0 bus contention
     * ota_suspend_tasks() below exists to avoid.  Every sibling flash-write
     * path (api_ota_impl, api_fs_ota_impl, ota_pull_task) already does this;
     * this one was the one gap.  Positioned before tls_sem_take() — it only
     * touches the display task, so it's safe to do this early, and wait.jpg
     * loads correctly here because LittleFS still holds the PRE-patch
     * content (see api_fs_ota_impl's identical reasoning). */
    display_show_wait();

    ESP_LOGD(TAG, "[webui] waiting for TLS semaphore…");
    tls_sem_take();
    ESP_LOGD(TAG, "[webui] TLS semaphore acquired");

    /* Suspend competing background tasks for the duration of the download
     * and extraction — reduces network/flash contention and prevents WDT
     * triggers during the long LittleFS write phase. Resumed at all exit
     * points.
     *
     * Must run AFTER tls_sem_take() above, never before: ota_suspend_tasks()
     * suspends "net_poll", which independently takes this same tls_sem for
     * its own HTTPS fetches — if it ran first while net_poll held the
     * semaphore mid-handshake, vTaskSuspend would freeze it there without
     * releasing it, and our own tls_sem_take() would burn its full timeout
     * and proceed unserialized, reintroducing the fragmentation risk
     * tls_sem exists to prevent. */
    ota_suspend_tasks();
    tasks_suspended = true;

    char s_webui_ua[128];
    { char repo[64];
      resolve_update_repo(repo, sizeof(repo));
      snprintf(s_webui_ua, sizeof(s_webui_ua), HTTP_UA_BASE "%s", repo); }

    esp_http_client_handle_t client;
    int content_len;
    if (!hp_https_open(s_webui.url, s_webui_ua, OTA_PULL_TIMEOUT_MS, HP_MAX_ZIP,
                        &client, &content_len, s_webui.error, sizeof(s_webui.error), "webui"))
        goto webui_err_notls;
    ESP_LOGI(TAG, "[webui] download starting: %d B, heap free %lu B",
             content_len, (unsigned long)esp_get_free_heap_size());

    uint8_t *zip = (uint8_t *)heap_caps_malloc((size_t)content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!zip) {
        snprintf(s_webui.error, sizeof(s_webui.error), "OOM: can't buffer %d B", content_len);
        ESP_LOGE(TAG, "[webui] %s", s_webui.error);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        goto webui_err_notls;
    }
    ESP_LOGD(TAG, "[webui] buffer allocated, reading…");

    int received = hp_https_read_body(client, zip, content_len, NULL, "webui", OTA_PULL_TIMEOUT_MS);
    tls_sem_give();
    ESP_LOGI(TAG, "[webui] download complete: %d/%d B", received, content_len);

    if (received != content_len) {
        snprintf(s_webui.error, sizeof(s_webui.error),
                 "Short download: %d/%d B", received, content_len);
        ESP_LOGE(TAG, "[webui] %s", s_webui.error);
        free(zip); goto webui_err;
    }

    ESP_LOGI(TAG, "[webui] verifying SHA-256…");
    if (!sha256_verify(zip, (size_t)received, s_webui.sha256)) {
        snprintf(s_webui.error, sizeof(s_webui.error), "SHA-256 mismatch");
        ESP_LOGE(TAG, "[webui] SHA-256 FAILED");
        free(zip); goto webui_err;
    }
    ESP_LOGI(TAG, "[webui] SHA-256 OK%s", s_webui.sha256[0] ? "" : " (skipped — no hash)");

    {
        uint32_t sig0 = (uint32_t)(zip[0]|zip[1]<<8|zip[2]<<16|zip[3]<<24);
        ESP_LOGD(TAG, "[webui] ZIP magic: 0x%08" PRIx32 " (expect 0x%08" PRIx32 ")",
                 sig0, (uint32_t)ZIP_LFH_SIG);
    }
    if (received < 4 || (uint32_t)(zip[0]|zip[1]<<8|zip[2]<<16|zip[3]<<24) != ZIP_LFH_SIG) {
        snprintf(s_webui.error, sizeof(s_webui.error), "Not a valid ZIP");
        ESP_LOGE(TAG, "[webui] %s", s_webui.error);
        free(zip); goto webui_err;
    }
    ESP_LOGI(TAG, "[webui] ZIP valid — extracting to LittleFS…");

    {
        /* Invalidate the shell cache before any file is written so concurrent
         * requests get a cache-miss during extraction, not stale content. */
        shell_cache_flush();
        cJSON *files_arr = cJSON_CreateArray();
        int ok = 0, skipped = 0, failed = 0, unchanged = 0;

        /* Timed — see api_fs_hotpatch_impl's identical instrumentation
         * around its own call to this same function for why. */
        int64_t extract_t0 = esp_timer_get_time();
        extract_zip_to_littlefs(zip, (size_t)received, "[webui] ", /*success_at_info=*/false,
                                 files_arr, &ok, &skipped, &failed, &unchanged, NULL, true);
        int64_t extraction_ms = (esp_timer_get_time() - extract_t0) / 1000;

        /* Flatten into the same bounded, comma-joined shape
         * s_stock_repair.repaired_files uses — one consistent "which files
         * actually changed" answer across both repair/update status
         * endpoints, cheap to hold across a polled task's whole lifetime
         * (unlike keeping files_arr itself alive, which we don't need to). */
        size_t wf_len = 0;
        int n_files = cJSON_GetArraySize(files_arr);
        for (int fi = 0; fi < n_files; fi++) {
            const char *fn = cJSON_GetArrayItem(files_arr, fi)->valuestring;
            append_bounded_list(s_webui.written_files, sizeof(s_webui.written_files),
                                 &wf_len, fn, n_files - fi);
        }
        cJSON_Delete(files_arr);

        free(zip);
        hp_drop_stale_index();
        hp_drop_stale_marked();
        display_theme_cache_flush();   /* re-probe PNG format on next render */
        /* Async, via stock_files_check_spawn() — see the hotpatch handler's
         * identical call for why a direct call here would block this task's
         * own task-resume/display-resume below for the whole ~13-15s+ scan
         * instead of just the extraction time this log line reports, and
         * why the spawn helper (not a raw xTaskCreate) matters now that
         * this runs before tls_sem_give() releases the lock. */
        stock_files_check_spawn();
        ESP_LOGI(TAG, "[webui] extraction done: %d written, %d skipped, %d failed, %d unchanged, extraction took %lld ms",
                 ok, skipped, failed, unchanged, (long long)extraction_ms);
        if (failed > 0)
            snprintf(s_webui.error, sizeof(s_webui.error), "%d file(s) failed to write", failed);
    }

    /* Erase the stored URL from NVS so neither a future reboot nor a stray
     * browser call to api_webui_pull_auto re-downloads an already-applied
     * WebUI patch.                                                          */
    {
        nvs_handle_t nvs_h;
        if (nvs_open("nextube_sec", NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_erase_key(nvs_h, "webui_url");
            nvs_erase_key(nvs_h, "webui_sha256");
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
        }
    }
    if (tasks_suspended) webui_resume_tasks();
    /* This flow does NOT reboot on success (unlike every other
     * display_show_wait() caller) — un-park the display now, or the tubes
     * are stuck showing wait.jpg forever with no reboot ever coming to fix
     * it. */
    display_resume_after_wait();
    s_webui.state = WEBUI_DONE;
    s_ota_active  = false;
    ESP_LOGI(TAG, "[webui] state → DONE");
    ESP_LOGI(TAG, "[webui] stack high-water mark: %u B unused (of %u allocated)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)PERIODIC_NET_POLL_STACK_SIZE);
    return;

webui_err_notls:
    tls_sem_give();
webui_err:
    ESP_LOGE(TAG, "[webui] FAILED: %s", s_webui.error);
    if (tasks_suspended) webui_resume_tasks();
    /* Same reasoning as the success path — a failed pull doesn't reboot
     * either, so the display would otherwise be stuck on wait.jpg with no
     * automatic recovery at all. */
    display_resume_after_wait();
    s_webui.state = WEBUI_ERROR;
    s_ota_active  = false;
    ESP_LOGI(TAG, "[webui] stack high-water mark: %u B unused (of %u allocated)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)PERIODIC_NET_POLL_STACK_SIZE);
}

/* webui_pull_task writes to LittleFS (flash), so its stack must stay in internal RAM: it runs on the net_poll task, whose stack is
 * allocated once at boot. */
static bool start_webui_pull_task(void)
{
    return np_job_start(webui_pull_task);
}

static esp_err_t api_webui_pull_status(httpd_req_t *r)
{
    if (!post_ota_auth_valid()) { REQUIRE_AUTH(r); }
    /* 1024, not 768: GCC's -Werror=format-truncation worst case assumes
     * error[128] and written_files[512] both fill to their full declared
     * size — see api_stock_repair_status()'s matching comment, which hit
     * this same class of build failure at 768. */
    char buf[1024];
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"error\":\"%s\",\"written_files\":\"%s\"}",
             webui_state_name(), s_webui.error, s_webui.written_files);
    return send_json(r, buf);
}

/* ── LCD tuning preview (A/B compare) ─────────────────────────────────────────────────────────────────────────────────
 * POST /api/lcd_preview puts a set of per-tube LCD tuning values on the tubes WITHOUT saving them, so the web UI can flip between the saved
 * values (A) and its unsaved edits (B). {"revert":true} puts the saved values back. A preview also ends by itself after LCD_PREVIEW_MS, when
 * settings are saved (api_settings reverts first, then applies the new values as usual) and on reboot; nothing is written to flash.
 * s_lcd_applied tracks what the tubes currently have, so only the groups that differ are applied again (a profile change re-initialises
 * the panel, the others are cheap). */
#define LCD_PREVIEW_MS (5 * 60 * 1000)
static lcd_tuning_t       s_lcd_applied;
static bool               s_lcd_applied_valid = false;
static esp_timer_handle_t s_lcd_prev_timer = NULL;

static void lcd_applied_invalidate(void) { s_lcd_applied_valid = false; }

static void lcd_apply_diff(const lcd_tuning_t *w)
{
    if (!s_lcd_applied_valid) { config_get_lcd_tuning(&s_lcd_applied); s_lcd_applied_valid = true; }
    const lcd_tuning_t *a = &s_lcd_applied;
    if (w->lcd_invert_mask != a->lcd_invert_mask) display_apply_invert_mask(w->lcd_invert_mask);
    if (memcmp(w->lcd_init_profile, a->lcd_init_profile, sizeof(w->lcd_init_profile)) != 0) display_apply_init_profiles(w->lcd_init_profile);
    if (memcmp(w->lcd_vcom, a->lcd_vcom, sizeof(w->lcd_vcom)) != 0) display_apply_tube_vcom(w->lcd_vcom);
    bool gamma_changed = false;
    for (int i = 0; i < 6; i++) {
        if (fabsf(w->lcd_gamma[i] - a->lcd_gamma[i]) > 0.005f) { gamma_changed = true; break; }
    }
    if (gamma_changed) display_apply_tube_gamma(w->lcd_gamma);
    if (memcmp(w->lcd_col_offset, a->lcd_col_offset, sizeof(w->lcd_col_offset)) != 0 ||
        memcmp(w->lcd_row_offset, a->lcd_row_offset, sizeof(w->lcd_row_offset)) != 0)
        display_apply_tube_offsets(w->lcd_col_offset, w->lcd_row_offset);
    if (memcmp(w->lcd_tube_brightness, a->lcd_tube_brightness, sizeof(w->lcd_tube_brightness)) != 0)
        display_apply_tube_brightness(w->lcd_tube_brightness);
    if (memcmp(w->lcd_shadow_lift, a->lcd_shadow_lift, sizeof(w->lcd_shadow_lift)) != 0)
        display_apply_tube_shadow_lift(w->lcd_shadow_lift);
    if (memcmp(w->lcd_gain_r, a->lcd_gain_r, sizeof(w->lcd_gain_r)) != 0 ||
        memcmp(w->lcd_gain_g, a->lcd_gain_g, sizeof(w->lcd_gain_g)) != 0 ||
        memcmp(w->lcd_gain_b, a->lcd_gain_b, sizeof(w->lcd_gain_b)) != 0 ||
        memcmp(w->lcd_black_level, a->lcd_black_level, sizeof(w->lcd_black_level)) != 0)
        display_apply_tube_color(w->lcd_gain_r, w->lcd_gain_g, w->lcd_gain_b, w->lcd_black_level);
    if (memcmp(w->lcd_curve_r, a->lcd_curve_r, sizeof(w->lcd_curve_r)) != 0 ||
        memcmp(w->lcd_curve_g, a->lcd_curve_g, sizeof(w->lcd_curve_g)) != 0 ||
        memcmp(w->lcd_curve_b, a->lcd_curve_b, sizeof(w->lcd_curve_b)) != 0 ||
        memcmp(w->lcd_pedestal_r, a->lcd_pedestal_r, sizeof(w->lcd_pedestal_r)) != 0 ||
        memcmp(w->lcd_pedestal_g, a->lcd_pedestal_g, sizeof(w->lcd_pedestal_g)) != 0 ||
        memcmp(w->lcd_pedestal_b, a->lcd_pedestal_b, sizeof(w->lcd_pedestal_b)) != 0)
        display_apply_tube_channel_curves(w->lcd_curve_r, w->lcd_curve_g, w->lcd_curve_b,
                                          w->lcd_pedestal_r, w->lcd_pedestal_g, w->lcd_pedestal_b);
    s_lcd_applied = *w;
}

static void lcd_preview_revert(void)
{
    if (s_lcd_prev_timer) esp_timer_stop(s_lcd_prev_timer);
    if (!s_lcd_preview_pending) return;
    lcd_tuning_t saved;
    config_get_lcd_tuning(&saved);
    lcd_apply_diff(&saved);
    s_lcd_preview_pending = false;
    ESP_LOGI(TAG, "[lcd] preview ended - saved tuning restored");
}

static void lcd_prev_work(void *arg) { (void)arg; lcd_preview_revert(); }
/* esp_timer task: only queue the revert, the SPI work belongs on the httpd task like every other display apply. */
static void lcd_prev_timer_cb(void *arg) { (void)arg; if (s_server) httpd_queue_work(s_server, lcd_prev_work, NULL); }

static esp_err_t api_lcd_preview_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    return send_json(r, s_lcd_preview_pending ? "{\"preview\":true}" : "{\"preview\":false}");
}

static esp_err_t api_lcd_preview_post(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char body[1536];
    cJSON *root = read_json_body_small(r, body, sizeof(body));
    if (!root) return ESP_FAIL;
    const bool revert = cJSON_IsTrue(cJSON_GetObjectItem(root, "revert"));
    cJSON_Delete(root);
    if (revert) {
        lcd_preview_revert();
        return send_json(r, "{\"status\":\"ok\",\"preview\":false}");
    }
    if (s_ota_active) {
        httpd_resp_set_status(r, "409 Conflict");
        return send_json(r, "{\"error\":\"busy\"}");
    }
    lcd_tuning_t want;
    if (!config_parse_lcd_tuning(body, &want))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid body"), ESP_FAIL;
    lcd_apply_diff(&want);
    s_lcd_preview_pending = true;
    if (!s_lcd_prev_timer) {
        const esp_timer_create_args_t ta = { .callback = lcd_prev_timer_cb, .name = "lcd_preview" };
        if (esp_timer_create(&ta, &s_lcd_prev_timer) != ESP_OK) s_lcd_prev_timer = NULL;
    }
    if (s_lcd_prev_timer) {
        esp_timer_stop(s_lcd_prev_timer);
        esp_timer_start_once(s_lcd_prev_timer, (uint64_t)LCD_PREVIEW_MS * 1000ULL);
    }
    return send_json(r, "{\"status\":\"ok\",\"preview\":true}");
}

/* ── Theme Manager (downloadable theme packs) ───────────────────────────────
 * themes/catalog.json in the repo lists every theme pack (see themes/README.md); a pack is one STORE-only ZIP of
 * images/themes/<id>/. The catalog is fetched on request. A pack is installed the way webui_pull_task applies a WebUI update
 * (wait screen, TLS lock, background tasks suspended, ZIP buffered in PSRAM, extracted straight into LittleFS): the old folder
 * is moved aside first and restored if anything fails. An installed pack carries .pack.json (id, rev, content_sha256), so a higher
 * rev in the catalog reads as "update available". Removing a theme deletes its folder. */
#define TM_BRANCH          "main"
#define TM_CATALOG_SCHEMA  1
#define TM_MAX_CATALOG     65536     /* sanity cap on the download (the buffer is PSRAM): about 110 themes; guards against a wrong URL */
#define TM_MAX_PACK        (1024 * 1024)
#define TM_ID_MAX          31        /* config theme[32] */
#define TM_THEMES_DIR      "/spiffs/images/themes"

static int fs_remove_recursive(const char *path);   /* defined with the file-browser handlers below */
static esp_http_client_handle_t hp_stock_client_init(const char *user_agent, int timeout_ms);   /* defined with the stock repair */

typedef enum { TM_IDLE = 0, TM_RUNNING, TM_DONE, TM_ERROR } tm_state_t;
static struct {
    volatile tm_state_t state;
    bool         install;              /* false = refresh the catalog (or, with check, only verify what is installed) */
    bool         check;                /* verify the installed themes against their packs; no network access */
    bool         uninstall;            /* delete the theme folder s_tm.id (no network access); changed = 1 if the clock switched theme */
    int          changed;              /* themes that failed that check */
    char         id[TM_ID_MAX + 1];
    char         phase[16];
    volatile int progress;             /* 0-100 */
    char         error[96];
} s_tm;
#define TM_PREV_DIR     "/spiffs/web/theme-previews"   /* served as /theme-previews/<id>_r<rev>.jpg by the static handler */
#define TM_PREV_MAX     16384
#define TM_PREV_THEMES  64
static volatile bool s_tm_pending;      /* a job is queued for the shared network task */
static uint64_t s_tm_prev_mask;       /* bit i: catalog theme i has its preview cached on the clock */
static char    *s_tm_catalog;          /* raw catalog.json (PSRAM) from the last good refresh; only replaced by tm_job */
static time_t   s_tm_fetched;          /* when that refresh happened (epoch s), 0 = unknown */
static bool     s_tm_restored;         /* the saved copy was looked for once this boot */
#define TM_CATALOG_FILE "/spiffs/theme-catalog.json"   /* kept across reboots and updates: first line = fetch time, then the raw catalog */

static bool tm_id_valid(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n == 0 || n > TM_ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) return false;
    }
    return true;
}

/* "1.20.0" style: true when this firmware is at least `req` (empty = no requirement). */
static bool tm_fw_ok(const char *req)
{
    if (!req || !req[0]) return true;
    int a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
    sscanf(FW_VERSION_STR, "%d.%d.%d", &a[0], &a[1], &a[2]);
    sscanf(req, "%d.%d.%d", &b[0], &b[1], &b[2]);
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return true;
}

/* Installed = folder exists. *rev_out is the revision from its .pack.json, or 0 for a folder that did not come from a pack. */
static bool tm_installed(const char *id, int *rev_out)
{
    char path[96], mp[112];
    struct stat st;
    snprintf(path, sizeof(path), TM_THEMES_DIR "/%s", id);
    *rev_out = 0;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    snprintf(mp, sizeof(mp), "%s/.pack.json", path);
    FILE *f = fopen(mp, "rb");
    if (f) {
        char buf[256];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        cJSON *j = cJSON_Parse(buf);
        if (j) {
            cJSON *rv = cJSON_GetObjectItem(j, "rev");
            if (cJSON_IsNumber(rv)) *rev_out = rv->valueint;
            cJSON_Delete(j);
        }
    }
    return true;
}

static void tm_progress_cb(int received, int total)
{
    s_tm.progress = (received * 80) / total;   /* the rest is verifying and extracting */
}

/* ── Recognising themes that are already on the clock ─────────────────────────
 * A theme folder that came with the firmware has no .pack.json. After the catalog loads, each folder whose catalog entry has no marker
 * yet is fingerprinted and compared with the entry's content_sha256 (the value pack_theme.py computes: SHA-256 over, for every file in
 * sorted relative-path order, "<path>\0" + the size as 4 little-endian bytes + the file's own SHA-256; dot files are skipped). A match
 * gets the same .pack.json the pack would have written, so updates work from then on; a folder that differs stays unmarked and the
 * UI offers to replace it with the catalog version. A missing folder is simply a download option. */
#define TM_VERIFY_MAX_FILES 128
#define TM_REL_MAX          72

typedef struct { char rel[TM_REL_MAX]; } tm_rel_t;

static bool tm_collect(const char *dir, const char *rel_prefix, tm_rel_t *out, int *n)
{
    DIR *dp = opendir(dir);
    if (!dp) return false;
    bool ok = true;
    struct dirent *e;
    while (ok && (e = readdir(dp)) != NULL) {
        if (e->d_name[0] == '.' || strcmp(e->d_name, "Thumbs.db") == 0) continue;
        char path[192], rel[TM_REL_MAX];
        int pl = snprintf(path, sizeof(path), "%s/%.80s", dir, e->d_name);
        int rl = snprintf(rel, sizeof(rel), "%s%s%.60s", rel_prefix, rel_prefix[0] ? "/" : "", e->d_name);
        if (pl < 0 || pl >= (int)sizeof(path) || rl < 0 || rl >= (int)sizeof(rel)) { ok = false; break; }
        if (e->d_type == DT_DIR) {
            ok = tm_collect(path, rel, out, n);
        } else if (*n < TM_VERIFY_MAX_FILES) {
            memcpy(out[*n].rel, rel, sizeof(rel));
            (*n)++;
        } else {
            ok = false;
        }
    }
    closedir(dp);
    return ok;
}

static int tm_rel_cmp(const void *a, const void *b)
{
    return strcmp(((const tm_rel_t *)a)->rel, ((const tm_rel_t *)b)->rel);
}

/* Fingerprint of the theme folder `dir`; *nfiles gets the number of files hashed. */
static bool tm_content_hash(const char *dir, char hex[65], int *nfiles)
{
    tm_rel_t *list = heap_caps_malloc(TM_VERIFY_MAX_FILES * sizeof(tm_rel_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t  *buf  = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = list && buf;
    int n = 0;
    if (ok) ok = tm_collect(dir, "", list, &n);
    if (ok) qsort(list, (size_t)n, sizeof(tm_rel_t), tm_rel_cmp);
    mbedtls_sha256_context outer;
    mbedtls_sha256_init(&outer);
    mbedtls_sha256_starts(&outer, 0);
    for (int i = 0; ok && i < n; i++) {
        char path[192];
        if (snprintf(path, sizeof(path), "%s/%.71s", dir, list[i].rel) >= (int)sizeof(path)) { ok = false; break; }
        FILE *f = fopen(path, "rb");
        if (!f) { ok = false; break; }
        mbedtls_sha256_context inner;
        mbedtls_sha256_init(&inner);
        mbedtls_sha256_starts(&inner, 0);
        uint32_t total = 0;
        size_t got;
        while ((got = fread(buf, 1, 2048, f)) > 0) {
            mbedtls_sha256_update(&inner, buf, got);
            total += (uint32_t)got;
        }
        fclose(f);
        uint8_t digest[32], le[4] = { (uint8_t)total, (uint8_t)(total >> 8), (uint8_t)(total >> 16), (uint8_t)(total >> 24) };
        mbedtls_sha256_finish(&inner, digest);
        mbedtls_sha256_free(&inner);
        mbedtls_sha256_update(&outer, (const uint8_t *)list[i].rel, strlen(list[i].rel));
        mbedtls_sha256_update(&outer, (const uint8_t *)"", 1);
        mbedtls_sha256_update(&outer, le, 4);
        mbedtls_sha256_update(&outer, digest, 32);
        vTaskDelay(1);
    }
    uint8_t d[32];
    mbedtls_sha256_finish(&outer, d);
    mbedtls_sha256_free(&outer);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    *nfiles = n;
    free(list);
    free(buf);
    return ok;
}

/* The content hash recorded in a theme folder's .pack.json (what the pack contained when it was installed). */
static bool tm_marker_hash(const char *dir, char hex[65])
{
    char mp[112], buf[256];
    snprintf(mp, sizeof(mp), "%s/.pack.json", dir);
    FILE *f = fopen(mp, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *j = cJSON_Parse(buf);
    const char *h = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "content_sha256")) : NULL;
    bool ok = h && strlen(h) == 64;
    if (ok) strlcpy(hex, h, 65);
    cJSON_Delete(j);
    return ok;
}

/* For every catalog theme that is on the clock: fingerprint the folder. Without a marker, a match with the catalog writes its
 * .pack.json. With a marker, a folder that no longer matches what was installed (a file changed, deleted or added) loses the marker,
 * so it shows as "Differs from catalog" and can be replaced. One theme at a time with the display paused around the flash reads,
 * the way the stock-file scan does it. Folders with a marker are only re-hashed when `recheck` is set (the Verify installed button).
 * Returns how many themes were found to differ. */
static int tm_verify_installed(bool recheck)
{
    int differ = 0;
    cJSON *cat = s_tm_catalog ? cJSON_Parse(s_tm_catalog) : NULL;
    cJSON *themes = cat ? cJSON_GetObjectItem(cat, "themes") : NULL;
    cJSON *it;
    cJSON_ArrayForEach(it, themes) {
        const char *id  = cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
        const char *csh = cJSON_GetStringValue(cJSON_GetObjectItem(it, "content_sha256"));
        cJSON *rv = cJSON_GetObjectItem(it, "rev"), *fl = cJSON_GetObjectItem(it, "files");
        int inst_rev = 0;
        if (!tm_id_valid(id) || !csh || strlen(csh) != 64 || !cJSON_IsNumber(rv)) continue;
        if (!tm_installed(id, &inst_rev)) continue;
        if (inst_rev > 0 && !recheck) continue;   /* already marked: only the Verify installed button re-hashes it */
        bool pause_failed = false;
        {
            DISPLAY_PAUSE(pz, 5000);
            if (!pz.paused) {
                pause_failed = true;
            } else {
                char dir[96], hex[65] = "";
                int nfiles = 0;
                snprintf(dir, sizeof(dir), TM_THEMES_DIR "/%s", id);
                bool hashed = tm_content_hash(dir, hex, &nfiles);
                if (inst_rev > 0) {
                    char want[65] = "", mp[112];
                    if (hashed && tm_marker_hash(dir, want) && strcmp(hex, want) != 0) {
                        snprintf(mp, sizeof(mp), "%s/.pack.json", dir);
                        remove(mp);
                        differ++;
                        ESP_LOGW(TAG, "[themes] installed '%s' no longer matches its pack", id);
                    }
                } else {
                    bool match = hashed && strcmp(hex, csh) == 0 && (!cJSON_IsNumber(fl) || fl->valueint == nfiles);
                    if (match) {
                        char mp[112], js[160];
                        snprintf(mp, sizeof(mp), "%s/.pack.json", dir);
                        snprintf(js, sizeof(js), "{\"content_sha256\":\"%s\",\"files\":%d,\"id\":\"%s\",\"rev\":%d}", csh, nfiles, id, rv->valueint);
                        FILE *f = fopen(mp, "wb");
                        if (f) { fwrite(js, 1, strlen(js), f); fclose(f); }
                    }
                    if (!match) differ++;
                    ESP_LOGI(TAG, "[themes] installed '%s' %s the catalog", id, match ? "matches" : "differs from");
                }
            }
        }
        if (pause_failed) { ESP_LOGW(TAG, "[themes] display did not pause - skipping the check of installed themes"); break; }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    cJSON_Delete(cat);
    return differ;
}

/* ── Local copies of the theme previews ───────────────────────────────────────
 * The page shows each theme's digit strip. Right after a catalog refresh the clock downloads the strips it doesn't have yet into
 * /web/theme-previews/<id>_r<rev>.jpg (a new revision gets a new name, so the browser never shows a stale one) and deletes the ones no
 * catalog entry points at any more. The page then loads them from the clock, with the repository only as a fallback. */
typedef struct { char id[TM_ID_MAX + 1]; int rev; int pos; char rel[64]; char name[56]; uint8_t *data; int len; bool have; } tm_prev_t;   /* pos: index in the catalog */

static bool tm_fetch_small(esp_http_client_handle_t c, const char *url, uint8_t **out, int *len)
{
    if (esp_http_client_set_url(c, url) != ESP_OK || esp_http_client_open(c, 0) != ESP_OK) return false;
    int cl = esp_http_client_fetch_headers(c);
    int st = esp_http_client_get_status_code(c);
    if (st != 200 || cl <= 0 || cl > TM_PREV_MAX) { esp_http_client_close(c); return false; }
    uint8_t *b = heap_caps_malloc((size_t)cl, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) { esp_http_client_close(c); return false; }
    int got = 0;
    while (got < cl) {
        int n = esp_http_client_read(c, (char *)b + got, cl - got);
        if (n <= 0) break;
        got += n;
    }
    if (got != cl || b[0] != 0xFF || b[1] != 0xD8) { free(b); esp_http_client_close(c); return false; }   /* short read, or not a JPEG */
    *out = b;
    *len = cl;
    return true;
}

static void tm_cache_previews(const char *base, const char *ua)
{
    cJSON *cat = s_tm_catalog ? cJSON_Parse(s_tm_catalog) : NULL;
    cJSON *themes = cat ? cJSON_GetObjectItem(cat, "themes") : NULL;
    tm_prev_t *pv = heap_caps_calloc(TM_PREV_THEMES, sizeof(tm_prev_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pv) { cJSON_Delete(cat); return; }
    int n = 0, idx = 0, missing = 0;
    uint64_t mask = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, themes) {
        const int at = idx++;
        const char *id  = cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
        const char *rel = cJSON_GetStringValue(cJSON_GetObjectItem(it, "preview"));
        cJSON *rv = cJSON_GetObjectItem(it, "rev");
        if (at >= TM_PREV_THEMES || !tm_id_valid(id) || !rel || !cJSON_IsNumber(rv)) continue;
        size_t rl = strlen(rel);
        if (strncmp(rel, "previews/", 9) != 0 || rl < 14 || rl >= sizeof(pv[0].rel) || strcmp(rel + rl - 4, ".jpg") != 0 ||
            strstr(rel, "..") || strstr(rel, "//")) continue;
        tm_prev_t *e = &pv[n];
        strlcpy(e->id, id, sizeof(e->id));
        strlcpy(e->rel, rel, sizeof(e->rel));
        e->rev = rv->valueint;
        snprintf(e->name, sizeof(e->name), "%.31s_r%d.jpg", e->id, e->rev);
        e->pos = at;
        n++;
    }
    cJSON_Delete(cat);

    /* Which are already on the clock? */
    if (display_pause_for_spi(5000)) {
        for (int i = 0; i < n; i++) {
            char path[128];
            struct stat st;
            snprintf(path, sizeof(path), TM_PREV_DIR "/%s", pv[i].name);
            pv[i].have = (stat(path, &st) == 0 && st.st_size > 0);
            if (!pv[i].have) missing++;
        }
        display_unpause();
    } else {
        missing = 0;    /* could not pause the display; try again at the next refresh */
    }

    /* Download what is missing over one connection. */
    char *url = missing > 0 ? heap_caps_malloc(256, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (missing > 0 && url) {
        tls_sem_take();
        esp_http_client_handle_t c = hp_stock_client_init(ua, 30000);
        if (c) {
            for (int i = 0; i < n; i++) {
                if (pv[i].have) continue;
                if (snprintf(url, 256, "%s%s", base, pv[i].rel) >= 256) continue;
                int len = 0;
                if (tm_fetch_small(c, url, &pv[i].data, &len)) pv[i].len = len;
            }
            esp_http_client_cleanup(c);
        }
        tls_sem_give();
    }
    free(url);

    /* Write the new ones and remove every cached file that no catalog entry names. */
    if (display_pause_for_spi(5000)) {
        hp_mkdir_p(TM_PREV_DIR "/x");
        DIR *dp = opendir(TM_PREV_DIR);
        if (dp) {
            struct dirent *de;
            while ((de = readdir(dp)) != NULL) {
                bool wanted = false;
                for (int i = 0; i < n && !wanted; i++) wanted = (strcmp(de->d_name, pv[i].name) == 0);
                if (!wanted && de->d_name[0] != '.') {
                    char path[160];
                    if (snprintf(path, sizeof(path), TM_PREV_DIR "/%.80s", de->d_name) < (int)sizeof(path)) remove(path);
                }
            }
            closedir(dp);
        }
        for (int i = 0; i < n; i++) {
            if (!pv[i].have && pv[i].data) {
                char path[128];
                snprintf(path, sizeof(path), TM_PREV_DIR "/%s", pv[i].name);
                FILE *f = fopen(path, "wb");
                if (f) {
                    pv[i].have = (fwrite(pv[i].data, 1, (size_t)pv[i].len, f) == (size_t)pv[i].len);
                    fclose(f);
                    if (!pv[i].have) remove(path);
                }
            }
        }
        display_unpause();
    }
    for (int i = 0; i < n; i++) {
        if (pv[i].have) mask |= ((uint64_t)1 << pv[i].pos);
        free(pv[i].data);
    }
    s_tm_prev_mask = mask;
    free(pv);
}

/* Which catalog themes already have their preview file on the clock (the files outlive a reboot, the mask does not). */
static void tm_prev_scan(void)
{
    cJSON *cat = s_tm_catalog ? cJSON_Parse(s_tm_catalog) : NULL;
    uint64_t mask = 0;
    if (cat) {
        DISPLAY_PAUSE(pz, 5000);
        if (pz.paused) {
            int idx = 0;
            cJSON *it;
            cJSON_ArrayForEach(it, cJSON_GetObjectItem(cat, "themes")) {
                const int at = idx++;
                const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
                cJSON *rv = cJSON_GetObjectItem(it, "rev");
                char path[128];
                struct stat st;
                if (at >= TM_PREV_THEMES || !tm_id_valid(id) || !cJSON_IsNumber(rv)) continue;
                snprintf(path, sizeof(path), TM_PREV_DIR "/%.31s_r%d.jpg", id, rv->valueint);
                if (stat(path, &st) == 0 && st.st_size > 0) mask |= ((uint64_t)1 << at);
            }
        }
    }
    cJSON_Delete(cat);
    s_tm_prev_mask = mask;
}

/* After a good refresh: keep the catalog on flash so a reboot or firmware update does not lose it. */
static void tm_save_catalog(void)
{
    if (!s_tm_catalog) return;
    DISPLAY_PAUSE(pz, 5000);
    if (!pz.paused) return;
    const char *tmp = TM_CATALOG_FILE ".tmp";
    size_t len = strlen(s_tm_catalog);
    bool ok = false;
    FILE *f = fopen(tmp, "wb");
    if (f) {
        ok = fprintf(f, "%lld\n", (long long)s_tm_fetched) > 0 && fwrite(s_tm_catalog, 1, len, f) == len;
        if (fclose(f) != 0) ok = false;
    }
    if (!ok || rename(tmp, TM_CATALOG_FILE) != 0) {
        remove(tmp);
        ESP_LOGW(TAG, "[themes] could not save the catalog");
    }
}

/* First use after boot: bring the saved catalog back (only if no refresh has loaded one) and re-check the cached previews. */
static void tm_restore_catalog(void)
{
    if (s_tm_restored || s_tm_catalog || s_tm.state == TM_RUNNING) return;
    s_tm_restored = true;
    char *buf = NULL;
    {
        DISPLAY_PAUSE(pz, 5000);
        if (!pz.paused) { s_tm_restored = false; return; }
        FILE *f = fopen(TM_CATALOG_FILE, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 2 && sz <= TM_MAX_CATALOG + 32 && (buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) != NULL) {
                if (fread(buf, 1, (size_t)sz, f) == (size_t)sz) buf[sz] = '\0';
                else { free(buf); buf = NULL; }
            }
            fclose(f);
        }
    }
    if (!buf) return;
    char *body = strchr(buf, '\n');
    cJSON *cat = body ? cJSON_Parse(body + 1) : NULL;
    cJSON *sc = cat ? cJSON_GetObjectItem(cat, "schema") : NULL;
    if (!cat || !cJSON_IsArray(cJSON_GetObjectItem(cat, "themes")) || (cJSON_IsNumber(sc) && sc->valueint > TM_CATALOG_SCHEMA)) {
        cJSON_Delete(cat);
        free(buf);
        return;
    }
    cJSON_Delete(cat);
    s_tm_fetched = (time_t)atoll(buf);
    memmove(buf, body + 1, strlen(body + 1) + 1);
    s_tm_catalog = buf;
    tm_prev_scan();
    ESP_LOGI(TAG, "[themes] restored the saved catalog");
}

/* Removes the theme folder s_tm.id one file at a time so the page can show a progress bar. If it is the active theme the clock first
 * switches to WeatherLive (drawn by the firmware, needs no files) and waits a moment for the display task to leave the folder. The
 * display is paused around batches of deletes, as around every other flash write here. */
static void tm_do_remove(void)
{
    char active[32], dir[96];
    config_lock();
    strlcpy(active, config_get()->theme, sizeof(active));
    config_unlock();
    if (strcmp(active, s_tm.id) == 0) {
        const char *fallback = "{\"apps\":[{\"theme\":\"WeatherLive\"}]}";
        config_set_json(fallback, strlen(fallback));
        display_config_changed();
        vTaskDelay(pdMS_TO_TICKS(600));
        s_tm.changed = 1;
    }
    snprintf(dir, sizeof(dir), TM_THEMES_DIR "/%s", s_tm.id);
    tm_rel_t *list = heap_caps_malloc(TM_VERIFY_MAX_FILES * sizeof(tm_rel_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int n = 0;
    if (!list || !tm_collect(dir, "", list, &n)) n = 0;   /* too many files or no memory: the sweep below still removes everything */
    bool paused = false;
    for (int i = 0; i < n; i++) {
        char path[192];
        if (!paused) paused = display_pause_for_spi(2000);
        if (snprintf(path, sizeof(path), "%s/%.71s", dir, list[i].rel) < (int)sizeof(path)) remove(path);
        s_tm.progress = (i + 1) * 90 / n;
        if ((i & 7) == 7) {
            if (paused) display_unpause();
            paused = false;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (!paused) paused = display_pause_for_spi(2000);
    const int rc = fs_remove_recursive(dir);   /* what is left: the folders and the marker */
    if (paused) display_unpause();
    free(list);
    fs_usage_invalidate();
    display_theme_cache_flush();
    if (rc != 0) snprintf(s_tm.error, sizeof(s_tm.error), "Delete failed");
}

/* One task for both operations: refresh the catalog, or download + install one pack (s_tm.install / s_tm.id). */
/* Runs on the shared network task (see np_job_tick), whose stack is allocated once at boot: the internal heap is far too fragmented
 * to create a new task of this size mid-uptime. The long strings live in one PSRAM block to keep this frame small. */
typedef struct { char repo[64], ua[128], base[160], url[256], zip_rel[96], sha[65]; } tm_strs_t;

static void tm_job(void)
{
    const bool install = s_tm.install;
    const bool check = s_tm.check;
    const bool uninstall = s_tm.uninstall;
    bool parked = false, suspended = false, have_tls = false, ok = false, was_spectrum = false;
    uint8_t *buf = NULL;
    tm_strs_t *ts = heap_caps_calloc(1, sizeof(*ts), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int want_size = 0, timeout_ms = install ? OTA_PULL_TIMEOUT_MS : 30000;
    esp_http_client_handle_t client = NULL;
    int clen = 0, got = 0;

    s_tm.state = TM_RUNNING;
    s_tm.progress = 0;
    s_tm.error[0] = '\0';
    s_tm.changed = 0;
    strlcpy(s_tm.phase, install ? "download" : uninstall ? "removing" : check ? "checking" : "catalog", sizeof(s_tm.phase));

    if (!ts) {
        snprintf(s_tm.error, sizeof(s_tm.error), "Out of memory");
        goto done;
    }
    if (s_stock_scan_active) {      /* cannot normally happen (tm_start refuses, spawns wait), but never overlap */
        snprintf(s_tm.error, sizeof(s_tm.error), "A stock-file check is running - try again in a moment");
        goto done;
    }
    if (uninstall) {                       /* local only, like the check below */
        tm_do_remove();
        ok = !s_tm.error[0];
        goto done;
    }
    if (check) {                           /* local only: no network, no clock time needed */
        s_tm.changed = tm_verify_installed(true);
        ok = true;
        goto done;
    }
    if (!ntp_has_valid_time()) {
        snprintf(s_tm.error, sizeof(s_tm.error), "Clock time not synced yet");
        goto done;
    }
    resolve_update_repo(ts->repo, sizeof(ts->repo));
    snprintf(ts->ua, sizeof(ts->ua), HTTP_UA_BASE "%s", ts->repo);
    snprintf(ts->base, sizeof(ts->base), "https://raw.githubusercontent.com/%s/" TM_BRANCH "/themes/", ts->repo);

    if (install) {
        cJSON *cat = s_tm_catalog ? cJSON_Parse(s_tm_catalog) : NULL;
        cJSON *themes = cat ? cJSON_GetObjectItem(cat, "themes") : NULL;
        cJSON *it;
        cJSON_ArrayForEach(it, themes) {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
            if (!id || strcmp(id, s_tm.id) != 0) continue;
            const char *z = cJSON_GetStringValue(cJSON_GetObjectItem(it, "zip"));
            const char *h = cJSON_GetStringValue(cJSON_GetObjectItem(it, "sha256"));
            cJSON *sz = cJSON_GetObjectItem(it, "size");
            if (z) strlcpy(ts->zip_rel, z, sizeof(ts->zip_rel));
            if (h) strlcpy(ts->sha, h, sizeof(ts->sha));
            if (cJSON_IsNumber(sz)) want_size = sz->valueint;
            break;
        }
        cJSON_Delete(cat);
        if (strncmp(ts->zip_rel, "packs/", 6) != 0 || strstr(ts->zip_rel, "..") || strstr(ts->zip_rel, "//") || strlen(ts->sha) != 64) {
            snprintf(s_tm.error, sizeof(s_tm.error), "Catalog entry for '%.31s' is incomplete", s_tm.id);
            goto done;
        }
        display_show_wait();
        parked = true;
    }
    snprintf(ts->url, sizeof(ts->url), "%s%s", ts->base, install ? ts->zip_rel : "catalog.json");

    tls_sem_take();
    have_tls = true;
    if (install) {
        /* A catalog refresh is one small fetch under the TLS lock, like the update check. An install writes flash, so it
         * suspends the background tasks (which also switches Spectrum to Clock; put it back afterwards). */
        config_lock();
        was_spectrum = (config_get()->current_mode == APP_MODE_SPECTRUM);
        config_unlock();
        ota_suspend_tasks();
        suspended = true;
    }

    if (!hp_https_open(ts->url, ts->ua, timeout_ms, install ? TM_MAX_PACK : TM_MAX_CATALOG, &client, &clen,
                       s_tm.error, sizeof(s_tm.error), "themes"))
        goto done;
    if (install && want_size > 0 && clen != want_size) {
        esp_http_client_close(client); esp_http_client_cleanup(client);
        snprintf(s_tm.error, sizeof(s_tm.error), "Download size differs from the catalog");
        goto done;
    }
    buf = heap_caps_malloc((size_t)clen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        esp_http_client_close(client); esp_http_client_cleanup(client);
        snprintf(s_tm.error, sizeof(s_tm.error), "Out of memory for %d B", clen);
        goto done;
    }
    got = hp_https_read_body(client, buf, clen, install ? tm_progress_cb : NULL, "themes", timeout_ms);
    tls_sem_give();
    have_tls = false;
    if (got != clen) {
        snprintf(s_tm.error, sizeof(s_tm.error), "Short download: %d/%d B", got, clen);
        goto done;
    }
    buf[clen] = '\0';

    if (!install) {
        cJSON *cat = cJSON_Parse((const char *)buf);
        cJSON *sc = cat ? cJSON_GetObjectItem(cat, "schema") : NULL;
        if (!cat || !cJSON_IsArray(cJSON_GetObjectItem(cat, "themes"))) {
            snprintf(s_tm.error, sizeof(s_tm.error), "Catalog is not valid JSON");
        } else if (cJSON_IsNumber(sc) && sc->valueint > TM_CATALOG_SCHEMA) {
            snprintf(s_tm.error, sizeof(s_tm.error), "Catalog needs a newer firmware");
        } else {
            char *old = s_tm_catalog;
            s_tm_catalog = (char *)buf;
            buf = NULL;
            s_tm_fetched = time(NULL);
            free(old);
            ok = true;
        }
        cJSON_Delete(cat);
        if (ok) {
            strlcpy(s_tm.phase, "checking", sizeof(s_tm.phase));
            tm_verify_installed(false);
            strlcpy(s_tm.phase, "catalog", sizeof(s_tm.phase));   /* the previews belong to the catalog update */
            tm_cache_previews(ts->base, ts->ua);
            tm_save_catalog();
        }
        goto done;
    }

    strlcpy(s_tm.phase, "verify", sizeof(s_tm.phase));
    s_tm.progress = 82;
    if (!sha256_verify(buf, (size_t)clen, ts->sha)) {
        snprintf(s_tm.error, sizeof(s_tm.error), "SHA-256 mismatch");
        goto done;
    }
    if (clen < 4 || (uint32_t)(buf[0] | buf[1] << 8 | buf[2] << 16 | buf[3] << 24) != ZIP_LFH_SIG) {
        snprintf(s_tm.error, sizeof(s_tm.error), "Not a valid ZIP");
        goto done;
    }

    strlcpy(s_tm.phase, "extract", sizeof(s_tm.phase));
    s_tm.progress = 85;
    {
        char dst[96], old[96], marker[112], prefix[TM_ID_MAX + 20];
        struct stat st;
        snprintf(dst, sizeof(dst), TM_THEMES_DIR "/%s", s_tm.id);
        snprintf(old, sizeof(old), TM_THEMES_DIR "/.old_%s", s_tm.id);
        snprintf(marker, sizeof(marker), "%s/.pack.json", dst);
        snprintf(prefix, sizeof(prefix), "images/themes/%s/", s_tm.id);

        fs_remove_recursive(old);   /* leftover from an interrupted install */
        bool had_old = (stat(dst, &st) == 0);
        if (had_old && rename(dst, old) != 0) {
            snprintf(s_tm.error, sizeof(s_tm.error), "Could not move the current theme aside");
            goto done;
        }
        shell_cache_flush();
        int n_ok = 0, n_skip = 0, n_fail = 0, n_same = 0;
        extract_zip_to_littlefs(buf, (size_t)clen, "[themes] ", /*success_at_info=*/false, NULL,
                                &n_ok, &n_skip, &n_fail, &n_same, prefix, /*honor_exceptions=*/false);
        if (n_fail == 0 && n_ok > 0 && stat(marker, &st) == 0) {
            if (had_old) fs_remove_recursive(old);
            ok = true;
            ESP_LOGI(TAG, "[themes] installed '%s': %d files", s_tm.id, n_ok);
        } else {
            fs_remove_recursive(dst);
            if (had_old) rename(old, dst);
            if (n_fail) snprintf(s_tm.error, sizeof(s_tm.error), "%d file(s) failed to write", n_fail);
            else        snprintf(s_tm.error, sizeof(s_tm.error), "Pack has no files");
        }
        free(buf);
        buf = NULL;
        display_theme_cache_flush();
        fs_usage_invalidate();
        display_invalidate();
    }

done:
    free(buf);
    if (have_tls) tls_sem_give();
    if (suspended) webui_resume_tasks();
    if (parked) display_resume_after_wait();
    if (was_spectrum) config_set_mode(APP_MODE_SPECTRUM);
    if (!ok && !s_tm.error[0]) snprintf(s_tm.error, sizeof(s_tm.error), "Failed");
    if (!ok) ESP_LOGE(TAG, "[themes] %s failed: %s", install ? "install" : uninstall ? "remove" : "refresh", s_tm.error);
    s_tm.progress = ok ? 100 : s_tm.progress;
    s_tm.state = ok ? TM_DONE : TM_ERROR;
    s_ota_active = false;
    s_tm_busy = false;
    if (s_stock_spawn_deferred) { s_stock_spawn_deferred = false; stock_files_check_spawn(); }   /* a scan asked for meanwhile */
    free(ts);
    ESP_LOGI(TAG, "[themes] net_poll stack high-water mark: %u B unused", (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

/* Once a day the catalog refreshes by itself (while the automatic update check is on), so a theme update is known without anyone
 * pressing Check for themes. The saved fetch time counts across reboots, the first look waits out the boot, and a failed attempt is
 * retried after a couple of hours instead of at the next wake-up. */
#define TM_AUTO_INTERVAL_S    (24 * 3600)
#define TM_AUTO_RETRY_S       (2 * 3600)
#define TM_AUTO_BOOT_GRACE_US (180LL * 1000000)
#define TM_AUTO_POLL_MS       (15U * 60U * 1000U)
static int64_t s_tm_auto_after_us;

static void tm_auto_check(void)
{
    const int64_t now_us = esp_timer_get_time();
    bool enabled;
    config_lock();
    enabled = config_get()->update_check_enabled;
    config_unlock();
    if (!enabled || now_us < TM_AUTO_BOOT_GRACE_US || now_us < s_tm_auto_after_us) return;
    if (s_ota_active || s_tm.state == TM_RUNNING || s_stock_scan_active || !ntp_has_valid_time()) return;
    tm_restore_catalog();
    const time_t now = time(NULL);
    if (s_tm_catalog && s_tm_fetched > 0 && now >= s_tm_fetched && now - s_tm_fetched < TM_AUTO_INTERVAL_S) {
        s_tm_auto_after_us = now_us + (int64_t)(TM_AUTO_INTERVAL_S - (now - s_tm_fetched)) * 1000000;
        return;
    }
    ESP_LOGI(TAG, "[themes] daily catalog check");
    s_tm_busy = true;
    s_tm.install = false;
    s_tm.check = false;
    s_tm.uninstall = false;
    s_tm.id[0] = '\0';
    s_tm.state = TM_RUNNING;
    s_tm.progress = 0;
    s_tm.error[0] = '\0';
    s_ota_active = true;
    strlcpy(s_tm.phase, "queued", sizeof(s_tm.phase));
    tm_job();
    s_tm_auto_after_us = esp_timer_get_time() + (int64_t)(s_tm.state == TM_DONE ? TM_AUTO_INTERVAL_S : TM_AUTO_RETRY_S) * 1000000;
}

/* ── Automatic firmware update ────────────────────────────────────────────────────────────────────────────
 * Settings: config auto_update_* (System tab). Once a day, inside a 2-hour window
 * starting at auto_update_hour (local time), the clock reads the newest release from GitHub (the same endpoints as the update check:
 * /releases/latest, or /releases?per_page=1 with pre-releases when update_repo is overridden), applies the policy (same major.minor
 * only, or any stable release), and hands the firmware and Web UI assets with their SHA-256 digests to the same ota_pull_task() the
 * Online Updater drives; the Web UI pull after the reboot is the existing post-OTA one. A
 * version that was rolled back (pb_failed) or failed AU_MAX_TRIES times is skipped. */
#define AU_NS            "nextube_diag"
#define AU_WINDOW_H      2
#define AU_MIN_UPTIME_US (600LL * 1000000)
#define AU_RECHECK_US    (6LL * 3600 * 1000000)
#define AU_MAX_TRIES     2
#define AU_API_MAX       (96 * 1024)
#define AU_DEFAULT_REPO  "MrToast99/Nextube-Remaster"

static struct {
    volatile bool busy;
    time_t        last;            /* last look at GitHub (epoch s), 0 = none since boot */
    char          result[120];
    char          latest[24];
    char          updated_to[24];  /* set when this boot followed an automatic update ... */
    char          updated_from[24];/* ... and the version it replaced */
} s_au;
static int64_t s_au_next_us;
static volatile bool s_au_force_install;

static bool au_ver(const char *s, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    if (s && (*s == 'v' || *s == 'V')) s++;
    return s && sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) >= 2;
}

static int au_cmp(const int a[3], const int b[3])
{
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static bool au_nvs_str(const char *key, char *out, size_t n)
{
    nvs_handle_t h;
    bool ok = false;
    out[0] = '\0';
    if (nvs_open(AU_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = n;
        ok = nvs_get_str(h, key, out, &len) == ESP_OK;
        nvs_close(h);
    }
    return ok;
}

static void au_nvs_set_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(AU_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

static uint8_t au_nvs_u8(const char *key)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(AU_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v;
}

static void au_nvs_set_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(AU_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* The release JSON (PSRAM, NUL-terminated), or NULL with s_au.result explaining why. */
static char *au_fetch_release(const char *repo, bool overridden)
{
    char ua[128], url[160];
    snprintf(ua, sizeof(ua), HTTP_UA_BASE "%s", repo);
    snprintf(url, sizeof(url), overridden ? "https://api.github.com/repos/%s/releases?per_page=1"
                                          : "https://api.github.com/repos/%s/releases/latest", repo);
    char *buf = NULL;
    TLS_LOCK(tls);
    esp_http_client_handle_t c = hp_stock_client_init(ua, 15000);
    if (!c) {
        snprintf(s_au.result, sizeof(s_au.result), "could not start the HTTPS client");
    } else if (esp_http_client_set_url(c, url) != ESP_OK || esp_http_client_open(c, 0) != ESP_OK) {
        snprintf(s_au.result, sizeof(s_au.result), "could not reach GitHub");
    } else {
        esp_http_client_fetch_headers(c);
        const int st = esp_http_client_get_status_code(c);
        if (st != 200) {
            snprintf(s_au.result, sizeof(s_au.result), "GitHub answered HTTP %d%s", st, st == 404 ? " (private repository or no release)" : "");
        } else {
            buf = heap_caps_malloc(AU_API_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            int got = 0;
            while (buf && got < AU_API_MAX) {
                const int n = esp_http_client_read(c, buf + got, AU_API_MAX - got);
                if (n <= 0) break;
                got += n;
            }
            if (!buf) {
                snprintf(s_au.result, sizeof(s_au.result), "out of memory");
            } else if (got <= 0 || got >= AU_API_MAX) {
                snprintf(s_au.result, sizeof(s_au.result), got <= 0 ? "empty answer from GitHub" : "release data too large");
                free(buf);
                buf = NULL;
            } else {
                buf[got] = '\0';
            }
        }
        esp_http_client_close(c);
    }
    if (c) esp_http_client_cleanup(c);
    return buf;
}

/* One look at GitHub. force = from the Check now button (ignores the window, the switch and the attempt limit); install = really install
 * (otherwise only record what would happen). On a real install this only returns if it failed: the clock reboots on success. */
static void au_run(bool force, bool install)
{
    s_au.busy = true;
    s_au.last = time(NULL);
    s_au.result[0] = '\0';
    char repo[64], ver[24] = "", fw_sha[65] = "", ui_sha[65] = "";
    char *urls = heap_caps_calloc(1, sizeof(s_pull.url) + sizeof(s_pull.webui_url), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *fw_url = urls, *ui_url = urls ? urls + sizeof(s_pull.url) : NULL;
    uint8_t policy;
    config_lock();
    policy = config_get()->auto_update_policy;
    config_unlock();
    resolve_update_repo(repo, sizeof(repo));
    const bool overridden = strcmp(repo, AU_DEFAULT_REPO) != 0;

    char *body = urls ? au_fetch_release(repo, overridden) : NULL;
    if (!urls) snprintf(s_au.result, sizeof(s_au.result), "out of memory");
    const bool have_body = body != NULL;
    cJSON *root = have_body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!have_body && !s_au.result[0]) snprintf(s_au.result, sizeof(s_au.result), "no answer");
    else if (have_body && !root) snprintf(s_au.result, sizeof(s_au.result), "could not read the release data");
    cJSON *rel = root ? (cJSON_IsArray(root) ? cJSON_GetArrayItem(root, 0) : root) : NULL;
    const char *tag = rel ? cJSON_GetStringValue(cJSON_GetObjectItem(rel, "tag_name")) : NULL;
    if (root && !tag) snprintf(s_au.result, sizeof(s_au.result), "no release found");
    if (tag) {
        int cur[3], lat[3];
        au_ver(FW_VERSION_STR, cur);
        const bool ok_ver = au_ver(tag, lat);
        strlcpy(ver, tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag, sizeof(ver));
        strlcpy(s_au.latest, ver, sizeof(s_au.latest));
        char failed[24], tried[24];
        au_nvs_str("pb_failed", failed, sizeof(failed));
        const bool tried_same = au_nvs_str("au_try", tried, sizeof(tried)) && strcmp(tried, ver) == 0;
        if (!ok_ver) snprintf(s_au.result, sizeof(s_au.result), "cannot read the version '%.20s'", tag);
        else if ((cJSON_IsTrue(cJSON_GetObjectItem(rel, "prerelease")) && !overridden) || cJSON_IsTrue(cJSON_GetObjectItem(rel, "draft")))
            snprintf(s_au.result, sizeof(s_au.result), "newest release v%s is a pre-release: skipped", ver);
        else if (au_cmp(lat, cur) <= 0)
            snprintf(s_au.result, sizeof(s_au.result), "up to date (running v%s, latest v%s)", FW_VERSION_STR, ver);
        else if (policy == 0 && (lat[0] != cur[0] || lat[1] != cur[1]))
            snprintf(s_au.result, sizeof(s_au.result), "v%s is a larger step than a patch release: not installed automatically", ver);
        else if (failed[0] && strcmp(failed, ver) == 0)
            snprintf(s_au.result, sizeof(s_au.result), "v%s was rolled back before: skipped", ver);
        else if (!force && tried_same && au_nvs_u8("au_tries") >= AU_MAX_TRIES)
            snprintf(s_au.result, sizeof(s_au.result), "v%s failed %d times: skipped", ver, AU_MAX_TRIES);
        else {
            const char *fu = NULL, *fs = NULL, *uu = NULL, *us = NULL;
            cJSON *a;
            cJSON_ArrayForEach(a, cJSON_GetObjectItem(rel, "assets")) {
                const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(a, "name"));
                const char *url  = cJSON_GetStringValue(cJSON_GetObjectItem(a, "browser_download_url"));
                const char *dig  = cJSON_GetStringValue(cJSON_GetObjectItem(a, "digest"));
                if (!name || !url) continue;
                dig = (dig && strncmp(dig, "sha256:", 7) == 0 && strlen(dig) == 71) ? dig + 7 : NULL;
                const size_t nl = strlen(name);
                if (strncmp(name, "nextube-fw-", 11) == 0 && nl > 8 && strcmp(name + nl - 8, "-ota.bin") == 0) { fu = url; fs = dig; }
                else if (strncmp(name, "nextube-WebUI-", 14) == 0 && nl > 4 && strcmp(name + nl - 4, ".zip") == 0) { uu = url; us = dig; }
            }
            char want[120];
            snprintf(want, sizeof(want), "https://github.com/%.80s/releases/download/", repo);
            const size_t wl = strlen(want);
            if (!fu || !fs || !uu || !us)
                snprintf(s_au.result, sizeof(s_au.result), "release v%s is incomplete (%s missing): not installed", ver,
                         !fu ? "firmware file" : !fs ? "firmware hash" : !uu ? "Web UI file" : "Web UI hash");
            else if (strncmp(fu, want, wl) != 0 || strncmp(uu, want, wl) != 0 || strlen(fu) >= sizeof(s_pull.url) || strlen(uu) >= sizeof(s_pull.webui_url))
                snprintf(s_au.result, sizeof(s_au.result), "release v%s has unexpected download addresses: not installed", ver);
            else {
                strlcpy(fw_url, fu, sizeof(s_pull.url)); strlcpy(fw_sha, fs, sizeof(fw_sha));
                strlcpy(ui_url, uu, sizeof(s_pull.webui_url)); strlcpy(ui_sha, us, sizeof(ui_sha));
                snprintf(s_au.result, sizeof(s_au.result), install ? "installing v%s" : "v%s is available", ver);
            }
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "[auto-update] %s", s_au.result);

    if (install && fw_url && fw_url[0]) {
        char tried[24];
        const bool tried_same = au_nvs_str("au_try", tried, sizeof(tried)) && strcmp(tried, ver) == 0;
        au_nvs_set_str("au_try", ver);
        au_nvs_set_u8("au_tries", (uint8_t)((tried_same ? au_nvs_u8("au_tries") : 0) + 1));
        au_nvs_set_str("au_done", ver);
        au_nvs_set_str("au_from", FW_VERSION_STR);
        snprintf(s_pull.url, sizeof(s_pull.url), "%s", fw_url);
        snprintf(s_pull.sha256, sizeof(s_pull.sha256), "%s", fw_sha);
        snprintf(s_pull.webui_url, sizeof(s_pull.webui_url), "%s", ui_url);
        snprintf(s_pull.webui_sha256, sizeof(s_pull.webui_sha256), "%s", ui_sha);
        s_pull.state = OTA_PULL_IDLE;
        s_pull.progress = 0;
        s_pull.bytes_received = s_pull.bytes_total = s_pull.bytes_flashed = 0;
        s_pull.error[0] = '\0';
        free(urls);
        urls = NULL;
        s_ota_active = true;
        ota_pull_task(NULL);   /* returns only when the update failed */
        au_nvs_set_str("au_done", "");
        snprintf(s_au.result, sizeof(s_au.result), "install of v%s failed: %.70s", ver, s_pull.error);
        ESP_LOGW(TAG, "[auto-update] %s", s_au.result);
    }
    free(urls);
    s_au.busy = false;
}

static void au_forced_job(void *arg)
{
    (void)arg;
    au_run(true, s_au_force_install);
}

/* Runs on the shared network task every wake-up: starts a look when it is inside the window and nothing else is going on. */
static void au_tick(void)
{
    bool enabled;
    uint8_t hour;
    config_lock();
    enabled = config_get()->auto_update_enabled;
    hour = config_get()->auto_update_hour;
    config_unlock();
    const int64_t now_us = esp_timer_get_time();
    if (!enabled || now_us < AU_MIN_UPTIME_US || now_us < s_au_next_us) return;
    if (s_ota_active || s_tm.state == TM_RUNNING || s_stock_scan_active || s_au.busy || !ntp_has_valid_time()) return;
    struct tm lt;
    ntp_get_local(&lt);
    if (((lt.tm_hour - (int)hour + 24) % 24) >= AU_WINDOW_H) return;
    s_au_next_us = now_us + AU_RECHECK_US;
    au_run(false, true);
}

/* At startup: note an update this clock just installed by itself, and clear the marker. */
static void au_boot_note(void)
{
    char done[24];
    if (au_nvs_str("au_done", done, sizeof(done)) && done[0]) {
        if (strcmp(done, FW_VERSION_STR) == 0) {
            snprintf(s_au.updated_to, sizeof(s_au.updated_to), "%s", done);
            au_nvs_str("au_from", s_au.updated_from, sizeof(s_au.updated_from));
            au_nvs_set_u8("au_tries", 0);
            ESP_LOGI(TAG, "[auto-update] running v%s after an automatic update", done);
        }
        au_nvs_set_str("au_done", "");
    }
}

/* GET /api/auto_update — settings and the outcome of the last look. */
static esp_err_t api_auto_update_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    cJSON *o = cJSON_CreateObject();
    config_lock();
    const nextube_config_t *c = config_get();
    cJSON_AddBoolToObject(o, "enabled", c->auto_update_enabled);
    cJSON_AddNumberToObject(o, "policy", c->auto_update_policy);
    cJSON_AddNumberToObject(o, "hour", c->auto_update_hour);
    config_unlock();
    cJSON_AddBoolToObject(o, "busy", s_au.busy);
    cJSON_AddStringToObject(o, "current", FW_VERSION_STR);
    cJSON_AddStringToObject(o, "latest", s_au.latest);
    cJSON_AddStringToObject(o, "result", s_au.result);
    cJSON_AddStringToObject(o, "updated_to", s_au.updated_to);
    cJSON_AddStringToObject(o, "updated_from", s_au.updated_from);
    cJSON_AddNumberToObject(o, "last_check", (double)s_au.last);
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!js) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;
    const esp_err_t e = send_json(r, js);
    free(js);
    return e;
}

/* POST /api/auto_update/check {"install":false} — look now, ignoring the window and the switch (async; poll GET /api/auto_update).
 * "install":true really installs what a scheduled run would. */
static esp_err_t api_auto_update_check(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char body[48];
    cJSON *root = read_json_body_small(r, body, sizeof(body));
    if (!root) return ESP_FAIL;
    const bool install = cJSON_IsTrue(cJSON_GetObjectItem(root, "install"));
    cJSON_Delete(root);
    if (s_ota_active || s_au.busy) {
        httpd_resp_set_status(r, "409 Conflict");
        return send_json(r, "{\"error\":\"busy\"}");
    }
    s_au_force_install = install;
    s_au.busy = true;
    if (!np_job_start(au_forced_job)) {
        s_au.busy = false;
        httpd_resp_set_status(r, "503 Service Unavailable");
        return send_json(r, "{\"error\":\"not available (no network or busy)\"}");
    }
    return send_json(r, "{\"status\":\"started\"}");
}

/* The jobs' entry on the shared network task: runs a queued update job, then a queued Theme Manager refresh / install, then the
 * daily catalog check when it is due, then sleeps until woken again. */
static uint32_t np_job_tick(void)
{
    if (s_np_job) {
        UBaseType_t prio = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, NP_JOB_PRIORITY);
        s_np_job(NULL);
        vTaskPrioritySet(NULL, prio);
        s_np_job = NULL;
        ESP_LOGI(TAG, "[jobs] net_poll stack high-water mark: %u B unused", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    if (s_tm_pending) {
        s_tm_pending = false;
        tm_job();
    }
    uint32_t wake = TM_AUTO_POLL_MS;
    if (s_stock_scan_pending) {            /* the stock-file scan, queued by stock_files_check_spawn() */
        const int64_t left = s_stock_scan_due_us - esp_timer_get_time();
        if (left <= 0) {
            s_stock_scan_pending = false;
            stock_files_check();
            ESP_LOGI(TAG, "[stock] scan done on the network task; its stack high-water mark: %u B unused", (unsigned)uxTaskGetStackHighWaterMark(NULL));
            s_stock_scan_active = false;
            heap_watch("stock scan end");
        } else {
            wake = (uint32_t)(left / 1000) + 50;
        }
    }
    tm_auto_check();
    au_tick();
    heap_watch("net job tick");
    return wake;
}


static esp_err_t tm_start(httpd_req_t *r, bool install, bool check, bool uninstall, const char *id)
{
    if (s_ota_active || s_tm.state == TM_RUNNING) {
        httpd_resp_set_status(r, "409 Conflict");
        return send_json(r, "{\"error\":\"busy\"}");
    }
    if (!check && !uninstall && !wifi_manager_is_connected()) {
        httpd_resp_set_status(r, "503 Service Unavailable");
        return send_json(r, "{\"error\":\"no network\"}");
    }
    s_tm_busy = true;                      /* from here on no stock scan can start ... */
    if (s_stock_scan_active) {      /* ... and one that already runs blocks this job */
        s_tm_busy = false;
        httpd_resp_set_status(r, "409 Conflict");
        return send_json(r, "{\"error\":\"busy\"}");
    }
    s_tm.install = install;
    s_tm.check = check;
    s_tm.uninstall = uninstall;
    strlcpy(s_tm.id, id ? id : "", sizeof(s_tm.id));
    s_tm.state = TM_RUNNING;
    s_tm.progress = 0;
    s_tm.error[0] = '\0';
    s_ota_active = true;
    strlcpy(s_tm.phase, "queued", sizeof(s_tm.phase));
    s_tm_pending = true;
    periodic_net_poll_force("jobs");   /* wakes the shared network task, which runs np_job_tick() */
    return send_json(r, "{\"status\":\"started\"}");
}

/* GET /api/themes/catalog — the cached catalog merged with what is installed on this clock (no network access). */
static esp_err_t api_themes_catalog(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    cJSON *out = cJSON_CreateObject();
    char active[32];
    config_lock();
    strlcpy(active, config_get()->theme, sizeof(active));
    config_unlock();
    cJSON_AddStringToObject(out, "active", active);
    cJSON_AddStringToObject(out, "firmware", FW_VERSION_STR);
    cJSON *list = cJSON_AddArrayToObject(out, "themes");
    tm_restore_catalog();
    /* While a refresh runs the cached string is about to be replaced, so don't read it. */
    bool busy = (s_tm.state == TM_RUNNING && !s_tm.install && !s_tm.check && !s_tm.uninstall);
    cJSON *cat = (!busy && s_tm_catalog) ? cJSON_Parse(s_tm_catalog) : NULL;
    cJSON_AddBoolToObject(out, "busy", busy);
    cJSON_AddBoolToObject(out, "loaded", cat != NULL);
    if (cat) {
        cJSON *pv = cJSON_GetObjectItem(cat, "pack_version");
        cJSON_AddNumberToObject(out, "pack_version", cJSON_IsNumber(pv) ? pv->valueint : 0);
        const char *upd = cJSON_GetStringValue(cJSON_GetObjectItem(cat, "updated"));
        cJSON_AddStringToObject(out, "updated", upd ? upd : "");
        const time_t now = time(NULL);
        cJSON_AddNumberToObject(out, "age_s", (s_tm_fetched > 0 && ntp_has_valid_time() && now >= s_tm_fetched) ? (double)(now - s_tm_fetched) : -1.0);
        cJSON *it;
        int pos = -1;
        cJSON_ArrayForEach(it, cJSON_GetObjectItem(cat, "themes")) {
            pos++;
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
            if (!tm_id_valid(id)) continue;
            int inst_rev = 0;
            bool inst = tm_installed(id, &inst_rev);
            cJSON *rv = cJSON_GetObjectItem(it, "rev");
            int rev = cJSON_IsNumber(rv) ? rv->valueint : 1;
            const char *min_fw = cJSON_GetStringValue(cJSON_GetObjectItem(it, "min_fw"));
            cJSON *e = cJSON_CreateObject();
            static const char *const k_str[] = { "id", "name", "family", "variant", "notes", "min_fw", "preview", "preview_full", "category" };
            for (size_t k = 0; k < sizeof(k_str) / sizeof(k_str[0]); k++) {
                const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(it, k_str[k]));
                cJSON_AddStringToObject(e, k_str[k], v ? v : "");
            }
            cJSON *sz = cJSON_GetObjectItem(it, "size");
            cJSON_AddNumberToObject(e, "size", cJSON_IsNumber(sz) ? sz->valueint : 0);
            cJSON_AddNumberToObject(e, "rev", rev);
            cJSON_AddBoolToObject(e, "bundled", cJSON_IsTrue(cJSON_GetObjectItem(it, "bundled")));
            cJSON_AddBoolToObject(e, "installed", inst);
            cJSON_AddNumberToObject(e, "installed_rev", inst_rev);
            cJSON_AddBoolToObject(e, "update", inst && inst_rev > 0 && rev > inst_rev);
            cJSON_AddBoolToObject(e, "differs", inst && inst_rev == 0);   /* on the clock, but not the catalog's version */
            cJSON_AddBoolToObject(e, "compatible", tm_fw_ok(min_fw));
            cJSON_AddBoolToObject(e, "active", strcmp(active, id) == 0);
            cJSON_AddBoolToObject(e, "preview_local", pos < TM_PREV_THEMES && ((s_tm_prev_mask >> pos) & 1u));
            cJSON_AddItemToArray(list, e);
        }
        cJSON_Delete(cat);
    }
    char *js = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (!js) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;
    esp_err_t e = send_json(r, js);
    free(js);
    return e;
}

/* POST /api/themes/refresh — fetch themes/catalog.json from the repo (async; poll /api/themes/status). */
static esp_err_t api_themes_refresh(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    return tm_start(r, false, false, false, NULL);
}

/* POST /api/themes/verify — re-hash the installed catalog themes against their packs (local only; async, poll /api/themes/status). */
static esp_err_t api_themes_verify(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    tm_restore_catalog();
    if (!s_tm_catalog)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Load the catalog first"), ESP_FAIL;
    return tm_start(r, false, true, false, NULL);
}

/* POST /api/themes/install {"id":"Nixie"} — download and install that catalog entry (async; poll /api/themes/status). */
static esp_err_t api_themes_install(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char body[96];
    cJSON *root = read_json_body_small(r, body, sizeof(body));
    if (!root) return ESP_FAIL;
    char id[TM_ID_MAX + 1] = "";
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
    if (v) strlcpy(id, v, sizeof(id));
    cJSON_Delete(root);
    tm_restore_catalog();
    if (!tm_id_valid(id) || !s_tm_catalog)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, s_tm_catalog ? "Bad theme id" : "Load the catalog first"), ESP_FAIL;
    cJSON *cat = cJSON_Parse(s_tm_catalog);
    cJSON *themes = cat ? cJSON_GetObjectItem(cat, "themes") : NULL;
    cJSON *found = NULL, *it;
    cJSON_ArrayForEach(it, themes) {
        const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
        if (cid && strcmp(cid, id) == 0) { found = it; break; }
    }
    bool compatible = found && tm_fw_ok(cJSON_GetStringValue(cJSON_GetObjectItem(found, "min_fw")));
    cJSON_Delete(cat);
    if (!found) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not in the catalog"), ESP_FAIL;
    if (!compatible) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Needs a newer firmware"), ESP_FAIL;
    return tm_start(r, true, false, false, id);
}

/* POST /api/themes/remove {"id":"Nixie"} — delete an installed theme's folder (async; poll /api/themes/status, which reports progress).
 * If it is the active theme the clock switches to WeatherLive first (status "changed" = 1). */
static esp_err_t api_themes_remove(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char body[96];
    cJSON *root = read_json_body_small(r, body, sizeof(body));
    if (!root) return ESP_FAIL;
    char id[TM_ID_MAX + 1] = "";
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
    if (v) strlcpy(id, v, sizeof(id));
    cJSON_Delete(root);
    int rev;
    if (!tm_id_valid(id) || !tm_installed(id, &rev))
        return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Theme not installed"), ESP_FAIL;
    return tm_start(r, false, false, true, id);
}

/* GET /api/themes/status — progress of the running (or last) refresh / install. */
static esp_err_t api_themes_status(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    static const char *const names[] = { "idle", "running", "done", "error" };
    char buf[400];
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"op\":\"%s\",\"id\":\"%.31s\",\"phase\":\"%.15s\",\"progress\":%d,\"changed\":%d,\"error\":\"%.95s\"}",
             names[s_tm.state], s_tm.install ? "install" : s_tm.uninstall ? "remove" : s_tm.check ? "verify" : "refresh", s_tm.id, s_tm.phase, (int)s_tm.progress,
             s_tm.changed, s_tm.error);
    return send_json(r, buf);
}

/* GET /api/webui_pull_auto — no request body.
 * Reads the webui URL + sha256 written to NVS by ota_pull_task before the
 * firmware reboot and starts webui_pull_task.  Used by the online-updater
 * Phase 4 so the browser never has to POST a body after the OTA reboot
 * (httpd_req_recv fails on some connections right after the device reboots). */
static esp_err_t api_webui_pull_auto(httpd_req_t *r)
{
    ESP_LOGD(TAG, "[webui_auto] handler called — post_ota_auth=%d", (int)s_post_ota_auth);
    /* Check OTA-in-progress BEFORE consuming the one-time NVS bypass flag.
     * If Phase 4 fires pre-reboot (while the firmware OTA task is still
     * running), returning 409 without consuming the flag lets the post-reboot
     * call use it correctly for auth bypass. */
    if (s_ota_active) {
        httpd_resp_set_status(r, "409 Conflict");
        return send_json(r, "{\"error\":\"ota_in_progress\"}");
    }
    /* Device-driven pull already finished this boot — confirm done without
     * re-spawning the task (NVS webui_url may still be present).           */
    if (s_webui.state == WEBUI_DONE)
        return send_json(r, "{\"status\":\"done\"}");

    if (!s_post_ota_auth) s_post_ota_auth = consume_post_ota_flag();
    if (!post_ota_auth_valid()) { REQUIRE_AUTH(r); }

    nvs_handle_t h;
    if (nvs_open("nextube_sec", NVS_READWRITE, &h) != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS open failed"), ESP_FAIL;

    size_t url_sz = sizeof(s_webui.url);
    esp_err_t ue = nvs_get_str(h, "webui_url", s_webui.url, &url_sz);
    s_webui.sha256[0] = '\0';
    size_t sha_sz = sizeof(s_webui.sha256);
    nvs_get_str(h, "webui_sha256", s_webui.sha256, &sha_sz);
    nvs_close(h);

    if (ue != ESP_OK || !s_webui.url[0])
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "No webui URL stored — send webui_url in the ota_pull request"), ESP_FAIL;

    s_webui.state    = WEBUI_IDLE;
    s_webui.error[0] = '\0';
    s_webui.written_files[0] = '\0';
    s_ota_active     = true;

    if (!start_webui_pull_task()) {
        s_ota_active = false;
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Task create failed"), ESP_FAIL;
    }
    return send_json(r, "{\"status\":\"started\"}");
}

static esp_err_t api_webui_pull(httpd_req_t *r)
{
    /* Consume the one-time NVS bypass flag set before OTA reboot. */
    if (!s_post_ota_auth) s_post_ota_auth = consume_post_ota_flag();
    bool bypassed = !post_ota_auth_valid();   /* true = normal auth required below */
    if (bypassed) { REQUIRE_AUTH(r); }
    if (s_ota_active) {
        httpd_resp_set_status(r, "409 Conflict");
        return send_json(r, "{\"error\":\"ota_in_progress\"}");
    }

    /* Parse into local buffers first, NOT directly into s_webui.url/sha256 —
     * the bypass-window check below must be able to reject the request
     * without having already clobbered global pull state with an unvetted
     * attacker-supplied URL. */
    char url_buf[sizeof(s_webui.url)];
    char sha_buf[sizeof(s_webui.sha256)];
    if (!read_pull_url_body(r, url_buf, sizeof(url_buf), sha_buf, sizeof(sha_buf), NULL))
        return ESP_FAIL;
    bool hash_ok = sha_buf[0] != '\0';

    /* !bypassed reached here only via REQUIRE_AUTH, i.e. a genuine password —
     * that request body is trusted. During the post-OTA bypass window there
     * is no password at all, so an attacker-supplied url/sha256 in the body
     * would let anyone on the LAN point this device at an arbitrary ZIP for
     * up to POST_OTA_AUTH_TTL_US after any OTA reboot. Require and verify a
     * hash in that window instead of trusting the body's url/sha256 — same
     * bar api_webui_pull_auto already holds itself to for the same window. */
    if (bypassed && !hash_ok) {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                    "sha256 required during the post-OTA auth window"), ESP_FAIL;
    }

    snprintf(s_webui.url, sizeof(s_webui.url), "%s", url_buf);
    s_webui.sha256[0] = '\0';
    if (hash_ok) snprintf(s_webui.sha256, sizeof(s_webui.sha256), "%s", sha_buf);

    s_webui.state   = WEBUI_IDLE;
    s_webui.error[0] = '\0';
    s_webui.written_files[0] = '\0';
    s_ota_active     = true;

    if (!start_webui_pull_task()) {
        s_ota_active = false;
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Task create failed"), ESP_FAIL;
    }
    return send_json(r, "{\"status\":\"started\"}");
}

/* POST /api/webui_pull_reboot — body: {"url":"...", "sha256":"..."}
 * Fallback for the version-mismatch banner's "Fix now" flow, used only if
 * a live pull (/api/webui_pull) fails to start under memory pressure.
 * Stashes the URL/hash in NVS with the same keys ota_pull_task writes
 * before a real firmware reboot, then reboots — the existing post_ota
 * boot-time auto-pull path (see the post_ota_boot_pending check near
 * web_server_start()) picks it up from there, running in the fresh,
 * unfragmented heap a just-booted device has. */
static esp_err_t api_webui_pull_reboot(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    if (s_ota_active) {
        httpd_resp_set_status(r, "409 Conflict");
        return send_json(r, "{\"error\":\"ota_in_progress\"}");
    }

    char url_buf[sizeof(s_webui.url)];
    char sha_buf[sizeof(s_webui.sha256)];
    if (!read_pull_url_body(r, url_buf, sizeof(url_buf), sha_buf, sizeof(sha_buf), NULL))
        return ESP_FAIL;
    if (!url_buf[0])
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "url required"), ESP_FAIL;

    nvs_handle_t nvs_h;
    if (nvs_open("nextube_sec", NVS_READWRITE, &nvs_h) != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS open failed"), ESP_FAIL;
    nvs_set_u8(nvs_h, "post_ota", 1);
    nvs_set_str(nvs_h, "webui_url", url_buf);
    if (sha_buf[0]) nvs_set_str(nvs_h, "webui_sha256", sha_buf);
    else nvs_erase_key(nvs_h, "webui_sha256");
    esp_err_t ec = nvs_commit(nvs_h);
    nvs_close(nvs_h);
    if (ec != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS commit failed"), ESP_FAIL;

    /* Stays set — same reasoning as every other reboot-bound success path
     * (ota_pull_task, api_ota_impl): a reboot is imminent, so there's no
     * "clear after" moment, just a short window to block a duplicate
     * request in before esp_restart() fires. */
    s_ota_active = true;
    ESP_LOGI(TAG, "[webui] staged for post-reboot pull: %.80s", url_buf);
    return ota_finish_and_reboot(r, "{\"status\":\"rebooting\"}");
}

/* ── Live-source stock-file repair ───────────────────────────────────────
 * stock_files_check() is a cheap presence-only check — a present-but-stale
 * file (a partially-applied hotpatch, or a device old enough to predate a
 * change) looks identical to a healthy one under a plain stat(). This
 * mechanism verifies every STOCK_FILES entry's actual content against
 * STOCK_SHA256[i], fetching only the ones that don't match, from the
 * public repo at the git tag matching THIS firmware's FS_VERSION_STR
 * (never `main`).
 *
 * Explicit/user-triggered (POST /api/stock_repair), not automatic on
 * boot: hashing ~380 files' full content is real work with no business
 * running unattended or in a request path. No manifest is ever fetched —
 * STOCK_SHA256 baked into this firmware image already is the manifest.
 * The work list is sized to STOCK_FILES_COUNT (see s_work below), so one
 * pass repairs however many files actually need it — no separate cap.
 * Obsolete files (no longer part of STOCK_FILES at all) aren't this
 * mechanism's job — verifying a known path can't discover an unknown
 * stray one; those are left for LittleFS Recovery too. */
#define STOCK_REPAIR_MAX_FILE_BYTES (1 * 1024 * 1024)   /* sanity cap — largest stock file today is ~90 KB */

typedef enum { STOCK_REPAIR_IDLE=0, STOCK_REPAIR_VERIFYING, STOCK_REPAIR_FETCHING,
               STOCK_REPAIR_DONE, STOCK_REPAIR_ERROR } stock_repair_state_t;

static struct {
    stock_repair_state_t state;
    /* 192, not the 128 s_pull/s_webui use for the same purpose:
     * stock_repair_tag_exists()'s "no release tagged vX found in %s" message
     * interpolates s_stock_repair_owner_repo (char[64]) — GCC's
     * -Werror=format-truncation computes worst case assuming that buffer is
     * filled to its full declared size, which pushes the worst-case output
     * past 128 and fails the build even though no real repo string ever
     * gets close to 64 bytes. */
    char error[192];
    int  checked;       /* STOCK_FILES entries verified so far (phase 1) */
    int  need_repair;   /* how many were found missing or stale */
    int  fetched;       /* how many were successfully re-fetched and written (phase 2) */
    int  failed;        /* how many failed to fetch/verify/write */
    char repaired_files[512];   /* bounded, comma-joined list of files actually
                                  * repaired — see append_bounded_list() */
} s_stock_repair;
static char s_stock_repair_owner_repo[64];   /* set by api_stock_repair() just before spawning the task */

/* Reads a whole file into a heap buffer and checks it against expected_hex
 * via the existing sha256_verify() (defined above). A missing file, an
 * oversized one (sanity cap, not expected for anything in STOCK_FILES), or
 * an OOM all count as "doesn't match" — the caller treats that identically
 * to a genuine hash mismatch (needs repair), which is the correct behavior
 * for all three. */
static bool hp_file_sha256_matches(const char *vpath, const char *expected_hex)
{
    FILE *f = fopen(vpath, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > STOCK_REPAIR_MAX_FILE_BYTES) { fclose(f); return false; }

    /* PSRAM, not plain malloc(): this buffer is data-sized (up to
     * STOCK_REPAIR_MAX_FILE_BYTES), same class of allocation as every other
     * "read/download a variable-sized buffer" spot in this file (the OTA and
     * webui-pull image/ZIP buffers, all heap_caps_malloc(..., CAP_SPIRAM)) —
     * plain malloc() here would compete with the exact same tight internal
     * heap that task-stack allocation already fails against on this device. */
    uint8_t *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return false; }
    bool read_ok = (fread(buf, 1, (size_t)sz, f) == (size_t)sz);
    fclose(f);

    bool match = read_ok && sha256_verify(buf, (size_t)sz, expected_hex);
    free(buf);
    return match;
}

/* Builds one esp_http_client_handle_t meant to be reused across MANY
 * sequential requests to the same host (raw.githubusercontent.com) for the
 * whole of stock_repair_task's phase 2 — one TLS handshake instead of one
 * per file. Same config fields as hp_https_open() (buffer sizes,
 * crt_bundle_attach, timeout, user_agent) so per-request behavior stays
 * identical aside from lifecycle; .url is a placeholder, overwritten by the
 * first real call. Deliberately no redirect-hop handling — unlike
 * hp_https_open(), which follows up to 5 hops for GitHub Release assets
 * that commonly redirect to Azure Blob Storage — raw.githubusercontent.com
 * (Fastly-backed) serves content directly, so a non-200 here is just
 * treated as a per-file failure like any other. Caller owns the returned
 * handle for the whole phase-2 loop and must esp_http_client_cleanup() it
 * exactly once when done. */
static esp_http_client_handle_t hp_stock_client_init(const char *user_agent, int timeout_ms)
{
    esp_http_client_config_t hcfg = {
        .url               = "https://raw.githubusercontent.com",
        .timeout_ms        = timeout_ms,
        .user_agent        = user_agent,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size       = 16384,
        .buffer_size_tx    = 4096,
    };
    return esp_http_client_init(&hcfg);
}

/* Fetches ONE STOCK_FILES[idx] from the tag-pinned raw-content URL using a
 * caller-owned, potentially-already-open `client` (see hp_stock_client_init()
 * above) instead of opening/closing its own — so a whole phase-2 run can
 * share one TLS connection. Can't reuse hp_https_read_body() here: its
 * contract is that it ALWAYS closes and cleans up the client, which would
 * defeat reuse, so the chunked read-with-hard-deadline loop is inlined.
 *
 * On success, leaves `client` open/connected for the next call. On ANY
 * failure — open failure, bad status, bad content-length, OOM, short read,
 * or short write — closes `client` (but does not clean it up) before
 * returning, so an unread error body (e.g. a 404's small body) can never
 * desync the next file's response parsing over a reused connection; only
 * that one file loses the reuse benefit, not the whole run. */
static bool hp_fetch_verify_write_ka(esp_http_client_handle_t client, size_t idx,
                                      const char *owner_repo, int timeout_ms)
{
    char url[512];
    snprintf(url, sizeof(url), "https://raw.githubusercontent.com/%s/v%s/data/%s",
             owner_repo, FS_VERSION_STR, STOCK_FILES[idx]);

    esp_timer_handle_t deadline = hp_arm_hard_deadline(timeout_ms);
    if (esp_http_client_set_url(client, url) != ESP_OK ||
        esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGW(TAG, "[stock_repair] %s: HTTP open failed", STOCK_FILES[idx]);
        hp_disarm_hard_deadline(deadline);
        esp_http_client_close(client);
        return false;
    }
    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || content_len <= 0 || content_len > STOCK_REPAIR_MAX_FILE_BYTES) {
        ESP_LOGW(TAG, "[stock_repair] %s: HTTP %d (content-length=%d)",
                 STOCK_FILES[idx], status, content_len);
        hp_disarm_hard_deadline(deadline);
        esp_http_client_close(client);   /* any unread error body must not leak into the next request */
        return false;
    }
    hp_disarm_hard_deadline(deadline);

    /* PSRAM — same reasoning as hp_file_sha256_matches() above: a
     * data-sized download buffer has no business competing with the
     * internal heap a task stack allocation just failed against. */
    uint8_t *buf = heap_caps_malloc((size_t)content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        esp_http_client_close(client);
        return false;
    }

    esp_timer_handle_t rdeadline = hp_arm_hard_deadline(timeout_ms + (content_len / 20480) * 1000);
    int received = 0;
    while (received < content_len) {
        int want = content_len - received;
        if (want > HP_READ_CHUNK) want = HP_READ_CHUNK;
        int n = esp_http_client_read(client, (char *)buf + received, want);
        if (n <= 0) break;
        received += n;
    }
    hp_disarm_hard_deadline(rdeadline);

    bool ok = (received == content_len) && sha256_verify(buf, (size_t)received, STOCK_SHA256[idx]);
    if (!ok) {
        ESP_LOGW(TAG, "[stock_repair] %s: %s", STOCK_FILES[idx],
                 received != content_len ? "short download" : "SHA-256 mismatch — rejected, not written");
        free(buf);
        esp_http_client_close(client);
        return false;
    }

    char vpath[320];
    snprintf(vpath, sizeof(vpath), "/spiffs/%s", STOCK_FILES[idx]);
    hp_mkdir_p(vpath);
    FILE *f = fopen(vpath, "wb");
    if (!f) { free(buf); esp_http_client_close(client); return false; }
    size_t written = fwrite(buf, 1, (size_t)received, f);
    fclose(f);
    free(buf);

    if (written != (size_t)received) {
        ESP_LOGW(TAG, "[stock_repair] %s: short write (%u/%d)", STOCK_FILES[idx], (unsigned)written, received);
        esp_http_client_close(client);
        return false;
    }
    ESP_LOGI(TAG, "[stock_repair] repaired '%s' (%d B)", STOCK_FILES[idx], received);
    return true;   /* client left open/connected for reuse by the next file */
}

/* Checks whether a GitHub Release tagged v{FS_VERSION_STR} exists in `repo`
 * before Phase 2 below attempts even one per-file fetch. hp_fetch_verify_write_ka()
 * pins every raw-content URL to that exact tag — a tag that hasn't been
 * published yet makes every file 404 identically to a genuinely
 * missing/corrupt one, with no way to tell the difference from a single
 * file's response. Reuses hp_https_open() (same TLS/redirect/hard-deadline
 * handling as every other fetch here); on success the client is left open
 * by that helper and just needs closing since we only care that the tag
 * resolved, not the release JSON body. */
static bool stock_repair_tag_exists(const char *repo, char *err, size_t err_sz)
{
    char url[160];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/tags/v%s",
             repo, FS_VERSION_STR);
    char ua[128];
    snprintf(ua, sizeof(ua), HTTP_UA_BASE "%s", repo);

    esp_http_client_handle_t client;
    int content_len;
    if (!hp_https_open(url, ua, 10000, 8192, &client, &content_len, err, err_sz, "stock_repair")) {
        return false;   /* err already set, e.g. "HTTP 404 (content-length=..)" */
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return true;
}


static void stock_repair_task(void *arg)
{
    ESP_LOGI(TAG, "[stock_repair] task started on core %d", xPortGetCoreID());
    s_stock_repair.state = STOCK_REPAIR_VERIFYING;
    bool tasks_suspended = false;

    /* Same TLS-needs-correct-wall-clock reasoning as webui_pull_task. */
    if (!ntp_has_valid_time()) {
        for (int i = 0; i < 30 && !ntp_has_valid_time(); i++)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!ntp_has_valid_time()) {
        snprintf(s_stock_repair.error, sizeof(s_stock_repair.error),
                 "No valid time after 30 s — NTP/RTC unavailable");
        s_stock_repair.state = STOCK_REPAIR_ERROR;
        goto stock_repair_done;
    }

    display_show_wait();
    tls_sem_take();
    ota_suspend_tasks();
    tasks_suspended = true;

    /* ── Phase 1: verify — local only, no network yet ────────────────── */
    {
        /* Sized to STOCK_FILES_COUNT — the true ceiling, since you can never
         * need to repair more files than exist — so one pass always covers
         * however many actually need it, no separate cap to track.
         *
         * PSRAM, NOT a stack local and NOT a static internal-RAM array —
         * same reasoning as exc_buf below: a stack local this size would
         * overflow this function's own measured ~4544 B peak on a 6144 B
         * stack, but making it `static` instead only trades that for a
         * permanent internal-DRAM cost. See stock_missing_paths_buf() above
         * for the heap_init evidence that cost was real and severe on this
         * device's memory budget. */
        static size_t *s_work = NULL;
        if (!s_work)
            s_work = heap_caps_malloc(STOCK_FILES_COUNT * sizeof(*s_work),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_work) {
            snprintf(s_stock_repair.error, sizeof(s_stock_repair.error), "Out of memory (work buffer)");
            s_stock_repair.state = STOCK_REPAIR_ERROR;
            goto stock_repair_done;
        }
        int work_n = 0;

        /* Same exception snapshot as stock_files_check() — a file the user
         * deliberately removed (and excepted via the missing-files picker)
         * should never be flagged as needing repair.
         *
         * PSRAM, NOT a stack local and NOT a static internal-RAM array —
         * same reasoning as stock_files_check()'s copy: a stack local here
         * would have overflowed this function's own measured ~4544 B peak
         * on a 6144 B stack by ~1.5 KB, but making it `static` instead only
         * traded that for a permanent internal-DRAM cost. See
         * stock_missing_paths_buf() above for the heap_init evidence that
         * cost was real and severe on this device's memory budget. */
        static char (*exc_buf)[STOCK_EXCEPTION_PATH_LEN] = NULL;
        if (!exc_buf)
            exc_buf = heap_caps_malloc(STOCK_EXCEPTION_MAX * sizeof(*exc_buf),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint8_t exc_count = 0;
        if (exc_buf) {
            config_lock();
            const nextube_config_t *repair_cfg = config_get();
            exc_count = repair_cfg->stock_exception_count;
            memcpy(exc_buf, repair_cfg->stock_exception_paths, STOCK_EXCEPTION_MAX * sizeof(*exc_buf));
            config_unlock();
        }

        for (size_t i = 0; i < STOCK_FILES_COUNT; i++) {
            s_stock_repair.checked = (int)i + 1;
            /* Real-hardware telemetry: 383 files took ~37s here (fopen +
             * fread + SHA-256 per file, no yield) and starved IDLE0 on this
             * task's own core long enough to trip the task watchdog ~30s in
             * — the exact failure mode extract_zip_to_littlefs's per-write
             * vTaskDelay(1) already guards against elsewhere in this file.
             * This loop needed the same discipline and didn't have it. Must
             * run before the exception `continue` below too, or a device
             * with many excepted files re-creates the exact same starvation
             * this was added to fix. */
            vTaskDelay(1);
            if (stock_path_excepted(exc_buf, exc_count, STOCK_FILES[i])) continue;
            char vpath[320];
            snprintf(vpath, sizeof(vpath), "/spiffs/%s", STOCK_FILES[i]);
            if (!hp_file_sha256_matches(vpath, STOCK_SHA256[i]))
                s_work[work_n++] = i;
        }
        s_stock_repair.need_repair = work_n;

        if (work_n == 0) {
            s_stock_repair.state = STOCK_REPAIR_DONE;
            goto stock_repair_resume;
        }

        /* ── Phase 2: fetch + verify + write, only the entries that need it ── */
        char tag_err[64];
        if (!stock_repair_tag_exists(s_stock_repair_owner_repo, tag_err, sizeof(tag_err))) {
            if (strncmp(tag_err, "HTTP 404", 8) == 0) {
                /* Confident, specific signal — see stock_repair_tag_exists()'s
                 * doc comment: every one of the work_n fetches below would 404
                 * identically to this pre-check, each misread as "that
                 * particular file is missing/corrupt". Fail fast with the
                 * actual cause instead of work_n misleading per-file 404s. */
                snprintf(s_stock_repair.error, sizeof(s_stock_repair.error),
                         "No release tagged v%s found in %s — publish a matching release first",
                         FS_VERSION_STR, s_stock_repair_owner_repo);
                s_stock_repair.failed = work_n;
                s_stock_repair.state  = STOCK_REPAIR_ERROR;
                ESP_LOGE(TAG, "[stock_repair] %s", s_stock_repair.error);
                goto stock_repair_resume;
            }
            /* Anything else (connect/TLS/timeout) doesn't confidently mean the
             * tag is missing — proceed to the per-file fetches as before
             * rather than blocking the whole repair on an inconclusive
             * pre-check; each file's own hp_https_open() call will report its
             * own real failure if the network is genuinely down. */
            ESP_LOGW(TAG, "[stock_repair] tag-existence pre-check inconclusive (%s) — proceeding anyway", tag_err);
        }

        s_stock_repair.state = STOCK_REPAIR_FETCHING;
        char ua[128];
        snprintf(ua, sizeof(ua), HTTP_UA_BASE "%s", s_stock_repair_owner_repo);

        /* One connection reused across every file below instead of one TLS
         * handshake per file — see hp_stock_client_init()/hp_fetch_verify_write_ka(). */
        esp_http_client_handle_t ka_client = hp_stock_client_init(ua, 15000);
        if (!ka_client) {
            snprintf(s_stock_repair.error, sizeof(s_stock_repair.error), "HTTP client init failed");
            s_stock_repair.failed = work_n;
            s_stock_repair.state  = STOCK_REPAIR_ERROR;
            goto stock_repair_resume;
        }

        size_t rf_len = 0;
        for (int k = 0; k < work_n; k++) {
            if (hp_fetch_verify_write_ka(ka_client, s_work[k], s_stock_repair_owner_repo, 15000)) {
                s_stock_repair.fetched++;
                append_bounded_list(s_stock_repair.repaired_files, sizeof(s_stock_repair.repaired_files),
                                     &rf_len, STOCK_FILES[s_work[k]], work_n - k);
            } else {
                s_stock_repair.failed++;
            }
            vTaskDelay(1);
        }
        esp_http_client_cleanup(ka_client);   /* close() already called per-file as needed; this frees the handle */

        s_stock_repair.state = (s_stock_repair.failed == 0) ? STOCK_REPAIR_DONE : STOCK_REPAIR_ERROR;
        if (s_stock_repair.failed > 0)
            snprintf(s_stock_repair.error, sizeof(s_stock_repair.error),
                     "%d/%d file(s) could not be repaired — try LittleFS Recovery for those",
                     s_stock_repair.failed, work_n);
    }

stock_repair_resume:
    fs_usage_invalidate();
    display_theme_cache_flush();
    /* Async, via stock_files_check_spawn() — same reasoning as the
     * hotpatch/webui-pull call sites: a direct call here would hold tasks
     * suspended and the display parked for the whole ~13-15s+ scan on top
     * of the repair work already done above. */
    stock_files_check_spawn();
    ESP_LOGI(TAG, "[stock_repair] done: checked=%d need_repair=%d fetched=%d failed=%d",
             s_stock_repair.checked, s_stock_repair.need_repair,
             s_stock_repair.fetched, s_stock_repair.failed);

stock_repair_done:
    if (tasks_suspended) { webui_resume_tasks(); tls_sem_give(); }
    display_resume_after_wait();
    s_ota_active = false;
    ESP_LOGI(TAG, "[stock_repair] stack high-water mark: %u B unused (of %u allocated)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)PERIODIC_NET_POLL_STACK_SIZE);
}

/* POST /api/stock_repair — no body. Starts stock_repair_task() (see its
 * doc comment above for the full design). Reuses the same s_ota_active
 * single-flight guard as every other flash-affecting background task in
 * this file. */
static esp_err_t api_stock_repair(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    if (s_ota_active) {
        httpd_resp_set_status(r, "409 Conflict");
        return send_json(r, "{\"error\":\"ota_in_progress\"}");
    }

    resolve_update_repo(s_stock_repair_owner_repo, sizeof(s_stock_repair_owner_repo));

    s_stock_repair.state             = STOCK_REPAIR_IDLE;
    s_stock_repair.error[0]          = '\0';
    s_stock_repair.checked           = 0;
    s_stock_repair.need_repair       = 0;
    s_stock_repair.fetched           = 0;
    s_stock_repair.failed            = 0;
    s_stock_repair.repaired_files[0] = '\0';
    s_ota_active = true;

    if (!np_job_start(stock_repair_task)) {
        s_ota_active = false;
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not start (busy or no network)"), ESP_FAIL;
    }
    return send_json(r, "{\"status\":\"started\"}");
}

static esp_err_t api_stock_repair_status(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    static const char *const names[] = { "idle","verifying","fetching","done","error" };
    /* 1024: needs to clear GCC's -Werror=format-truncation worst case, which
     * assumes error[192] and repaired_files[512] (see their struct-field
     * comments) are BOTH filled to their full declared size at once —
     * 192+512 already exceeds the previous 768 before even counting the
     * literal JSON text and the numeric fields. */
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"error\":\"%s\",\"checked\":%d,\"total\":%u,"
             "\"need_repair\":%d,\"fetched\":%d,\"failed\":%d,"
             "\"repaired_files\":\"%s\"}",
             names[s_stock_repair.state], s_stock_repair.error,
             s_stock_repair.checked, (unsigned)STOCK_FILES_COUNT,
             s_stock_repair.need_repair, s_stock_repair.fetched, s_stock_repair.failed,
             s_stock_repair.repaired_files);
    return send_json(r, buf);
}

/* GET /api/stock_missing_list — full list backing the missing-files
 * exception picker (Settings → System → "Manage exceptions"). Distinct from
 * stock_repair_status: that one verifies CONTENT (SHA-256) — this just
 * reports the last stock_files_check() presence scan's results (already
 * computed, existence-only), so a device with hundreds of deliberately-
 * removed files can still list every one of them for the picker to build
 * its checkbox tree from. */
static esp_err_t api_stock_missing_list(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "total", s_stock_missing_count);
    cJSON_AddBoolToObject(root, "truncated", s_stock_missing_count > s_stock_missing_list_count);
    cJSON *arr = cJSON_AddArrayToObject(root, "paths");
    for (int i = 0; i < s_stock_missing_list_count; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_stock_missing_paths[i]));
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;
    esp_err_t err = send_json(r, out);
    free(out);
    return err;
}

/* URL-decode a query-string parameter value in-place.
 * httpd_query_key_value() returns the raw (percent-encoded) value.
 * Without decoding, a path like "/" arrives as "%2F" and opendir/fopen
 * fail with ENOENT because the kernel never sees the real '/' character.
 *
 * Returns true on a clean decode; false if the input contained an encoded
 * NUL or other control character.  %00 NUL injection
 * truncates strings before any post-decode validation can catch it —
 * e.g. /api/file/get?path=/secret%00.png would slip through a hypothetical
 * ".png-suffix" filter while opening "/secret".  Other control bytes
 * (CR/LF/TAB/etc) also have no place in URL parameters here.  Callers
 * should treat a false return as a 400 Bad Request — the input is almost
 * certainly hostile. */
static bool url_decode_inplace(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], '\0' };
            char *end;
            long v = strtol(hex, &end, 16);
            if (end == hex + 2) {
                /* Reject encoded NUL (truncation attack) and ASCII control
                 * characters (no legitimate use in our URL parameters).
                 * 0x80-0xFF are still allowed so UTF-8 filenames work. */
                if (v < 0x20 || v == 0x7F) return false;
                *w++ = (char)v;
                r += 3;
            } else {
                /* Malformed sequence — copy the '%' literally and advance one */
                *w++ = *r++;
            }
        } else if (*r == '+') {
            *w++ = ' '; r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
    return true;
}

static esp_err_t api_file_ls(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char path[128] = "/spiffs";
    char q[128];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
        char d[64];
        if (httpd_query_key_value(q, "dir", d, sizeof(d)) == ESP_OK && d[0] != '\0') {
            if (!url_decode_inplace(d) || strstr(d, ".."))
                return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid path"), ESP_FAIL;
            snprintf(path, sizeof(path), "/spiffs%s", d);
        }
    }
    /* Strip trailing slash — ESP-IDF SPIFFS opendir() is sensitive to it.
     * Never strip below "/spiffs" (len=7). */
    int plen = (int)strlen(path);
    while (plen > 7 && path[plen - 1] == '/')
        path[--plen] = '\0';

    /* Stream the directory listing as chunked JSON.
     *
     * Building the whole listing with cJSON then calling httpd_resp_sendstr()
     * sends a single large payload that overflows the lwIP TCP send buffer
     * (default 5760 B) and triggers EAGAIN → connection failure.  Instead,
     * emit one JSON object per file via httpd_resp_send_chunk() so each
     * chunk is ≤ 512 bytes and always fits in the send buffer. */
    httpd_resp_set_type(r, "application/json");

    DIR *dp = opendir(path);
    if (!dp) {
        ESP_LOGW(TAG, "api_file_ls: opendir(%s) failed: errno=%d (%s)",
                 path, errno, strerror(errno));
        /* Return an empty JSON array — the UI can distinguish "no files"
         * from a real error by the HTTP status code remaining 200. */
        return httpd_resp_sendstr(r, "[]");
    }

    httpd_resp_send_chunk(r, "[", 1);

    bool first = true;
    struct dirent *e;
    char chunk[512];
    char ename[256];    /* JSON-escaped filename */

    while ((e = readdir(dp))) {
        /* JSON-escape the filename (guard against " and \ in names). */
        {
            const char *s = e->d_name;
            char *w = ename, *wend = ename + sizeof(ename) - 2;
            while (*s && w < wend) {
                if (*s == '"' || *s == '\\') *w++ = '\\';
                *w++ = *s++;
            }
            *w = '\0';
        }

        int n;
        if (e->d_type == DT_DIR) {
            n = snprintf(chunk, sizeof(chunk),
                         "%s{\"name\":\"%s\",\"type\":\"dir\"}",
                         first ? "" : ",", ename);
        } else {
            /* stat() the file for size + mtime.  mtime requires
             * CONFIG_LITTLEFS_MTIME=y; returns 0 for pre-existing files. */
            char fp[384];
            snprintf(fp, sizeof(fp), "%s/%s", path, e->d_name);
            struct stat st;
            long sz = 0, mt = 0;
            if (stat(fp, &st) == 0) { sz = (long)st.st_size; mt = (long)st.st_mtime; }
            n = snprintf(chunk, sizeof(chunk),
                         "%s{\"name\":\"%s\",\"type\":\"file\",\"size\":%ld,\"mtime\":%ld}",
                         first ? "" : ",", ename, sz, mt);
        }

        if (n > 0 && n < (int)sizeof(chunk))
            httpd_resp_send_chunk(r, chunk, n);
        first = false;
    }
    closedir(dp);

    httpd_resp_send_chunk(r, "]", 1);
    httpd_resp_send_chunk(r, NULL, 0);   /* terminate chunked transfer */
    return ESP_OK;
}

/* GET /api/themes
 * Scans /spiffs/images/themes/ and returns a sorted JSON array of directory
 * names.  The web UI uses this to build the theme dropdown dynamically so
 * custom themes added via the file browser appear without a firmware update. */
static esp_err_t api_themes(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
#define MAX_THEMES      48
#define THEME_NAME_MAX  64
    char names[MAX_THEMES][THEME_NAME_MAX];
    int  count = 0;

    /* Built-in procedural themes — drawn from primitives in the firmware, so
     * they have no /images/themes/ asset folder and must be injected here.
     * "WeatherLive Demo" runs an accelerated day/night + auto-cycles every
     * weather condition for showcasing. */
    strlcpy(names[count++], "WeatherLive", THEME_NAME_MAX);
    strlcpy(names[count++], "WeatherLive Demo", THEME_NAME_MAX);
    /* "DotMatrix" — procedural 7x14-cell dot-matrix glyphs (display.c's
     * dm_render_asset()/dm_draw_text()), independently on/off colorable.
     * Also ships with no asset folder, for the same reason. */
    strlcpy(names[count++], "DotMatrix", THEME_NAME_MAX);
    /* "Segments" — procedural 16-segment display glyphs (display.c's
     * seg_render_asset()/seg_draw_glyph()), independently on/off colorable.
     * Also ships with no asset folder, for the same reason. */
    strlcpy(names[count++], "Segments", THEME_NAME_MAX);

    DIR *dp = opendir("/spiffs/images/themes");
    if (dp) {
        struct dirent *e;
        while ((e = readdir(dp)) && count < MAX_THEMES) {
            if (e->d_type == DT_DIR && e->d_name[0] != '.') {
                strlcpy(names[count], e->d_name, THEME_NAME_MAX);
                count++;
            }
        }
        closedir(dp);
    }

    /* Insertion sort (small list — no need for qsort overhead) */
    for (int i = 1; i < count; i++) {
        char tmp[THEME_NAME_MAX];
        strlcpy(tmp, names[i], THEME_NAME_MAX);
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) > 0) {
            strlcpy(names[j + 1], names[j], THEME_NAME_MAX);
            j--;
        }
        strlcpy(names[j + 1], tmp, THEME_NAME_MAX);
    }

    /* Build JSON: {"themes":["A","B",...]} */
    /* Max size: 14 (header) + count*(THEME_NAME_MAX+4) + 2 (footer) */
    size_t bufsz = 16 + (size_t)count * (THEME_NAME_MAX + 4);
    char  *buf   = malloc(bufsz);
    if (!buf)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;

    char *p = buf;
    p += snprintf(p, bufsz - (size_t)(p - buf), "{\"themes\":[");
    for (int i = 0; i < count; i++) {
        p += snprintf(p, bufsz - (size_t)(p - buf),
                      "%s\"%s\"", i ? "," : "", names[i]);
    }
    snprintf(p, bufsz - (size_t)(p - buf), "]}");

    esp_err_t ret = send_json(r, buf);
    free(buf);
    return ret;
}

/* GET /api/file/download?path=/images/themes/foo/1.jpg
 * Streams the file as a download attachment. */
static esp_err_t api_file_download(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char q[256], p[256] = {0}, spiffs_path[320];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "path", p, sizeof(p)) != ESP_OK || p[0] == '\0')
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing path"), ESP_FAIL;
    if (!url_decode_inplace(p) || strstr(p, ".."))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid path"), ESP_FAIL;

    snprintf(spiffs_path, sizeof(spiffs_path), "/spiffs%s", p);
    FILE *f = fopen(spiffs_path, "rb");
    if (!f) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found"), ESP_FAIL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    httpd_resp_set_type(r, content_type(p));
    const char *fname = strrchr(p, '/'); fname = fname ? fname + 1 : p;
    /* Sanitize filename for Content-Disposition — reject CR/LF/quotes */
    for (const char *c = fname; *c; c++) {
        if (*c == '\r' || *c == '\n' || *c == '"') {
            fclose(f);
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid filename"), ESP_FAIL;
        }
    }
    char disp[280];   /* 23 ("attachment; filename=\"\"") + 255 (max fname) + NUL */
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);
    httpd_resp_set_hdr(r, "Content-Disposition", disp);
    char clen[24]; snprintf(clen, sizeof(clen), "%ld", sz);
    httpd_resp_set_hdr(r, "Content-Length", clen);

    char *buf = malloc(8192);
    if (!buf) { fclose(f); return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL; }
    size_t rd;
    while ((rd = fread(buf, 1, 8192, f)) > 0) {
        httpd_resp_send_chunk(r, buf, rd);
        vTaskDelay(pdMS_TO_TICKS(1));  /* block 1 tick — taskYIELD() never lets IDLE (pri-0) run */
    }
    httpd_resp_send_chunk(r, NULL, 0);
    free(buf); fclose(f);
    return ESP_OK;
}

/* POST /api/file/upload?path=/audio/click.wav
 * Writes the raw request body to the given SPIFFS path, creating or
 * overwriting the file.  If the destination directory doesn't exist yet,
 * the parent chain is created on demand and the open retried (see
 * fs_mkdir_parents() below) — unlike the old SPIFFS backend, LittleFS has
 * real directories that must exist before fopen(..., "wb") on a file inside
 * them will succeed. */
/* Hard cap on a single upload's Content-Length.  Prevents a slow-trickle or
 * runaway upload from holding a connection slot indefinitely while consuming
 * filesystem space.  2 MB comfortably accommodates audio clips and theme
 * JPEGs; the WebUI patch route has its own dedicated limit. */
#define MAX_UPLOAD_BYTES (2 * 1024 * 1024)

/* Below this much estimated headroom, api_file_upload()'s space check
 * forces a real esp_littlefs_info() rescan instead of trusting its cached
 * running estimate — see that check's own comment for why the estimate
 * exists and how it can drift. */
#define FS_SPACE_CHECK_MARGIN (256 * 1024)

/* fs_mkdir_parents – ensure every directory component of `file_path` exists,
 * creating any that don't (like `mkdir -p $(dirname file_path)`). Needed
 * because the web UI's "upload folder" control preserves each file's
 * webkitRelativePath and uploads straight to its final nested path — e.g. a
 * brand-new theme folder posts .../images/themes/Nixie/Numbers/1.jpg before
 * either directory exists. LittleFS has true directory entries, so
 * fopen(..., "wb") fails with ENOENT if the parent chain isn't already
 * there.
 *
 * ONLY call this after a first fopen() attempt has actually failed with
 * ENOENT — never unconditionally before every upload. Each mkdir() in the
 * chain pays LittleFS's real per-directory-open lookup cost even when the
 * directory already exists, so calling this on every file of a many-file
 * folder upload turns a few KB write into seconds of blocking per file and
 * can trip the IDLE task watchdog partway through a real upload. Gating on
 * ENOENT keeps that cost to once per new directory instead of once per
 * file. */
static void fs_mkdir_parents(const char *file_path)
{
    char buf[320];
    snprintf(buf, sizeof(buf), "%s", file_path);
    for (char *slash = strchr(buf + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        mkdir(buf, 0755);
        *slash = '/';
    }
}

/* True for paths whose images the display caches (theme art, system icons). */
static bool is_theme_asset_path(const char *p)
{
    return strncmp(p, "/images/themes/", 15) == 0 || strncmp(p, "/images/system/", 15) == 0;
}

static esp_err_t api_file_upload(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char q[256], p[256] = {0}, spiffs_path[320];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "path", p, sizeof(p)) != ESP_OK || p[0] == '\0')
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing path"), ESP_FAIL;
    if (!url_decode_inplace(p) || strstr(p, ".."))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid path"), ESP_FAIL;

    /* Reject an empty body up front — otherwise a bodyless POST silently
     * creates a zero-byte file instead of a clear error. */
    if (r->content_len <= 0)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Empty body"), ESP_FAIL;

    /* Hard size cap — reject before opening the destination file so we don't
     * leave a partially-written file behind on rejection. */
    if (r->content_len > MAX_UPLOAD_BYTES)
        return httpd_resp_send_err(r, HTTPD_413_CONTENT_TOO_LARGE,
                                   "File too large (max 2 MB per upload)"), ESP_FAIL;

    snprintf(spiffs_path, sizeof(spiffs_path), "/spiffs%s", p);

    /* Reject the upload if the declared size exceeds available free space.
     *
     * Prefers the cached total/used figures (s_fs_cache_valid, shared with
     * /api/status — see its doc comment) over a fresh esp_littlefs_info()
     * block-walk: that walk measured ~2.7s on this device's file count, and
     * a folder upload calls this handler once per file, so re-walking on
     * every single one turned a many-file theme-folder upload into minutes
     * of pure space-check overhead with no data actually moving. The cache
     * is kept current without a full rescan by nudging `used` up by each
     * upload's byte count below (see there) instead of invalidating it —
     * invalidating here would force the very next file in the same folder
     * upload to pay the same 2.7s walk again, defeating the point.
     *
     * That running estimate can under-count real usage (LittleFS rounds
     * each file up to block-sized allocations, so many small files use
     * more space than their raw byte sum suggests), so it's only trusted
     * while there's comfortable headroom. Within FS_SPACE_CHECK_MARGIN of
     * the requested size — or if nothing has ever been cached yet — this
     * falls back to a real scan to confirm, same display_task-pause
     * handling as before. A genuinely full filesystem is still caught by
     * the fopen()/fwrite() failure path below regardless of how far the
     * estimate has drifted; a pause timeout during the fallback scan skips
     * the check the same way it always did. */
    {
        size_t total = s_fs_total_cached, used = s_fs_used_cached;
        bool have_usage = s_fs_cache_valid;
        if (!have_usage || (total - used) < (size_t)r->content_len + FS_SPACE_CHECK_MARGIN) {
            if (display_pause_for_spi(5000)) {
                esp_littlefs_info("littlefs", &s_fs_total_cached, &s_fs_used_cached);
                display_unpause();
                s_fs_cache_valid = true;
                total = s_fs_total_cached; used = s_fs_used_cached; have_usage = true;
            } else {
                ESP_LOGW(TAG, "fs usage: display task did not pause — skipping pre-upload space check");
            }
        }
        if (have_usage && (size_t)r->content_len > (total - used))
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Not enough space"), ESP_FAIL;
    }

    /* Try the plain open first — the destination directory already exists
     * for the overwhelming majority of uploads (a flat upload, or anything
     * past the first file of a folder upload) — and only pay for
     * fs_mkdir_parents() on an actual ENOENT miss.  See that function's doc
     * comment for why this order matters, not just whether it's correct. */
    FILE *f = fopen(spiffs_path, "wb");
    if (!f && errno == ENOENT) {
        fs_mkdir_parents(spiffs_path);
        f = fopen(spiffs_path, "wb");
    }
    if (!f) {
        /* Diagnostic only — same display_task pause as the two call sites
         * above, same reason. Log without the littlefs totals rather than
         * risk the read if the pause times out; the fopen failure itself is
         * still reported either way. */
        size_t total = 0, used = 0;
        if (display_pause_for_spi(5000)) {
            esp_littlefs_info("littlefs", &total, &used);
            display_unpause();
            ESP_LOGE(TAG, "fopen(%s, wb) failed: errno=%d (%s)  littlefs total=%u used=%u free=%u",
                     spiffs_path, errno, strerror(errno),
                     (unsigned)total, (unsigned)used, (unsigned)(total - used));
        } else {
            ESP_LOGE(TAG, "fopen(%s, wb) failed: errno=%d (%s)  (littlefs usage unavailable — display task did not pause)",
                     spiffs_path, errno, strerror(errno));
        }
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file"), ESP_FAIL;
    }

    char *buf = malloc(8192);
    if (!buf) { fclose(f); return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL; }
    int received = 0, n;
    while ((n = httpd_req_recv(r, buf, 8192)) > 0) {
        fwrite(buf, 1, n, f);
        received += n;
    }
    free(buf); fclose(f);

    if (n < 0) { remove(spiffs_path); return ESP_FAIL; }
    /* Keep the fs-usage cache current without invalidating it — see the
     * space-check comment above for why a full rescan on every file would
     * undo the whole point of caching within one folder upload. Safe to
     * skip when nothing's cached yet (have_usage was false above): there's
     * nothing to keep in sync, and the next reader just populates it fresh. */
    if (s_fs_cache_valid) s_fs_used_cached += (size_t)received;
    ESP_LOGI(TAG, "Uploaded: %s (%d bytes)", spiffs_path, received);
    if (strncmp(p, "/images/album/", 14) == 0)
        display_album_invalidate();
    stock_missing_mark_present(p + 1);   /* p has a leading '/'; cached entries don't */
    if (is_theme_asset_path(p)) display_theme_cache_flush();
    return send_json(r, "{\"status\":\"ok\"}");
}

/* POST /api/file/mkdir?path=/images/themes/MyTheme/Numbers
 * Creates a real directory on the LittleFS partition.
 * LittleFS (unlike SPIFFS) supports true directories via mkdir(). */
static esp_err_t api_file_mkdir(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char q[256], p[256] = {0}, spiffs_path[320];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "path", p, sizeof(p)) != ESP_OK || p[0] == '\0')
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing path"), ESP_FAIL;
    if (!url_decode_inplace(p) || strstr(p, ".."))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid path"), ESP_FAIL;

    /* Strip trailing slash */
    size_t plen = strlen(p);
    if (plen > 0 && p[plen - 1] == '/') p[--plen] = '\0';
    snprintf(spiffs_path, sizeof(spiffs_path), "/spiffs%s", p);

    if (mkdir(spiffs_path, 0755) != 0 && errno != EEXIST)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create dir"), ESP_FAIL;
    ESP_LOGI(TAG, "mkdir: %s", spiffs_path);
    return send_json(r, "{\"status\":\"ok\"}");
}

/* fs_remove_recursive – delete a file or a directory tree rooted at `path`.
 * Works on LittleFS real directories.  Returns 0 on success, -1 on error. */
static int fs_remove_recursive(const char *path)
{
    /* Try removing as a file (or empty dir) first — fast path. */
    if (remove(path) == 0) return 0;

    /* If that failed because it is a non-empty directory, recurse. */
    DIR *dp = opendir(path);
    if (!dp) return -1;

    struct dirent *e;
    char child[320];
    int err = 0;
    while ((e = readdir(dp)) != NULL && err == 0) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        err = fs_remove_recursive(child);
    }
    closedir(dp);

    /* Remove now-empty directory */
    if (err == 0)
        err = (rmdir(path) == 0) ? 0 : -1;
    return err;
}

/* POST /api/file/rename?from=/images/themes/OldName&to=/images/themes/NewName
 * Renames (moves) a file or directory within the LittleFS partition.
 * Both paths are relative to the SPIFFS root (no leading /spiffs).
 * LittleFS supports rename() for both files and directories via VFS. */
static esp_err_t api_file_rename(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char q[512], from[256] = {0}, to[256] = {0};
    char from_path[320], to_path[320];

    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "from", from, sizeof(from)) != ESP_OK ||
        httpd_query_key_value(q, "to",   to,   sizeof(to))   != ESP_OK ||
        from[0] == '\0' || to[0] == '\0')
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing from/to"), ESP_FAIL;

    if (!url_decode_inplace(from) || !url_decode_inplace(to) ||
        strstr(from, "..") || strstr(to, ".."))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid path"), ESP_FAIL;
    if (strcmp(from, "/config.json") == 0)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Protected file"), ESP_FAIL;

    snprintf(from_path, sizeof(from_path), "/spiffs%s", from);
    snprintf(to_path,   sizeof(to_path),   "/spiffs%s", to);

    if (rename(from_path, to_path) != 0) {
        ESP_LOGW(TAG, "rename(%s → %s) failed: %s", from_path, to_path, strerror(errno));
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed"), ESP_FAIL;
    }

    ESP_LOGI(TAG, "renamed: %s → %s", from_path, to_path);
    if (strncmp(from, "/images/album/", 14) == 0 ||
        strncmp(to,   "/images/album/", 14) == 0)
        display_album_invalidate();
    if (is_theme_asset_path(from) || is_theme_asset_path(to)) display_theme_cache_flush();

    return send_json(r, "{\"status\":\"ok\"}");
}

/* DELETE /api/file/delete?path=/audio/click.wav
 * Removes a file or a directory tree.  config.json is protected. */
static esp_err_t api_file_delete(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char q[256], p[256] = {0}, spiffs_path[320];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "path", p, sizeof(p)) != ESP_OK || p[0] == '\0')
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing path"), ESP_FAIL;
    if (!url_decode_inplace(p) || strstr(p, ".."))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid path"), ESP_FAIL;
    if (strcmp(p, "/config.json") == 0)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Protected file"), ESP_FAIL;

    snprintf(spiffs_path, sizeof(spiffs_path), "/spiffs%s", p);
    if (fs_remove_recursive(spiffs_path) != 0)
        return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found or delete failed"), ESP_FAIL;
    fs_usage_invalidate();
    ESP_LOGI(TAG, "Deleted: %s", spiffs_path);
    if (strncmp(p, "/images/album/", 14) == 0)
        display_album_invalidate();
    if (is_theme_asset_path(p)) display_theme_cache_flush();
    return send_json(r, "{\"status\":\"ok\"}");
}

/* ── Mic calibration API ───────────────────────────────────────────── */

/* POST /api/mic/calibrate
 * Runs a MIC_CAL_FRAMES-frame baseline capture (~320 ms) and persists the
 * captured noise floor to config.  Returns the 24 floor values as JSON so
 * the UI can confirm receipt.
 * The mic task must be running (mic_enabled = true).  The capture works from
 * any mode — the mic task bypasses the Spectrum-mode gate when calibrating. */
static esp_err_t api_mic_calibrate(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    float floor_vals[MIC_BAND_COUNT];
    if (!mic_calibrate(floor_vals, 1500)) {
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
            "Calibration timeout — ensure mic is enabled and try again"), ESP_FAIL;
    }

    /* Build a minimal JSON patch and persist it via config_set_json() so the
     * baseline survives reboots without touching the rest of the config. */
    cJSON *patch = cJSON_CreateObject();
    cJSON *arr   = cJSON_AddArrayToObject(patch, "mic_noise_floor");
    for (int i = 0; i < MIC_BAND_COUNT; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)floor_vals[i]));
    cJSON_AddBoolToObject(patch, "mic_calibration_saved", true);
    config_lock();
    uint16_t cal_top_hz = config_get()->mic_top_hz;
    config_unlock();
    cJSON_AddNumberToObject(patch, "mic_floor_top_hz", cal_top_hz | MIC_FLOOR_CHAIN_FLAG);   /* the layout this baseline belongs to */
    char *js = cJSON_PrintUnformatted(patch);
    cJSON_Delete(patch);
    if (js) { config_set_json(js, strlen(js)); free(js); }

    /* Return the captured values so the browser can inspect them */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON *ra = cJSON_AddArrayToObject(resp, "floor");
    for (int i = 0; i < MIC_BAND_COUNT; i++)
        cJSON_AddItemToArray(ra, cJSON_CreateNumber((double)floor_vals[i]));
    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(r, "application/json");
    esp_err_t ret = httpd_resp_sendstr(r, out ? out : "{\"status\":\"ok\"}");
    free(out);
    return ret;
}

/* POST /api/mic/reset_calibration
 * Zeroes the noise floor and restarts Phase 1 auto-calibration (~4 s ramp).
 * Clears the mic_calibration_saved flag so the baseline is not re-applied
 * on the next boot. */
static esp_err_t api_mic_reset_calibration(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    mic_reset_calibration();
    cJSON *patch = cJSON_CreateObject();
    cJSON_AddBoolToObject(patch, "mic_calibration_saved", false);
    char *js = cJSON_PrintUnformatted(patch);
    cJSON_Delete(patch);
    if (js) { config_set_json(js, strlen(js)); free(js); }
    return send_json(r, "{\"status\":\"ok\"}");
}

/* POST /api/social/refresh
 * Immediately wakes the subscribers task to run a fresh poll cycle,
 * bypassing the remaining sleep interval.  Returns {"status":"ok"} instantly;
 * the actual fetch happens asynchronously in the subscribers task. */
static esp_err_t api_social_refresh(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    subscribers_refresh_now();
    return send_json(r, "{\"status\":\"ok\"}");
}

/* POST /api/debug/burnin
 * Body: {"mask": <0–63>, "duration_s": <seconds>, "only_if_idle": <bool>}
 *   mask is a 6-bit field, one bit per tube (bit 0 = tube 1 … bit 5 = tube 6).
 *   0 = restore all tubes to normal rendering.
 *   duration_s: 0 = run until mask=0 is sent; otherwise auto-stops.
 *   only_if_idle: true = don't touch an already-running color-cycle or snow
 *     session (responds {"status":"busy"} instead) — used by Quick Refresh.
 *   stop_all: true = stop any running color-cycle AND snow session (the
 *     Display tab's Cancel button); mask/duration_s are ignored.
 *   pattern: 1-4 = show that calibration test pattern on the masked tubes
 *     instead of the color cycle ({"status":"busy"} if a burn-in or snow
 *     session is running); see display_start_test_pattern().
 * Masked tubes run the color cycle (see display_set_burnin_mask()). */
static esp_err_t api_debug_burnin(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char body[96];
    cJSON *root = read_json_body_small(r, body, sizeof(body));
    if (!root) return ESP_FAIL;
    cJSON *jm = cJSON_GetObjectItem(root, "mask");
    cJSON *jd = cJSON_GetObjectItem(root, "duration_s");
    uint8_t  mask       = cJSON_IsNumber(jm) ? (uint8_t)(jm->valueint & 0x3F) : 0;
    uint32_t duration_s = cJSON_IsNumber(jd) ? (uint32_t)jd->valueint : 0;
    bool     only_idle  = cJSON_IsTrue(cJSON_GetObjectItem(root, "only_if_idle"));
    bool     stop_all   = cJSON_IsTrue(cJSON_GetObjectItem(root, "stop_all"));
    cJSON   *jp         = cJSON_GetObjectItem(root, "pattern");
    uint8_t  pattern    = cJSON_IsNumber(jp) ? (uint8_t)(jp->valueint & 0x07) : 0;
    cJSON_Delete(root);
    if (stop_all) {
        display_stop_burnin();
    } else if (pattern) {
        if (!display_start_test_pattern(mask, duration_s, pattern))
            return send_json(r, "{\"status\":\"busy\"}");
    } else if (only_idle) {
        if (!display_start_burnin_if_idle(mask, duration_s))
            return send_json(r, "{\"status\":\"busy\"}");
    } else {
        display_set_burnin_mask(mask, duration_s);
    }
    return send_json(r, "{\"status\":\"ok\"}");
}

/* POST /api/debug/snow
 * Body: {"mask": <0–63>, "duration_s": <seconds>}
 *   mask:       6-bit field, bit N = tube N.  0 = stop immediately.
 *   duration_s: 0 = run until mask=0 is sent.
 * Fills masked tubes with random RGB565 pixels every display tick (5 Hz),
 * exercising every individual pixel address independently. */
static esp_err_t api_debug_snow(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char body[64];
    cJSON *root = read_json_body_small(r, body, sizeof(body));
    if (!root) return ESP_FAIL;
    cJSON *jm = cJSON_GetObjectItem(root, "mask");
    cJSON *jd = cJSON_GetObjectItem(root, "duration_s");
    uint8_t  mask       = cJSON_IsNumber(jm) ? (uint8_t)(jm->valueint & 0x3F) : 0;
    uint32_t duration_s = cJSON_IsNumber(jd) ? (uint32_t)jd->valueint : 0;
    cJSON_Delete(root);
    display_set_snow_mask(mask, duration_s);
    return send_json(r, "{\"status\":\"ok\"}");
}

/* POST /api/update_notify
 * Body: {"active":true}  — draw the 4-row red update indicator on tube 6
 *       {"active":false} — clear the indicator
 *
 * Called by the web UI's update-check logic when:
 *   - an update is detected AND the user has enabled "clock face update
 *     notification" in Display settings  → active=true
 *   - the update toast is dismissed or the feature is disabled → active=false
 *
 * The indicator is rendered by display_show_digit() on every frame; it does
 * not require a display-task restart.  The state is volatile in RAM only —
 * it resets to false on reboot (expected, since a new boot re-runs the
 * update check). */
static esp_err_t api_update_notify(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char body[64];
    cJSON *root = read_json_body_small(r, body, sizeof(body));
    if (!root) return ESP_FAIL;
    const cJSON *ja = cJSON_GetObjectItem(root, "active");
    bool active = cJSON_IsTrue(ja);
    cJSON_Delete(root);
    display_set_update_indicator(active);
    ESP_LOGI(TAG, "update_notify: clock-face indicator %s", active ? "ON" : "OFF");
    return send_json(r, "{\"status\":\"ok\"}");
}

/* POST /api/rotation_pause
 * Body: {"paused": false}                     -> resume immediately
 *       {"paused": true}                      -> pause indefinitely, until resumed
 *       {"paused": true, "duration_s": 3600}   -> pause for N seconds, then
 *                                                 auto-resume on its own
 * Pauses/resumes automatic mode rotation without touching config_mgr or
 * flash — see display_set_rotation_paused(). Runtime only: any pause
 * (timed or indefinite) always clears on reboot; the persisted Mode
 * Rotation setting itself (enabled/interval/weights) is untouched. */
static esp_err_t api_rotation_pause(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char body[64];
    cJSON *root = read_json_body_small(r, body, sizeof(body));
    if (!root) return ESP_FAIL;
    cJSON *jp = cJSON_GetObjectItem(root, "paused");
    cJSON *jd = cJSON_GetObjectItem(root, "duration_s");
    bool     paused     = cJSON_IsTrue(jp);
    uint32_t duration_s = cJSON_IsNumber(jd) ? (uint32_t)jd->valueint : 0;
    cJSON_Delete(root);
    display_set_rotation_paused(paused, duration_s);
    ESP_LOGI(TAG, "rotation_pause: %s", paused
             ? (duration_s ? "paused (timed)" : "paused (indefinite)")
             : "resumed");
    return send_json(r, "{\"status\":\"ok\"}");
}

/* POST /api/cx_image?tube=5|6  — body = a JPG, exactly 80×160 px.
 * Pushes the image to the 24H_CX tube-5/6 "Pushed image" info panel so an
 * external script can drive that tube (asset/base themes only — WeatherLive
 * renders its own panels procedurally).  The image shows whenever that tube's
 * Pushed-image panel is the active rotation slot; it persists until the next
 * push or reboot.  tube=6 = rightmost (LCD 5); tube=5 = 2nd-from-right (LCD 4,
 * only visible in dual-panel mode). */
#define CX_IMAGE_MAX_BYTES  (96 * 1024)   /* generous cap for an 80×160 JPG */
static esp_err_t api_cx_image(httpd_req_t *r)
{
    REQUIRE_AUTH(r);

    /* Resolve target tube from ?tube=5|6 → which (0 = tube5/LCD4, 1 = tube6/LCD5). */
    int which = -1;
    char q[32], v[8];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "tube", v, sizeof(v)) == ESP_OK) {
        if      (strcmp(v, "6") == 0) which = 1;
        else if (strcmp(v, "5") == 0) which = 0;
    }
    if (which < 0)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "tube=5 or tube=6 required"), ESP_FAIL;

    int len = (int)r->content_len;
    if (len <= 0 || len > CX_IMAGE_MAX_BYTES)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "JPG body required (1..98304 B)"), ESP_FAIL;

    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;
    int rx = 0;
    while (rx < len) {
        int n = httpd_req_recv(r, (char *)buf + rx, (size_t)(len - rx));
        if (n <= 0) { heap_caps_free(buf); return ESP_FAIL; }
        rx += n;
    }

    bool ok = display_cx_push_image(which, buf, (size_t)len);
    heap_caps_free(buf);
    if (!ok)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "decode failed — image must be an 80x160 JPEG"), ESP_FAIL;
    ESP_LOGI(TAG, "cx_image: pushed %d B to tube %d", len, which ? 6 : 5);
    return send_json(r, "{\"status\":\"ok\"}");
}

/* ── Hardware debug API ────────────────────────────────────────────── */
/* GET /api/debug/adc
 * Reads one raw 12-bit ADC sample from the currently configured mic channel.
 * Intended for the hidden debug panel — use while NOT in Spectrum mode so
 * the mic_task is gated and not simultaneously reading the ADC.
 * Response: {"channel":0,"gpio":36,"raw":2048,"voltage_mv":1650} */
static esp_err_t api_debug_adc(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    config_lock();
    uint8_t ch = config_get()->mic_adc_channel;
    config_unlock();
    if (ch >= 8) ch = 0;
    int     gpio = mic_gpio_num();
    int     raw  = mic_read_raw();   /* -1 if mic not initialised */

    /* Convert 12-bit reading to millivolts (ADC_ATTEN_DB_12 → 0-3300 mV) */
    int mv = (raw >= 0) ? (int)((raw * 3300L) / 4095) : -1;

    char buf[128];
    if (raw < 0)
        snprintf(buf, sizeof(buf),
                 "{\"channel\":%d,\"gpio\":%d,\"raw\":null,\"voltage_mv\":null,"
                 "\"error\":\"mic not initialised - enable mic and reboot\"}",
                 ch, gpio);
    else
        snprintf(buf, sizeof(buf),
                 "{\"channel\":%d,\"gpio\":%d,\"raw\":%d,\"voltage_mv\":%d}",
                 ch, gpio, raw, mv);

    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, buf);
}

/* POST /api/debug/dac
 * Body: {"mode":"tone","freq_hz":1000,"amplitude":64}
 *       {"mode":"dc","level":200}
 *       {"mode":"silence"}  |  {"mode":"hiz"}  |  {"mode":"normal"}
 *
 * Injects a test signal on GPIO25 (DAC CH0) for hardware noise diagnostics.
 * "normal" restores idle behaviour.  Any active audio playback is stopped. */
static esp_err_t api_debug_dac(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char body[256];
    cJSON *root = read_json_body_small(r, body, sizeof(body));
    if (!root) return ESP_FAIL;

    const cJSON *jmode = cJSON_GetObjectItem(root, "mode");
    /* Copy mode string NOW — cJSON_Delete(root) below frees jmode->valuestring */
    char mode_buf[32] = "normal";
    if (cJSON_IsString(jmode))
        snprintf(mode_buf, sizeof(mode_buf), "%s", jmode->valuestring);

    int param_a = 0, param_b = 0;
    const cJSON *jfreq = cJSON_GetObjectItem(root, "freq_hz");
    const cJSON *jamp  = cJSON_GetObjectItem(root, "amplitude");
    const cJSON *jlev  = cJSON_GetObjectItem(root, "level");
    if (cJSON_IsNumber(jlev))  param_a = (int)jlev->valueint;   /* "dc" level   */
    if (cJSON_IsNumber(jfreq)) param_a = (int)jfreq->valueint;  /* "tone" freq  */
    if (cJSON_IsNumber(jamp))  param_b = (int)jamp->valueint;   /* "tone" amp   */
    cJSON_Delete(root);

    audio_dac_test_set(mode_buf, param_a, param_b);
    return send_json(r, "{\"status\":\"ok\"}");
}

/* POST /api/debug/pwm
 * Body: {"freq_hz":10000,"brightness_pct":80}  — apply custom freq + duty
 *       {"restore":true}                        — restore 50 kHz (task corrects duty)
 *
 * Adjusts LEDC_TIMER_0 / LEDC_CHANNEL_0 (backlight GPIO) at runtime to
 * find a PWM frequency that minimises coupling into the DAC output. */
static esp_err_t api_debug_pwm(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char body[256];
    cJSON *root = read_json_body_small(r, body, sizeof(body));
    if (!root) return ESP_FAIL;

    const cJSON *jrestore = cJSON_GetObjectItem(root, "restore");
    if (cJSON_IsTrue(jrestore)) {
        cJSON_Delete(root);
        display_debug_restore_pwm();
        return send_json(r, "{\"status\":\"ok\",\"action\":\"restored\"}");
    }

    const cJSON *jfreq = cJSON_GetObjectItem(root, "freq_hz");
    const cJSON *jbrt  = cJSON_GetObjectItem(root, "brightness_pct");
    uint32_t freq_hz  = cJSON_IsNumber(jfreq) ? (uint32_t)(int)jfreq->valueint : 50000;
    uint8_t  duty_pct = cJSON_IsNumber(jbrt)  ? (uint8_t)(int)jbrt->valueint   : 50;
    cJSON_Delete(root);

    display_debug_set_pwm(freq_hz, duty_pct);
    return send_json(r, "{\"status\":\"ok\"}");
}

/* POST /api/debug/loglevel  — runtime per-tag log verbosity (esp_log_level_set).
 * Body: { "tag": "weather", "enabled": false }   → silence that subsystem
 *    or { "tag": "*",       "level": 3 }          → set a level explicitly
 * level: 0=NONE 1=ERROR 2=WARN 3=INFO 4=DEBUG 5=VERBOSE.  "*" sets the global
 * default.  Runtime only — NOT persisted, resets on reboot.  Handy for getting
 * a clean long log of one subsystem (e.g. ntp) without others' chatter. */
static esp_err_t api_debug_loglevel(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char buf[128];
    cJSON *root = read_json_body_small(r, buf, sizeof(buf));
    if (!root) return ESP_FAIL;

    cJSON *jt = cJSON_GetObjectItem(root, "tag");
    if (!cJSON_IsString(jt) || jt->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing tag"), ESP_FAIL;
    }

    esp_log_level_t lvl;
    cJSON *je = cJSON_GetObjectItem(root, "enabled");
    cJSON *jl = cJSON_GetObjectItem(root, "level");
    if (cJSON_IsBool(je)) {
        lvl = cJSON_IsTrue(je) ? ESP_LOG_INFO : ESP_LOG_NONE;
    } else if (cJSON_IsNumber(jl)) {
        int v = jl->valueint;
        if (v < 0) v = 0;
        if (v > 5) v = 5;
        lvl = (esp_log_level_t)v;
    } else {
        cJSON_Delete(root);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Need 'enabled' or 'level'"), ESP_FAIL;
    }

    /* esp_log_level_set() copies the tag string into its cache, so the
     * transient buffer is safe. */
    esp_log_level_set(jt->valuestring, lvl);
    ESP_LOGW("web_srv", "log level: tag='%s' -> %d", jt->valuestring, (int)lvl);
    cJSON_Delete(root);
    return send_json(r, "{\"status\":\"ok\"}");
}

/* POST /api/debug/wl_fps — runtime WeatherLive animation-rate override.
 * Body: { "fps": 30 }  → run animated WeatherLive at 30 Hz instead of the
 *   normal 10/20 Hz split (Spectrum mode is exempt, see display_set_debug_wl_fps).
 *   { "fps": 0 }        → disable, restore default behavior.
 * Runtime only — NOT persisted, resets on reboot. */
static esp_err_t api_debug_wl_fps(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char buf[64];
    cJSON *root = read_json_body_small(r, buf, sizeof(buf));
    if (!root) return ESP_FAIL;

    cJSON *jf = cJSON_GetObjectItem(root, "fps");
    if (!cJSON_IsNumber(jf)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing fps"), ESP_FAIL;
    }
    int fps = jf->valueint;
    cJSON_Delete(root);
    if (fps != 0 && (fps < 5 || fps > 40))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "fps must be 0 or 5-40"), ESP_FAIL;

    display_set_debug_wl_fps(fps);
    return send_json(r, "{\"status\":\"ok\"}");
}

/* POST /api/debug/stacklog — toggle the per-task stack high-water-mark dump
 * that main.c's heap_telemetry_task() emits alongside its 5-minute heap line
 * (see log_task_stacks() in main.c). Off by default: it's one ESP_LOGI per
 * task (~25 tasks) every 5 minutes, useful while chasing a specific stack
 * size but pure noise otherwise. Body: { "enabled": true }.
 * Runtime only — NOT persisted, resets to off on reboot. */
static esp_err_t api_debug_stacklog(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char buf[32];
    cJSON *root = read_json_body_small(r, buf, sizeof(buf));
    if (!root) return ESP_FAIL;

    cJSON *je = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsBool(je)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing enabled"), ESP_FAIL;
    }
    s_debug_stacklog = cJSON_IsTrue(je);
    cJSON_Delete(root);
    return send_json(r, "{\"status\":\"ok\"}");
}

bool web_server_debug_stacklog_enabled(void)
{
    return s_debug_stacklog;
}

/* GET /api/debug/micbands — per-band pipeline snapshot: raw band energy,
 * noise floor, post-tilt power and final normalised display value for all
 * 24 bands.  Shows which processing stage eats a missing signal.  Capture
 * must be running (Spectrum mode on screen). */
static esp_err_t api_debug_micbands(httpd_req_t *r)
{
    REQUIRE_AUTH(r);

    float raw[MIC_BAND_COUNT], floor_[MIC_BAND_COUNT];
    float power[MIC_BAND_COUNT], bands[MIC_BAND_COUNT];
    mic_get_band_debug(raw, floor_, power, bands);
    float band_f[MIC_BAND_COUNT];
    bool  band_m[MIC_BAND_COUNT];
    mic_get_band_freqs(band_f);
    mic_get_band_masked(band_m);

    size_t cap = MIC_BAND_COUNT * 112 + 420;
    mic_stats_t st;
    mic_get_stats(&st);
    char  *out = malloc(cap);
    if (!out) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;

    size_t off = (size_t)snprintf(out, cap, "{\"stats\":{\"frames\":%lu,\"fail\":%lu,\"gated\":%lu,\"holds\":%lu,\"adc\":%d,\"gv\":%.1f,\"gt\":%.1f,\"ge\":%lu,\"go\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu]},\"bands\":[",
                                  (unsigned long)st.frames, (unsigned long)st.read_fail, (unsigned long)st.gated,
                                  (unsigned long)st.spike_holds, st.adc_last, (double)st.gate_val, (double)st.gate_thr,
                                  (unsigned long)st.gate_eval,
                                  (unsigned long)st.gate_open[0], (unsigned long)st.gate_open[1], (unsigned long)st.gate_open[2],
                                  (unsigned long)st.gate_open[3], (unsigned long)st.gate_open[4], (unsigned long)st.gate_open[5],
                                  (unsigned long)st.gate_open[6], (unsigned long)st.gate_open[7], (unsigned long)st.gate_open[8]);
    for (int b = 0; b < MIC_BAND_COUNT; b++)
        off += (size_t)snprintf(out + off, cap - off,
            "%s{\"i\":%d,\"f\":%.0f,\"m\":%d,\"raw\":%.3f,\"floor\":%.3f,\"power\":%.3f,\"disp\":%.3f}",
            b ? "," : "", b, (double)band_f[b], band_m[b] ? 1 : 0, raw[b], floor_[b], power[b], bands[b]);
    off += (size_t)snprintf(out + off, cap - off, "]}");

    httpd_resp_set_type(r, "application/json");
    esp_err_t err = httpd_resp_send(r, out, (ssize_t)off);
    free(out);
    return err;
}

/* GET /api/debug/micframe — export one raw 32 kHz mic capture frame
 * (512 samples, pre-decimation) for offline waveform/spectrum analysis.
 * Requires Spectrum mode to be on screen (capture must be running). */
static esp_err_t api_debug_micframe(httpd_req_t *r)
{
    REQUIRE_AUTH(r);

    uint16_t *frame = malloc(MIC_RAW_FRAME_SAMPLES * sizeof(uint16_t));
    float    *dec   = malloc(MIC_FRAME_SAMPLES * sizeof(float));
    if (!frame || !dec) {
        free(frame); free(dec);
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;
    }

    if (!mic_capture_frame_pair(frame, dec, 1500)) {
        free(frame); free(dec);
        httpd_resp_set_status(r, "503 Service Unavailable");
        return httpd_resp_sendstr(r,
            "{\"error\":\"capture not running - switch to Spectrum mode (mic enabled, no audio playing)\"}");
    }

    /* raw ~6 chars/sample + decimated ~10 chars/sample + wrapper */
    size_t cap = MIC_RAW_FRAME_SAMPLES * 6 + MIC_FRAME_SAMPLES * 12 + 96;
    char  *out = malloc(cap);
    if (!out) { free(frame); free(dec); return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL; }

    size_t off = (size_t)snprintf(out, cap, "{\"rate_hz\":32000,\"n\":%d,\"samples\":[",
                                  MIC_RAW_FRAME_SAMPLES);
    for (int i = 0; i < MIC_RAW_FRAME_SAMPLES && off < cap - 8; i++)
        off += (size_t)snprintf(out + off, cap - off, "%s%u", i ? "," : "",
                                (unsigned)frame[i]);
    /* Decimated/DC-removed values from the SAME frame — the exact Goertzel
     * input (pre-window).  Lets offline analysis compare both views of one
     * frame and isolate the decimation stage. */
    off += (size_t)snprintf(out + off, cap - off, "],\"dec_rate_hz\":16000,\"dec\":[");
    for (int i = 0; i < MIC_FRAME_SAMPLES && off < cap - 8; i++)
        off += (size_t)snprintf(out + off, cap - off, "%s%.2f", i ? "," : "",
                                (double)dec[i]);
    off += (size_t)snprintf(out + off, cap - off, "]}");
    free(frame); free(dec);

    httpd_resp_set_type(r, "application/json");
    esp_err_t err = httpd_resp_send(r, out, (ssize_t)off);
    free(out);
    return err;
}

/* POST /api/debug/mictest — body {"on":true,"duration_s":1200}
 * Keeps the mic capture + analysis running in any mode while the display idles
 * blank. In Spectrum mode the 20 Hz drawing keeps the CPU busy enough that this
 * web server takes seconds per reply, which makes long mic measurements unusable;
 * this removes the drawing but keeps the mic. Auto-stops (default 20 min, max 1 h).
 * Used by tools/led_noise_sweep.py. */
static esp_err_t api_debug_mictest(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char body[64];
    cJSON *root = read_json_body_small(r, body, sizeof(body));
    if (!root) return ESP_FAIL;
    bool     on  = cJSON_IsTrue(cJSON_GetObjectItem(root, "on"));
    cJSON   *jd  = cJSON_GetObjectItem(root, "duration_s");
    uint32_t dur = cJSON_IsNumber(jd) && jd->valueint > 0 ? (uint32_t)jd->valueint : 0;
    cJSON_Delete(root);
    mic_set_test_capture(on, dur);
    return send_json(r, on ? "{\"status\":\"on\"}" : "{\"status\":\"off\"}");
}

/* GET/POST /api/debug/mictune — Spectrum / mic constants, live (runtime only, reset on reboot).
 * GET  → {"fields":[{"k":"eq","cur":0.6,"def":0.6,"lo":0,"hi":1}, ...]}
 * POST → {"eq":0.8, ...} sets any subset by key (clamped); {"reset":true} restores the compiled defaults.
 * Used by the hidden diagnostics panel. */
static esp_err_t api_debug_mictune(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    const mic_tune_desc_t *dsc = mic_tune_desc();
    float cur[MT_COUNT];
    mic_tune_get(cur);
    if (r->method == HTTP_POST) {
        char body[512];
        cJSON *root = read_json_body_small(r, body, sizeof(body));
        if (!root) return ESP_FAIL;
        if (cJSON_IsTrue(cJSON_GetObjectItem(root, "reset"))) {
            mic_tune_reset();
        } else {
            for (int i = 0; i < MT_COUNT; i++) {
                cJSON *j = cJSON_GetObjectItem(root, dsc[i].key);
                if (cJSON_IsNumber(j)) cur[i] = (float)j->valuedouble;
            }
            mic_tune_set(cur);
        }
        cJSON_Delete(root);
        mic_tune_get(cur);
    }
    const size_t cap = 160 + MT_COUNT * 112;
    char *out = malloc(cap);
    if (!out) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;
    size_t off = (size_t)snprintf(out, cap, "{\"fields\":[");
    for (int i = 0; i < MT_COUNT && off < cap; i++)
        off += (size_t)snprintf(out + off, cap - off, "%s{\"k\":\"%s\",\"cur\":%.5g,\"def\":%.5g,\"lo\":%.5g,\"hi\":%.5g}",
                                i ? "," : "", dsc[i].key, (double)cur[i], (double)dsc[i].def, (double)dsc[i].lo, (double)dsc[i].hi);
    if (off < cap) snprintf(out + off, cap - off, "]}");
    esp_err_t e = send_json(r, out);
    free(out);
    return e;
}

/* POST /api/debug/mictrace {"frames":625} — record the raw energy of all 24 bands for the next N mic frames (16 ms each).
 * GET  /api/debug/mictrace — {"active":true,...} while recording; once done, streams
 *   {"n":N,"floor":[24],"frames":[[24 sqrt-companded values: raw=(v/100)^2], ...]} and frees the buffer.
 * For offline replay of gate / threshold designs on real data. Needs capture running (Spectrum mode). */
static esp_err_t api_debug_mictrace(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    if (r->method == HTTP_POST) {
        char body[64];
        cJSON *root = read_json_body_small(r, body, sizeof(body));
        if (!root) return ESP_FAIL;
        cJSON *jf = cJSON_GetObjectItem(root, "frames");
        int fr = cJSON_IsNumber(jf) ? jf->valueint : 625;
        cJSON_Delete(root);
        return send_json(r, mic_trace_start(fr) ? "{\"status\":\"recording\"}" : "{\"status\":\"no memory\"}");
    }
    int n, cap; bool active;
    mic_trace_info(&n, &cap, &active);
    if (active || n <= 0) {
        char o[96];
        snprintf(o, sizeof(o), "{\"active\":%s,\"n\":%d,\"cap\":%d}", active ? "true" : "false", n, cap);
        return send_json(r, o);
    }
    const size_t BUF = 1600;
    char *buf = malloc(BUF);
    if (!buf) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;
    float fl[MIC_BAND_COUNT];
    mic_trace_floor(fl);
    httpd_resp_set_type(r, "application/json");
    size_t len = (size_t)snprintf(buf, BUF, "{\"active\":false,\"n\":%d,\"floor\":[", n);
    for (int b = 0; b < MIC_BAND_COUNT; b++)
        len += (size_t)snprintf(buf + len, BUF - len, "%s%.4f", b ? "," : "", (double)fl[b]);
    len += (size_t)snprintf(buf + len, BUF - len, "],\"frames\":[");
    esp_err_t e = ESP_OK;
    for (int i = 0; i < n && e == ESP_OK; i++) {
        uint16_t row[MIC_BAND_COUNT];
        if (!mic_trace_read(i, row)) break;
        if (len + 200 > BUF) { e = httpd_resp_send_chunk(r, buf, (ssize_t)len); len = 0; }
        len += (size_t)snprintf(buf + len, BUF - len, "%s[", i ? "," : "");
        for (int b = 0; b < MIC_BAND_COUNT; b++)
            len += (size_t)snprintf(buf + len, BUF - len, "%s%u", b ? "," : "", (unsigned)row[b]);
        len += (size_t)snprintf(buf + len, BUF - len, "]");
    }
    if (e == ESP_OK) { len += (size_t)snprintf(buf + len, BUF - len, "]}"); e = httpd_resp_send_chunk(r, buf, (ssize_t)len); }
    if (e == ESP_OK) httpd_resp_send_chunk(r, NULL, 0);
    free(buf);
    mic_trace_free();
    return e;
}

/* GET /api/debug/tasks — per-task CPU accounting (FreeRTOS runtime stats).
 *
 * `pct` is an **instantaneous** share measured over a short window inside the
 * handler: two uxTaskGetSystemState() snapshots ~250 ms apart, diffed per task
 * (matched by handle) against the wall-clock window.  This is robust to the
 * 32-bit runtime-counter wrap (~71 min) — an earlier "lifetime" pct (run/total)
 * produced garbage >100% once the counters wrapped.  pct is per-chip: both
 * cores accrue, so the sum across all tasks ≈ 200% (IDLE0+IDLE1 absorb slack).
 * `run_us` is still the raw 32-bit lifetime counter (µs) for manual diffing. */
/* GET /api/debug/heap — dump the per-region internal-heap breakdown to the
 * serial console.
 *
 * This is the view that distinguishes "the heap is FRAGMENTED" from "the
 * heap is FULL", which look identical from the single largest-free-block
 * number in /api/status. The discriminator is free_blocks per region: many
 * small free blocks scattered among allocations means fragmentation; a
 * handful of free blocks against hundreds of allocated ones means simply
 * exhausted and packed tight. It also shows WHICH of the six physical
 * regions the free space is stranded in — they're non-contiguous address
 * ranges, so free space in one can never satisfy an allocation that has to
 * come from another, which an aggregate "free bytes" figure hides.
 *
 * Output goes to UART only: heap_caps_print_heap_info() writes via printf,
 * and the web UI's log ring hooks ESP_LOG's vprintf, not stdout. Watch the
 * serial console when calling this. */
static esp_err_t api_debug_heap(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    printf("\n===== INTERNAL (MALLOC_CAP_INTERNAL) =====\n");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    /* DMA-capable internal RAM is a strict SUBSET of the above, and it is the
     * pool that actually constrains the DAC ring, the mic's adc_continuous
     * buffers and display SPI — none of which the INTERNAL figures predict.
     * Measured divergence in the field: internal largest 11776 B while DMA
     * largest was 1536 B at the same moment. Dump it separately or the real
     * constraint stays invisible. */
    printf("\n===== DMA-CAPABLE (MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA) =====\n");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    char out[256];
    snprintf(out, sizeof(out),
             "{\"status\":\"printed to serial console\","
             "\"internal_free\":%u,\"internal_largest\":%u,"
             "\"dma_free\":%u,\"dma_largest\":%u}",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return send_json(r, out);
}

static esp_err_t api_debug_tasks(httpd_req_t *r)
{
    REQUIRE_AUTH(r);

    UBaseType_t cap_n = uxTaskGetNumberOfTasks() + 8;   /* +headroom for spawns */
    TaskStatus_t *ts1 = malloc(cap_n * sizeof(TaskStatus_t));
    TaskStatus_t *ts2 = malloc(cap_n * sizeof(TaskStatus_t));
    if (!ts1 || !ts2) { free(ts1); free(ts2);
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL; }

    /* Snapshot, wait a fixed window, snapshot again. */
    int64_t     t1 = esp_timer_get_time();
    UBaseType_t n1 = uxTaskGetSystemState(ts1, cap_n, NULL);
    vTaskDelay(pdMS_TO_TICKS(250));
    int64_t     t2 = esp_timer_get_time();
    UBaseType_t n2 = uxTaskGetSystemState(ts2, cap_n, NULL);
    int64_t     wall = t2 - t1;
    if (n2 == 0 || wall <= 0) { free(ts1); free(ts2);
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "runtime stats unavailable"), ESP_FAIL; }

    /* ~110 B per task worst case + wrapper */
    size_t cap = (size_t)n2 * 120 + 96;
    char  *out = malloc(cap);
    if (!out) { free(ts1); free(ts2);
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL; }

    static const char *k_state[] = { "run", "ready", "blocked", "suspended", "deleted", "invalid" };
    size_t off = (size_t)snprintf(out, cap, "{\"window_us\":%lld,\"tasks\":[",
                                  (long long)wall);
    for (UBaseType_t i = 0; i < n2 && off < cap - 2; i++) {
        /* Match this task's previous counter by handle (robust to reordering /
         * task set changes), then diff.  Unsigned 32-bit subtraction is correct
         * across a single counter wrap; a 250 ms window can never itself wrap. */
        uint32_t prev = 0; bool found = false;
        for (UBaseType_t j = 0; j < n1; j++) {
            if (ts1[j].xHandle == ts2[i].xHandle) { prev = ts1[j].ulRunTimeCounter; found = true; break; }
        }
        uint32_t delta = found ? (ts2[i].ulRunTimeCounter - prev) : 0u;
        uint32_t pct10 = (uint32_t)(((uint64_t)delta * 1000) / (uint64_t)wall);
        int state = (int)ts2[i].eCurrentState;
        if (state < 0 || state > 5) state = 5;
        off += (size_t)snprintf(out + off, cap - off,
            "%s{\"name\":\"%s\",\"prio\":%u,\"core\":%d,\"state\":\"%s\","
            "\"stack_hwm\":%u,\"run_us\":%u,\"pct\":%u.%u}",
            i ? "," : "",
            ts2[i].pcTaskName,
            (unsigned)ts2[i].uxCurrentPriority,
            (int)ts2[i].xCoreID == INT32_MAX ? -1 : (int)ts2[i].xCoreID,
            k_state[state],
            (unsigned)ts2[i].usStackHighWaterMark,
            (unsigned)ts2[i].ulRunTimeCounter,
            (unsigned)(pct10 / 10), (unsigned)(pct10 % 10));
    }
    off += (size_t)snprintf(out + off, cap - off, "]}");
    free(ts1); free(ts2);

    httpd_resp_set_type(r, "application/json");
    esp_err_t err = httpd_resp_send(r, out, (ssize_t)off);
    free(out);
    return err;
}

/* ── Windowed task CPU accounting ─────────────────────────────────────────
 * api_debug_tasks()'s 250 ms window reports whatever happened to be running
 * in that quarter-second — useless for judging something like "is polling
 * or queued SPI actually better" where the interesting behaviour (rain
 * frames, mode switches, WiFi bursts) is spread over minutes. Since
 * FreeRTOS's per-task runtime counters are cumulative, a fair average over
 * an arbitrary window doesn't need repeated sampling — one snapshot at the
 * start, one at the end, one diff. Cost is two uxTaskGetSystemState() calls
 * total for the whole window, so it can't meaningfully perturb what it's
 * measuring. Safe for any window under the ~71 min 32-bit counter wrap,
 * same reasoning as the 250 ms version's comment above. */
static TaskStatus_t *s_task_snap_start   = NULL;
static UBaseType_t   s_task_snap_start_n = 0;
static int64_t       s_task_snap_start_t = 0;

/* POST /api/debug/tasks_start — snapshot now; call /api/debug/tasks_result
 * later (any time after, doesn't consume/end the window) to read the
 * average over the elapsed period. A second _start call discards whatever
 * window was already running and begins a fresh one. */
static esp_err_t api_debug_tasks_start(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    UBaseType_t cap_n = uxTaskGetNumberOfTasks() + 8;
    TaskStatus_t *ts = malloc(cap_n * sizeof(TaskStatus_t));
    if (!ts) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;

    int64_t     t   = esp_timer_get_time();
    UBaseType_t n   = uxTaskGetSystemState(ts, cap_n, NULL);
    if (n == 0) { free(ts);
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "runtime stats unavailable"), ESP_FAIL; }

    free(s_task_snap_start);   /* no-op if NULL — discard any prior window */
    s_task_snap_start   = ts;
    s_task_snap_start_n = n;
    s_task_snap_start_t = t;
    ESP_LOGI("web_srv", "task-stats window started (%u tasks)", (unsigned)n);
    return send_json(r, "{\"status\":\"ok\"}");
}

/* GET /api/debug/tasks_result — same response shape as /api/debug/tasks,
 * averaged over the whole window since the last _start call instead of
 * 250 ms. Repeatable — does not end or reset the window. */
static esp_err_t api_debug_tasks_result(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    if (!s_task_snap_start)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "No window started — POST /api/debug/tasks_start first"), ESP_FAIL;

    UBaseType_t cap_n = uxTaskGetNumberOfTasks() + 8;
    TaskStatus_t *ts2 = malloc(cap_n * sizeof(TaskStatus_t));
    if (!ts2) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;

    int64_t     t2   = esp_timer_get_time();
    UBaseType_t n2    = uxTaskGetSystemState(ts2, cap_n, NULL);
    int64_t     wall = t2 - s_task_snap_start_t;
    if (n2 == 0 || wall <= 0) { free(ts2);
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "runtime stats unavailable"), ESP_FAIL; }

    size_t cap = (size_t)n2 * 120 + 96;
    char  *out = malloc(cap);
    if (!out) { free(ts2);
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL; }

    static const char *k_state[] = { "run", "ready", "blocked", "suspended", "deleted", "invalid" };
    size_t off = (size_t)snprintf(out, cap, "{\"window_us\":%lld,\"tasks\":[",
                                  (long long)wall);
    for (UBaseType_t i = 0; i < n2 && off < cap - 2; i++) {
        uint32_t prev = 0; bool found = false;
        for (UBaseType_t j = 0; j < s_task_snap_start_n; j++) {
            if (s_task_snap_start[j].xHandle == ts2[i].xHandle) {
                prev = s_task_snap_start[j].ulRunTimeCounter; found = true; break;
            }
        }
        uint32_t delta = found ? (ts2[i].ulRunTimeCounter - prev) : 0u;
        uint32_t pct10 = (uint32_t)(((uint64_t)delta * 1000) / (uint64_t)wall);
        int state = (int)ts2[i].eCurrentState;
        if (state < 0 || state > 5) state = 5;
        off += (size_t)snprintf(out + off, cap - off,
            "%s{\"name\":\"%s\",\"prio\":%u,\"core\":%d,\"state\":\"%s\","
            "\"stack_hwm\":%u,\"run_us\":%u,\"pct\":%u.%u,\"new\":%s}",
            i ? "," : "",
            ts2[i].pcTaskName,
            (unsigned)ts2[i].uxCurrentPriority,
            (int)ts2[i].xCoreID == INT32_MAX ? -1 : (int)ts2[i].xCoreID,
            k_state[state],
            (unsigned)ts2[i].usStackHighWaterMark,
            (unsigned)ts2[i].ulRunTimeCounter,
            (unsigned)(pct10 / 10), (unsigned)(pct10 % 10),
            found ? "false" : "true");
    }
    off += (size_t)snprintf(out + off, cap - off, "]}");
    free(ts2);

    httpd_resp_set_type(r, "application/json");
    esp_err_t err = httpd_resp_send(r, out, (ssize_t)off);
    free(out);
    return err;
}

/* ── Log ring API ──────────────────────────────────────────────────── */
/* GET /api/logs  → {"lines":["I (12) tag: msg", ...]}  chronological  */
static esp_err_t api_get_logs(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "lines");

    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        int count = s_log_count;
        /* oldest entry when the buffer has wrapped */
        int start = (count < LOG_RING_LINES) ? 0 : s_log_head;
        for (int i = 0; i < count; i++) {
            int idx = (start + i) % LOG_RING_LINES;
            cJSON_AddItemToArray(arr, cJSON_CreateString(s_log_ring[idx]));
        }
        xSemaphoreGive(s_log_mutex);
    }

    char *json = cJSON_PrintUnformatted(root);
    esp_err_t ret = send_json(r, json);
    free(json); cJSON_Delete(root);
    return ret;
}

/* POST /api/logs/clear  → clears the in-RAM ring buffer only */
static esp_err_t api_clear_logs(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        s_log_head  = 0;
        s_log_count = 0;
        xSemaphoreGive(s_log_mutex);
    }
    return send_json(r, "{\"status\":\"ok\"}");
}

static esp_err_t api_wifi_scan_post(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    wifi_manager_scan_start();
    return send_json(r, "{\"status\":\"scanning\"}");
}

/* Response shape: {"scanning":bool,"networks":[...]}.  "scanning" lets the
 * web UI poll for real completion (wifi_manager_scan_in_progress()) instead
 * of the fixed few-second delay it used to guess before reading results —
 * channel count and nearby AP density both affect how long a scan actually
 * takes, so no fixed number was ever reliably right. "networks" reflects
 * whatever esp_wifi's scan-result cache currently holds — the previous
 * scan's results while one is still running, empty before the first scan
 * ever completes. */
static esp_err_t api_wifi_scan_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    bool scanning = wifi_manager_scan_in_progress();
    uint16_t cnt = 0;
    esp_wifi_scan_get_ap_num(&cnt);
    if (cnt > 20) cnt = 20;
    cJSON *arr = cJSON_CreateArray();
    if (cnt > 0) {
        wifi_ap_record_t *list = calloc(cnt, sizeof(wifi_ap_record_t));
        if (!list) { cJSON_Delete(arr); return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL; }
        esp_wifi_scan_get_ap_records(&cnt, list);
        for (int i = 0; i < cnt; i++) {
            cJSON *ap = cJSON_CreateObject();
            cJSON_AddStringToObject(ap, "ssid", (char*)list[i].ssid);
            cJSON_AddNumberToObject(ap, "rssi", list[i].rssi);
            cJSON_AddNumberToObject(ap, "auth", list[i].authmode);
            cJSON_AddItemToArray(arr, ap);
        }
        free(list);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "scanning", scanning);
    cJSON_AddItemToObject(root, "networks", arr);
    char *json = cJSON_PrintUnformatted(root);
    esp_err_t ret = send_json(r, json);
    free(json); cJSON_Delete(root);
    return ret;
}

static esp_err_t api_cors(httpd_req_t *r)
{
    /* CORS preflight (OPTIONS).  We deliberately return NO
     * Access-Control-Allow-* headers — the browser will fail the
     * preflight and block the cross-origin request entirely.
     *
     * Same-origin requests (the device's own web UI talking to its own
     * API) never trigger a preflight in the first place, so this
     * handler is only reached by cross-origin code, which is exactly
     * what we want to refuse.  Returning 204 No Content keeps the
     * response cheap. */
    httpd_resp_set_status(r, "204 No Content");
    httpd_resp_send(r, NULL, 0);
    return ESP_OK;
}

/* ── Static file serving ───────────────────────────────────────────── */
static const char *content_type(const char *p)
{
    if (strstr(p,".html")) return "text/html";
    if (strstr(p,".css"))  return "text/css";
    if (strstr(p,".js"))   return "application/javascript";
    if (strstr(p,".json")) return "application/json";
    if (strstr(p,".png"))  return "image/png";
    if (strstr(p,".jpg"))  return "image/jpeg";
    if (strstr(p,".svg"))  return "image/svg+xml";
    if (strstr(p,".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

/* Open a pre-compressed sibling "<path>.gz" if present (sets *gz=true), else
 * the plain "<path>".  index.html ships gzip-only (~308 KB → ~73 KB): the .gz
 * is served verbatim with Content-Encoding: gzip and the browser inflates it,
 * so the ESP never compresses/decompresses — it just reads fewer bytes from
 * flash and pushes fewer bytes through the socket.  Preferring .gz also makes
 * the migration safe: a stale plain index.html left over from an older build
 * is ignored in favour of the new index.html.gz. */
static FILE *open_gz_or_plain(const char *path, bool *gz)
{
    char gzp[608];
    snprintf(gzp, sizeof(gzp), "%s.gz", path);
    FILE *f = fopen(gzp, "rb");
    if (f) { *gz = true; return f; }
    *gz = false;
    return fopen(path, "rb");
}

/* Lazily load the app shell (index.html.gz, else index.html) into a PSRAM blob.
 * Returns true when the cache is populated (already-loaded counts).  On any
 * failure the cache stays empty and serve_static streams from flash instead.
 * Invalidated by shell_cache_flush() after a WebUI update (hp_drop_stale_index). */
static bool shell_cache_load(void)
{
    if (s_shell_buf) return true;
    bool gz = false;
    FILE *f = open_gz_or_plain("/spiffs/web/index.html", &gz);
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 512 * 1024) { fclose(f); return false; }   /* sanity cap */
    uint8_t *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return false; }
    s_shell_buf = buf;
    s_shell_len = (size_t)sz;
    s_shell_gz  = gz;
    ESP_LOGI(TAG, "[shell] cached %s index shell in PSRAM: %u B",
             gz ? "gzip" : "plain", (unsigned)s_shell_len);
    return true;
}

static esp_err_t serve_static(httpd_req_t *r)
{
    const char *uri = r->uri;

    /* The root path resolves to the app shell. */
    bool want_shell = (strcmp(uri, "/") == 0);

    char fp[600];
    snprintf(fp, sizeof(fp), "/spiffs/web%s", want_shell ? "/index.html" : uri);
    if (strstr(fp, "..")) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad path"), ESP_FAIL;

    /* Content-Type comes from the logical name, never the .gz suffix. */
    const char *ctype = content_type(fp);
    bool  gz = false;
    FILE *f  = NULL;

    /* zones.json and lang/ *.json are the only static assets under data/web/
     * that get hand-edited and pushed on their own (a translation fix, a
     * tzdata refresh via tools/update_zones.py) rather than only changing
     * as part of a whole new release — see that script and the "Missing
     * built-in files" banner exceptions work for examples. The blanket
     * max-age=3600 below is fine for everything else (images, JS, CSS,
     * the app shell, which has its own separate PSRAM-cache invalidation
     * via shell_cache_flush()), but it meant re-uploading just one of
     * these two file types left every already-open browser tab showing
     * stale content for up to an hour with no way to tell — confirmed on
     * real hardware: a zones.json update landed on the device fine, GET
     * confirmed it directly, and only a private/incognito window (no
     * cache) showed the new content. */
    size_t fp_len = strlen(fp);
    bool no_cache = fp_len > 5 && strcmp(fp + fp_len - 5, ".json") == 0;
    const char *cache_control = no_cache ? "no-cache" : "max-age=3600";

    if (!want_shell) {
        f = open_gz_or_plain(fp, &gz);
        if (!f) {
            /* The file doesn't exist.  Only fall back to the SPA app shell for
             * route-like paths (no file extension, e.g. "/settings").  Anything
             * that looks like a missing ASSET (has an extension — favicon.ico,
             * *.png/js/css/map/json …) gets a real 404 so the browser caches the
             * miss.  Without this, /favicon.ico — requested on essentially every
             * navigation — fell through to serving the entire shell each time,
             * dominating httpd CPU even with no real page load. */
            const char *base = strrchr(uri, '/');
            base = base ? base + 1 : uri;
            if (strchr(base, '.') != NULL) {
                /* TEMP diagnostic: log every missing asset (favicon.ico, stray
                 * *.png/js/css/map …) so we can add the file or stop the request.
                 * Remove once the 404s are cleaned up. */
                ESP_LOGW(TAG, "serve_static: missing asset → 404: '%s'", uri);
                return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found"), ESP_FAIL;
            }
            /* TEMP diagnostic: extension-less path with no file → SPA shell. */
            ESP_LOGW(TAG, "serve_static: missing route → shell fallback: '%s'", uri);
            want_shell = true;          /* extension-less route → shell */
        }
    }

    /* App shell: serve from the PSRAM cache in a single send (Content-Length set
     * by httpd, no chunked encoding, no per-chunk yield, no flash reads). */
    if (want_shell) {
        /* Honour a deferred invalidation here, on the httpd task, so the free()
         * never races a send in flight (shell_cache_flush() only flips the flag). */
        if (s_shell_stale) {
            s_shell_stale = false;
            if (s_shell_buf) { free(s_shell_buf); s_shell_buf = NULL; }
            s_shell_len = 0;
            s_shell_gz  = false;
        }
        if (shell_cache_load()) {
            httpd_resp_set_type(r, "text/html");
            if (s_shell_gz) httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
            httpd_resp_set_hdr(r, "Cache-Control", "max-age=3600");
            return httpd_resp_send(r, (const char *)s_shell_buf, (ssize_t)s_shell_len);
        }
        /* Cache load failed (OOM / file missing) — stream from flash as below. */
        if (!f) f = open_gz_or_plain("/spiffs/web/index.html", &gz);
        ctype = "text/html";
    }
    if (!f) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found"), ESP_FAIL;

    httpd_resp_set_type(r, ctype);
    if (gz) httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(r, "Cache-Control", cache_control);

    /* ETag + conditional GET, for the same small hand-pushed JSON files
     * no_cache covers above. "no-cache" alone still re-transfers the
     * whole file on every single settings-page load even when nothing
     * changed, because this server sends no validator for the browser to
     * check against — it can only ever answer with a fresh 200. Reading
     * the whole file once (a few KB) and hashing it is cheap enough to
     * redo on every request for just these two file types; deliberately
     * NOT extended to the generic streaming path below, where a file can
     * be far larger (an uploaded album photo, up to MAX_UPLOAD_BYTES) and
     * hashing on every GET would cost a real flash-read + CPU pass that
     * those files' long max-age already exists to avoid. */
    if (no_cache) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = (fsize > 0 && fsize < 65536) ? malloc((size_t)fsize) : NULL;
        if (buf && fread(buf, 1, (size_t)fsize, f) == (size_t)fsize) {
            /* FNV-1a 32-bit — not a security hash, just a cheap fingerprint
             * that changes whenever the content does; see the size cap
             * above for why this is fine to run on every request here. */
            uint32_t hash = 2166136261u;
            for (long i = 0; i < fsize; i++) { hash ^= buf[i]; hash *= 16777619u; }
            char etag[12];
            snprintf(etag, sizeof(etag), "\"%08x\"", (unsigned)hash);
            httpd_resp_set_hdr(r, "ETag", etag);

            char inm[12] = {0};
            esp_err_t ret;
            if (httpd_req_get_hdr_value_str(r, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
                strcmp(inm, etag) == 0) {
                httpd_resp_set_status(r, "304 Not Modified");
                ret = httpd_resp_send(r, NULL, 0);
            } else {
                ret = httpd_resp_send(r, (const char *)buf, fsize);
            }
            free(buf);
            fclose(f);
            return ret;
        }
        free(buf);              /* NULL-safe: too large for this path, or the read failed */
        fseek(f, 0, SEEK_SET);  /* undo the ftell probe above before falling through */
    }

    /* Stream non-shell files (and the rare cache-miss shell fallback) from flash.
     * fread() on LittleFS is a synchronous SPI op that never yields, so an 8 KB
     * buffer + a 1-tick yield per chunk keeps httpd from starving IDLE and
     * tripping the task WDT on larger files.  taskYIELD() is insufficient: IDLE
     * (pri-0) only runs when no pri-≥1 task is ready, which never happens while
     * httpd (pri-5) keeps re-entering its own ready queue. */
    char *buf = malloc(8192);
    if (!buf) { fclose(f); return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL; }
    size_t rd;
    while ((rd = fread(buf, 1, 8192, f)) > 0) {
        httpd_resp_send_chunk(r, buf, rd);
        vTaskDelay(pdMS_TO_TICKS(1));  /* block 1 tick so IDLE0 can feed the task WDT */
    }
    httpd_resp_send_chunk(r, NULL, 0);
    free(buf); fclose(f);
    return ESP_OK;
}

/* ── URI registration ──────────────────────────────────────────────── */
#define R(m, p, h) { .uri=p, .method=m, .handler=h, .user_ctx=NULL }

static const httpd_uri_t uris[] = {
    R(HTTP_GET,  "/api/ping",             api_ping),
    R(HTTP_GET,  "/api/themes",           api_themes),
    R(HTTP_GET,  "/api/settings",        api_get_settings),
    R(HTTP_GET,  "/api/backup",          api_backup),
    R(HTTP_POST, "/api/settings",        api_post_settings),
    R(HTTP_GET,  "/api/firmwareVersion", api_fw_ver),
    R(HTTP_GET,  "/api/hardwareVersion", api_hw_ver),
    R(HTTP_POST, "/api/reset",           api_reset),
    R(HTTP_POST, "/api/reboot",          api_reboot),
    R(HTTP_POST, "/api/audio/play",      api_audio_play),
    R(HTTP_POST, "/api/rotation_pause",  api_rotation_pause),
    R(HTTP_POST, "/api/weather",         api_post_weather),
    R(HTTP_POST, "/api/cx_image",        api_cx_image),
    R(HTTP_GET,  "/api/status",          api_status),
    R(HTTP_GET,  "/api/wl_scene",        api_wl_scene),
    R(HTTP_GET,  "/api/network_info",    api_network_info),
    R(HTTP_POST, "/api/update_firmware", api_ota),
    R(HTTP_POST, "/api/update_fs",          api_fs_ota),
    R(HTTP_POST, "/api/update_spiffs",      api_fs_ota),       /* backward-compat alias */
    R(HTTP_POST, "/api/update_fs_hotpatch", api_fs_hotpatch),  /* ZIP delta — no reboot */
    R(HTTP_GET,    "/api/file/ls",         api_file_ls),
    R(HTTP_GET,    "/api/file/download",  api_file_download),
    R(HTTP_POST,   "/api/file/upload",    api_file_upload),
    R(HTTP_POST,   "/api/file/mkdir",     api_file_mkdir),
    R(HTTP_POST,   "/api/file/rename",    api_file_rename),
    R(HTTP_DELETE, "/api/file/delete",    api_file_delete),
    R(HTTP_POST,   "/api/wifi/scan",      api_wifi_scan_post),
    R(HTTP_GET,  "/api/wifi/scan",       api_wifi_scan_get),
    R(HTTP_GET,  "/api/logs",            api_get_logs),
    R(HTTP_POST, "/api/logs/clear",      api_clear_logs),
    R(HTTP_GET,  "/api/debug/adc",       api_debug_adc),
    R(HTTP_POST, "/api/debug/dac",       api_debug_dac),
    R(HTTP_POST, "/api/debug/pwm",       api_debug_pwm),
    R(HTTP_POST, "/api/debug/loglevel",  api_debug_loglevel),
    R(HTTP_POST, "/api/debug/wl_fps",    api_debug_wl_fps),
    R(HTTP_POST, "/api/debug/stacklog",  api_debug_stacklog),
    R(HTTP_GET,  "/api/debug/heap",      api_debug_heap),
    R(HTTP_GET,  "/api/debug/tasks",     api_debug_tasks),
    R(HTTP_POST, "/api/debug/tasks_start",  api_debug_tasks_start),
    R(HTTP_GET,  "/api/debug/tasks_result", api_debug_tasks_result),
    R(HTTP_GET,  "/api/debug/micframe",  api_debug_micframe),
    R(HTTP_GET,  "/api/debug/micbands",  api_debug_micbands),
    R(HTTP_GET,  "/api/debug/mictrace",  api_debug_mictrace),
    R(HTTP_POST, "/api/debug/mictrace",  api_debug_mictrace),
    R(HTTP_POST, "/api/debug/mictest",   api_debug_mictest),
    R(HTTP_GET,  "/api/debug/mictune",   api_debug_mictune),
    R(HTTP_POST, "/api/debug/mictune",   api_debug_mictune),
    R(HTTP_POST, "/api/mic/calibrate",          api_mic_calibrate),
    R(HTTP_POST, "/api/mic/reset_calibration",  api_mic_reset_calibration),
    R(HTTP_POST, "/api/update_notify",          api_update_notify),
    R(HTTP_POST, "/api/ota_pull",              api_ota_pull),
    R(HTTP_GET,  "/api/ota_pull_status",       api_ota_pull_status),
    R(HTTP_POST, "/api/webui_pull",            api_webui_pull),
    R(HTTP_GET,  "/api/webui_pull_auto",       api_webui_pull_auto),
    R(HTTP_GET,  "/api/webui_pull_status",     api_webui_pull_status),
    R(HTTP_POST, "/api/webui_pull_reboot",     api_webui_pull_reboot),
    R(HTTP_POST, "/api/stock_repair",          api_stock_repair),
    R(HTTP_GET,  "/api/themes/catalog",        api_themes_catalog),
    R(HTTP_POST, "/api/themes/refresh",        api_themes_refresh),
    R(HTTP_POST, "/api/themes/install",        api_themes_install),
    R(HTTP_POST, "/api/themes/remove",         api_themes_remove),
    R(HTTP_POST, "/api/themes/verify",         api_themes_verify),
    R(HTTP_GET,  "/api/auto_update",           api_auto_update_get),
    R(HTTP_POST, "/api/auto_update/check",     api_auto_update_check),
    R(HTTP_GET,  "/api/lcd_preview",           api_lcd_preview_get),
    R(HTTP_POST, "/api/lcd_preview",           api_lcd_preview_post),
    R(HTTP_GET,  "/api/themes/status",         api_themes_status),
    R(HTTP_GET,  "/api/stock_repair_status",   api_stock_repair_status),
    R(HTTP_GET,  "/api/stock_missing_list",    api_stock_missing_list),
    R(HTTP_POST, "/api/social/refresh",         api_social_refresh),
    R(HTTP_POST, "/api/debug/burnin",           api_debug_burnin),
    R(HTTP_POST, "/api/debug/snow",             api_debug_snow),
    /* Auth routes.  set_password is allowed unauth on first boot only;
     * change_password is itself REQUIRE_AUTH'd. */
    R(HTTP_POST, "/api/auth/set_password",      api_auth_set_password),
    R(HTTP_POST, "/api/auth/login",             api_auth_login),
    R(HTTP_POST, "/api/auth/logout",            api_auth_logout),
    R(HTTP_POST, "/api/auth/change_password",   api_auth_change_password),
    R(HTTP_POST, "/api/auth/disable",           api_auth_disable),
    R(HTTP_GET,  "/api/auth/check",             api_auth_check),
    /* —— setup AP PIN management. */
    R(HTTP_GET,  "/api/wifi/ap_pin",            api_wifi_ap_pin),
    R(HTTP_POST, "/api/wifi/regen_pin",         api_wifi_regen_pin),
    R(HTTP_POST, "/api/factory_reset_full",     api_factory_reset_full),
    R(HTTP_OPTIONS, "/api/*",            api_cors),
};

/* Device-driven WebUI pull after a firmware OTA reboot.
 *
 * The WebUI ZIP is applied AFTER the firmware reboot. Relying on the
 * browser to re-connect through the post-reboot network churn (mDNS
 * re-registration + httpd connection resets) and call api_webui_pull_auto
 * itself is unreliable — that trigger can land during the unstable window
 * right after boot and get reset before the handler runs, silently
 * stalling the update. Instead the device starts the pull itself:
 * ota_pull_task already stored webui_url + webui_sha256 in NVS before
 * rebooting, so this task just waits for connectivity, then spawns
 * webui_pull_task exactly as api_webui_pull_auto would. The browser's
 * Phase 4 only has to poll /api/webui_pull_status for progress, not
 * trigger anything.
 *
 * s_ota_active is set before spawning, so a stray browser trigger that
 * does arrive hits the 409 guard in api_webui_pull_auto/api_webui_pull
 * and cannot double-spawn. */
static void post_ota_autostart_task(void *arg)
{
    ESP_LOGI(TAG, "[post_ota] auto-WebUI task started — waiting for network…");

    /* webui_pull_task needs a live connection for the TLS download.  Wait up to
     * 60 s for STA to obtain an IP (it normally arrives within ~10 s). */
    for (int i = 0; i < 60; i++) {
        const char *ip = wifi_manager_get_ip();
        if (ip && strcmp(ip, "0.0.0.0") != 0) {
            ESP_LOGI(TAG, "[post_ota] network up (%s) after %d s", ip, i);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* Read the stored WebUI URL + hash written by ota_pull_task pre-reboot. */
    nvs_handle_t h;
    if (nvs_open("nextube_sec", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "[post_ota] NVS open failed — cannot auto-pull WebUI");
        vTaskDelete(NULL);
        return;
    }
    size_t url_sz = sizeof(s_webui.url);
    esp_err_t ue = nvs_get_str(h, "webui_url", s_webui.url, &url_sz);
    s_webui.sha256[0] = '\0';
    size_t sha_sz = sizeof(s_webui.sha256);
    nvs_get_str(h, "webui_sha256", s_webui.sha256, &sha_sz);
    nvs_close(h);

    if (ue != ESP_OK || !s_webui.url[0]) {
        ESP_LOGW(TAG, "[post_ota] no webui_url stored — clearing flag, nothing to do");
        consume_post_ota_flag();
        vTaskDelete(NULL);
        return;
    }

    /* Consume the one-time post_ota flag: clears it in NVS and lets
     * api_ota_pull_status stop returning 503.  Also marks this boot as
     * post-OTA-authorised so a manual browser poll/trigger bypasses auth. */
    consume_post_ota_flag();   /* clears NVS; also sets s_post_ota_auth_set_us */
    s_post_ota_auth = true;

    if (s_ota_active) {
        ESP_LOGW(TAG, "[post_ota] OTA already active — skipping auto-pull (browser beat us to it)");
        vTaskDelete(NULL);
        return;
    }

    s_webui.state    = WEBUI_IDLE;
    s_webui.error[0] = '\0';
    s_webui.written_files[0] = '\0';
    s_ota_active     = true;

    ESP_LOGI(TAG, "[post_ota] starting device-driven WebUI pull: %.80s", s_webui.url);
    if (!start_webui_pull_task()) {
        s_ota_active = false;
        ESP_LOGE(TAG, "[post_ota] webui_pull task create failed");
    }
    vTaskDelete(NULL);
}

/* Timer callback for the post-OTA auto-pull defer — see the call site inside
 * web_server_start()'s one-time setup block for why this is deferred rather
 * than spawned immediately. Runs on the esp_timer service task's own
 * (already-allocated) stack, so no new allocation happens until the actual
 * xTaskCreatePinnedToCore() below. */
static void post_ota_autostart_timer_cb(void *arg)
{
    if (xTaskCreatePinnedToCore(post_ota_autostart_task, "post_ota_auto",
                               4096, NULL, 4, NULL, 0) != pdPASS)
        ESP_LOGE(TAG, "[boot] post_ota_autostart_task creation failed");
}



/* Shared with web_server_start()'s one-time setup block below, which creates
 * this on the initial 6 s defer — reused here so an OTA-busy reschedule (see
 * stock_files_check_timer_cb()) doesn't need its own separate timer. */
static esp_timer_handle_t s_stock_check_timer;


/* Timer callback for the deferred stock_files_check(). Spawns a task rather
 * than calling it directly: this callback runs on the shared esp_timer
 * service task, and stock_files_check()'s 384-file loop blocking here would
 * stall every other timer queued behind it. s_ota_active guard: this scan
 * touches the same LittleFS/flash bus a concurrent OTA/webUI/repair write
 * is using, and isn't on ota_suspend_tasks()'s list (it doesn't exist yet at
 * suspend-time — spawned fresh by this timer). Reschedules a few seconds
 * out rather than skipping the check for this boot, since the missing-files
 * recovery banner still needs it to run once. */
static void stock_files_check_timer_cb(void *arg)
{
    /* Auto-check opt-out (Settings → System → Verify & Repair Stock Files).
     * Checked first, ahead of every defer guard below: if it's off there's
     * nothing to defer or retry, just skip the automatic scan outright. The
     * standalone "Verify & Repair" button and the post-hotpatch/webui-pull
     * refreshes call stock_files_check()/stock_repair_task() directly, not
     * through this timer, so they're unaffected and the missing-files
     * banner still updates correctly after either. */
    config_lock();
    bool auto_check_on = config_get()->stock_auto_check_enabled;
    config_unlock();
    if (!auto_check_on) return;

    if (s_ota_active) {
        ESP_LOGI(TAG, "stock-file count check: OTA/webUI pull active — deferring 10 s");
        if (s_stock_check_timer)
            esp_timer_start_once(s_stock_check_timer, 10000 * 1000ULL);  /* 10 s in µs */
        return;
    }
    /* The stock scan pauses display_task for its whole run — a
     * generic hitch elsewhere in the UI, but the AP-PIN marquee redraws
     * every ~200ms, so a pause here reads as the tubes visibly stuttering.
     * Same defer-and-retry pattern as the s_ota_active guard above. */
    if (wifi_manager_ap_pin_visible()) {
        ESP_LOGI(TAG, "stock-file count check: AP PIN visible — deferring 5 s to avoid stalling its redraw");
        if (s_stock_check_timer)
            esp_timer_start_once(s_stock_check_timer, 5000 * 1000ULL);  /* 5 s in µs */
        return;
    }
    /* Same reasoning as the AP-PIN guard above, for the boot splash:
     * pausing the whole display for this scan reads as the splash hanging,
     * not a brief hitch — and under flash contention (busy WiFi/MQTT/NVS)
     * that hang can run into the tens of seconds. */
    if (display_boot_wait_active()) {
        ESP_LOGI(TAG, "stock-file count check: boot splash active — deferring 5 s to avoid pausing it");
        if (s_stock_check_timer)
            esp_timer_start_once(s_stock_check_timer, 5000 * 1000ULL);  /* 5 s in µs */
        return;
    }
    stock_files_check_spawn();
}

/* Refresh the HTTP server when STA obtains a new IP after a credential change.
 * Stop first so httpd gets fresh sockets bound to the new interface;
 * web_server_start() would be a no-op if we didn't clear s_server first. */
static void web_server_got_ip_handler(void *arg, esp_event_base_t base,
                                      int32_t id, void *data)
{
    if (!s_server_restart_pending) return;
    s_server_restart_pending = false;
    ESP_LOGI(TAG, "STA got new IP — refreshing HTTP server sockets");
    web_server_stop();   /* clears s_server so web_server_start() isn't a no-op */
    web_server_start();
}

void web_server_start(void)
{
    /* One-time setup: log hook and IP-reconnect handler.
     * Guard with s_log_mutex so these are only installed on the first call;
     * subsequent calls (after a WiFi reconnect) skip straight to httpd_start. */
    if (!s_log_mutex) {
        s_log_mutex = xSemaphoreCreateMutex();
        au_boot_note();
        periodic_net_poll_register(np_job_tick, 0, "jobs");   /* update and Theme Manager jobs run on the shared network task */
        esp_log_set_vprintf(log_vprintf_hook);
        /* Restart the HTTP server whenever STA obtains a (new) IP address,
         * so the listening socket is always fresh after a credential change. */
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   web_server_got_ip_handler, NULL);
        /* Initialise admin auth state (in-RAM session table, lockout counters).
         * NVS is already initialised by main.c::init_nvs() before web_server_start. */
        auth_init();

        /* One-time boot check — see stock_files_check() doc comment for why
         * it runs here rather than lazily inside api_status(). LittleFS is
         * already mounted by the time main.c calls web_server_start().
         *
         * Timer-deferred (6s), not called inline: this whole one-time block
         * runs synchronously before httpd_start() below, so a call here
         * would delay the httpd socket itself from opening ("Web UI ready"),
         * not just this check's own result. 6s clears the WiFi auth/assoc/
         * DHCP negotiation storm this scan would otherwise compete with
         * (higher-priority WiFi driver tasks can stretch its per-32-file
         * yields far past their nominal delay) without moving
         * web_server_start() itself later, which costs boot-to-Web-UI time
         * directly. */
        {
            const esp_timer_create_args_t sa = {
                .callback = stock_files_check_timer_cb,
                .name     = "stock_chk_defer",
            };
            if (esp_timer_create(&sa, &s_stock_check_timer) == ESP_OK)
                esp_timer_start_once(s_stock_check_timer, 6000 * 1000ULL);  /* 6 s in µs */
            else
                ESP_LOGE(TAG, "stock_check_defer_timer create failed — recovery check will not run");
        }

        /* Peek the NVS post_ota flag (without consuming it) to detect a
         * post-OTA-firmware reboot.  api_ota_pull_status will return 503
         * until the flag is consumed by api_webui_pull_auto. */
        {
            nvs_handle_t ph;
            uint8_t pflag = 0;
            if (nvs_open("nextube_sec", NVS_READONLY, &ph) == ESP_OK) {
                nvs_get_u8(ph, "post_ota", &pflag);
                nvs_close(ph);
            }
            s_post_ota_boot_pending = (pflag != 0);
            if (s_post_ota_boot_pending) {
                ESP_LOGI(TAG, "[boot] post-OTA boot — device will auto-pull WebUI; ota_pull_status → 503 until it fires");
                /* Drive the WebUI pull ourselves rather than waiting for the
                 * browser to reach us through the post-reboot network churn.
                 *
                 * Timer-deferred creation, not an immediate xTaskCreate here:
                 * web_server_start() runs early in app_main(), still mid-way
                 * through its permanent-task-creation block, so a same-boot
                 * xTaskCreate here would land post_ota_auto's stack right in
                 * the middle of that batch's own allocations — only on this
                 * one rare post-firmware-OTA boot. Deferring 3s pushes the
                 * actual xTaskCreate() well past app_main()'s return, with no
                 * cost: post_ota_autostart_task already opens by waiting for
                 * network internally. */
                static esp_timer_handle_t post_ota_defer_timer;
                const esp_timer_create_args_t pa = {
                    .callback = post_ota_autostart_timer_cb,
                    .name     = "post_ota_defer",
                };
                if (esp_timer_create(&pa, &post_ota_defer_timer) == ESP_OK)
                    esp_timer_start_once(post_ota_defer_timer, 3000 * 1000ULL);  /* 3 s in µs */
                else
                    ESP_LOGE(TAG, "[boot] post_ota_defer_timer create failed — auto-pull will not start");
            }
        }
    }

    if (s_server) return;   /* already running */

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* Route count + headroom. Must exceed uris[] table size + 1 for the
     * static wildcard registered after it — if this cap is hit, the excess
     * registrations fail and, since the wildcard registers last, every web
     * UI page 404s while the APIs still work. The loop below logs any
     * failure loudly. */
    cfg.max_uri_handlers = 86;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.stack_size       = 8192;
    /* Bump the open-socket cap so concurrent OTA + page reload + status polls
     * don't exhaust the default 7-slot pool.  Each socket costs ~1.3 KB
     * internal RAM; 12 sockets ≈ 16 KB total. */
    cfg.max_open_sockets  = WEB_SERVER_MAX_SOCKETS;
    /* Default recv/send timeouts are 5 s; bump to 10 s so a slow LAN doesn't
     * abort mid-handler.  The OTA path overrides recv to 60 s per-request via
     * setsockopt — that override remains in api_ota(). */
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    /* A connection that dies mid-flight during a WiFi drop/roam (no clean
     * FIN) leaves its socket slot stuck with no way for httpd to reclaim it
     * on its own — the request that owned it will never come back to close
     * it. Without this, enough of those over a long uptime (each WiFi
     * hiccup being one opportunity) eventually fills all max_open_sockets
     * slots with dead connections and httpd refuses every new one
     * permanently, with no self-recovery short of a reboot. This tells
     * httpd to evict the least-recently-used connection to make room for a
     * new one once the pool is full, instead of refusing it. */
    cfg.lru_purge_enable  = true;
    /* Pin httpd to Core 0, off the display task.  Core 1 carries the display
     * task, which the WeatherLive realtime theme keeps hot every tick (~full
     * core while animating) — sharing that core starved httpd (the original
     * 61%-with-stutter symptom).  Core 0 runs WiFi/lwIP (and, only in Spectrum
     * mode, the mic ADC), so httpd has headroom there for normal web use.
     * The PSRAM-cached shell + favicon 404 fix cut httpd's per-request cost, and
     * the cooperative park / display_busy_hint backoff cover the moments both
     * cores are loaded, so Core 0 is the right home in the common case. */
    cfg.core_id = 0;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    for (int i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) {
        esp_err_t rerr = httpd_register_uri_handler(s_server, &uris[i]);
        if (rerr != ESP_OK)
            ESP_LOGE(TAG, "route %d (%s) registration FAILED: %s — bump max_uri_handlers!",
                     i, uris[i].uri, esp_err_to_name(rerr));
    }

    /* Wildcard static handler (must be last) */
    httpd_uri_t wildcard = R(HTTP_GET, "/*", serve_static);
    esp_err_t werr = httpd_register_uri_handler(s_server, &wildcard);
    if (werr != ESP_OK)
        ESP_LOGE(TAG, "static wildcard registration FAILED: %s — web UI will 404; bump max_uri_handlers!",
                 esp_err_to_name(werr));

    /* Log the direct IP so users can reach the UI before mDNS propagates.
     * mDNS (nextube.local) takes 10–30 s to register after boot; this
     * fallback URL works immediately from the moment the server starts. */
    const char *ip = wifi_manager_get_ip();
    if (ip && strcmp(ip, "0.0.0.0") != 0) {
        ESP_LOGI(TAG, "Web UI ready → http://%s  (or http://nextube.local once mDNS registers)", ip);
    } else {
        ESP_LOGI(TAG, "HTTP server started on port 80 (IP not yet assigned — check again after WiFi connects)");
    }
}

void web_server_stop(void)
{
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
}

int web_server_socket_count(void)
{
    if (!s_server) return -1;
    size_t n = WEB_SERVER_MAX_SOCKETS;
    int fds[WEB_SERVER_MAX_SOCKETS];
    if (httpd_get_client_list(s_server, &n, fds) != ESP_OK) return -1;
    return (int)n;
}
