#include "update_check.h"
#include "config_mgr.h"
#include "periodic_net_poll.h"
#include "fw_version.h"
#include "display.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "update_check";

#define DEFAULT_REPO "MrToast99/Nextube-Remaster"

/* ── Result state ──────────────────────────────────────────────────────
 * Written only by do_check() (update_check_task's thread); read by
 * ha_mqtt.c's publish loop.  Protected by s_mutex since s_latest is a
 * multi-byte buffer — a torn read would surface a garbled version string. */
static SemaphoreHandle_t s_mutex          = NULL;
static bool              s_available      = false;
static char              s_latest[16]     = "";

/* ── HTTP helper ──────────────────────────────────────────────────────
 * Same shape as weather.c's http_get(): open/fetch_headers/read loop so
 * chunked-encoded responses work, crt_bundle_attach for TLS verification,
 * tls_sem_take/give to serialise mbedTLS contexts against weather/social
 * fetches, a required User-Agent (GitHub's API rejects requests without
 * one). Response is capped/truncated at HTTP_MAX_BODY — see
 * json_extract_string() below for why that's fine even though GitHub's
 * releases response is routinely much larger than 4 KB. */
#define HTTP_MAX_BODY 4096
#define HTTP_USER_AGENT \
    "NextubeRemaster/" FW_VERSION_STR " (github.com/MrToast99/Nextube-Remaster)"

static char *http_get(const char *url)
{
    tls_sem_take();

    esp_http_client_config_t hcfg = {
        .url               = url,
        .timeout_ms        = 10000,
        .user_agent        = HTTP_USER_AGENT,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    char *result = NULL;

    esp_http_client_handle_t c = esp_http_client_init(&hcfg);
    if (!c) goto done;

    if (esp_http_client_open(c, 0) != ESP_OK) {
        esp_http_client_cleanup(c); c = NULL; goto done;
    }
    esp_http_client_fetch_headers(c);

    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d: %s", status, url);
        esp_http_client_close(c);
        esp_http_client_cleanup(c); c = NULL; goto done;
    }

    result = malloc(HTTP_MAX_BODY + 1);
    if (!result) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c); c = NULL; goto done;
    }

    int total = 0, r;
    do {
        r = esp_http_client_read(c, result + total, HTTP_MAX_BODY - total);
        if (r > 0) total += r;
    } while (r > 0 && total < HTTP_MAX_BODY);
    result[total] = '\0';

    esp_http_client_close(c);
    esp_http_client_cleanup(c); c = NULL;

    if (total == 0) { free(result); result = NULL; }

done:
    tls_sem_give();
    return result;
}

/* Compares up to 3 dot-separated integer components (major.minor.patch).
 * Returns >0 if a > b, <0 if a < b, 0 if equal — C port of the browser's
 * semverCmp() in data/web/index.html so firmware and web UI agree. */
static int semver_cmp(const char *a, const char *b)
{
    int pa[3] = {0, 0, 0}, pb[3] = {0, 0, 0};
    sscanf(a ? a : "0", "%d.%d.%d", &pa[0], &pa[1], &pa[2]);
    sscanf(b ? b : "0", "%d.%d.%d", &pb[0], &pb[1], &pb[2]);
    for (int i = 0; i < 3; i++) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

/* Lightweight extraction of a JSON string value (first match) — same
 * technique as weather.c's fetch_met_no(), and for the same reason: GitHub's
 * releases response routinely exceeds HTTP_MAX_BODY once release notes and
 * an assets array are included, and a full-document parser (cJSON_Parse)
 * fails on the WHOLE document if it's truncated anywhere — even after
 * tag_name has already been fully captured earlier in the buffer.  A plain
 * strstr works on a truncated buffer as long as the target field itself is
 * intact, which "tag_name" reliably is (it appears within the first ~1 KB
 * of GitHub's response, well inside the 4 KB window). Also correct for
 * both response shapes (object for /releases/latest, array for
 * /releases?per_page=1): GitHub always lists the newest release first, so
 * the first "tag_name" in the byte stream is always the one we want. */
static bool json_extract_string(const char *buf, const char *key,
                                 char *dst, size_t dstsz)
{
    char search[32];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(buf, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < dstsz - 1) dst[i++] = *p++;
    dst[i] = '\0';
    return i > 0;
}

/* Runs one GitHub release check.  Any failure (bad HTTP status, unparsable
 * body, missing tag_name) is logged at WARN and otherwise a no-op — retried
 * on the next 24 h cycle. */
static void do_check(void)
{
    ESP_LOGI(TAG, "checking for updates...");
    char repo[64];
    config_lock();
    strncpy(repo, config_get()->update_repo, sizeof(repo) - 1);
    repo[sizeof(repo) - 1] = '\0';
    config_unlock();
    if (repo[0] == '\0') strncpy(repo, DEFAULT_REPO, sizeof(repo) - 1);

    /* Default repo: /releases/latest (object, excludes pre-releases).
     * Overridden repo: /releases?per_page=1 (array) so pre-release test
     * builds are visible — mirrors index.html's dbgRefreshUpdateRepo() path. */
    bool overridden = (strcmp(repo, DEFAULT_REPO) != 0);
    char url[160];
    if (overridden) {
        snprintf(url, sizeof(url),
                 "https://api.github.com/repos/%s/releases?per_page=1", repo);
    } else {
        snprintf(url, sizeof(url),
                 "https://api.github.com/repos/%s/releases/latest", repo);
    }

    char *body = http_get(url);
    if (!body) {
        ESP_LOGW(TAG, "GitHub fetch failed (connect/TLS/timeout): %s", url);
        return;
    }

    char tag[24];
    bool found = json_extract_string(body, "tag_name", tag, sizeof(tag));
    free(body);
    if (!found) {
        ESP_LOGW(TAG, "GitHub response had no usable tag_name (repo=%s%s)",
                 repo, overridden ? ", check per_page=1 array had a release" : "");
        return;
    }

    const char *raw = tag;
    if (raw[0] == 'v' || raw[0] == 'V') raw++;   /* GitHub tags are typically "v1.18.0" */

    bool avail = semver_cmp(raw, FW_VERSION_STR) > 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_available = avail;
    strncpy(s_latest, raw, sizeof(s_latest) - 1);
    s_latest[sizeof(s_latest) - 1] = '\0';
    xSemaphoreGive(s_mutex);

    display_set_update_indicator(avail);
    ESP_LOGI(TAG, "checked %s: latest=%s running=%s available=%s",
             repo, raw, FW_VERSION_STR, avail ? "yes" : "no");
}

/* 24h in milliseconds — the interval update_check_poll_tick() below hands
 * back to periodic_net_poll_task() after every check. (Used to be
 * maintained as microseconds against esp_timer_get_time() with an hourly
 * wake-and-compare loop, matching ntp_time.c's NTP_DNS_REFRESH_US idiom —
 * that hourly-granularity bookkeeping is no longer needed now that the
 * shared scheduler already wakes at least once a minute for its OTHER
 * registered subsystems and re-checks every entry's own deadline each
 * time; see periodic_net_poll.h's doc comment for the merge rationale.) */
#define UPDATE_CHECK_INTERVAL_MS (24LL * 3600LL * 1000LL)

/* Called by periodic_net_poll_task() once due; returns how many ms until it
 * should be called again — always the flat 24h interval, no backoff (a
 * failed check just logs a warning and is retried at the normal interval,
 * same as before the merge). */
static uint32_t update_check_poll_tick(void)
{
    do_check();
    return (uint32_t)UPDATE_CHECK_INTERVAL_MS;
}

void update_check_start(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    /* No longer a dedicated task (was 6144 B, measured peak 3820 B) — see
     * weather_start()'s matching comment and periodic_net_poll.h's doc
     * comment for the full rationale. first_delay_ms=0: check immediately
     * once the shared WiFi/DNS gate clears, matching this task's original
     * "do_check() once on boot" behavior. */
    periodic_net_poll_register(update_check_poll_tick, 0, "update_check");
}

bool update_check_get_status(char *out, size_t len)
{
    /* s_mutex is only created by update_check_start() — a caller (ha_mqtt's
     * publish loop) may run with update_check_enabled=false, in which case
     * the task, and its mutex, were never started. */
    if (!s_mutex) return false;

    bool avail;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    avail = s_available;
    /* s_latest reflects the latest known GitHub tag from any successful
     * check, not just ones where a newer release was found — a "Latest
     * Version" diagnostic sensor should read a real value even when the
     * device is already up to date (or ahead of the latest release), not
     * stay "unknown" until an update happens to be pending. */
    if (s_latest[0] && out && len) {
        strncpy(out, s_latest, len - 1);
        out[len - 1] = '\0';
    }
    xSemaphoreGive(s_mutex);
    return avail;
}
