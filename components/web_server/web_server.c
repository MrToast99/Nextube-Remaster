#include "web_server.h"
#include <math.h>
#include "microphone.h"
#include "config_mgr.h"
#include "wifi_manager.h"
#include "ntp_time.h"
#include "weather.h"
#include "subscribers.h"
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
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "auth.h"
#include "nvs.h"

static const char *TAG = "web_srv";
static httpd_handle_t s_server = NULL;
static bool s_server_restart_pending = false;   /* set when a WiFi reconnect stops the server */

/* LittleFS usage stats (fs_total/fs_used in /api/status) — cached until
 * explicitly invalidated. esp_littlefs_info() walks every block in the
 * filesystem to compute used space (no cheap counter API exists), which
 * measured ~2.7s on this device's file count (issue #82) — recomputing it
 * on every /api/status poll (every 5s from the dashboard) meant the
 * single-threaded httpd task spent roughly half its life blocked in this
 * one call, unable to accept any other connection.
 *
 * Computed lazily (first /api/status call — effectively "once at boot",
 * since that's the first request the device serves) and only recomputed
 * when fs_usage_invalidate() is called after an operation that can actually
 * change free space: file upload, file delete, hotpatch. A full LittleFS
 * OTA (api_fs_ota) always ends in esp_restart(), which resets this cache
 * for free — no explicit invalidation needed there. mkdir/rename are NOT
 * invalidation points: they don't meaningfully change used bytes (a stale
 * reading there is off by, at most, a few bytes of directory metadata).
 * Only ever touched from the httpd task, so no lock is needed. */
static bool    s_fs_cache_valid   = false;
static size_t  s_fs_total_cached  = 0;
static size_t  s_fs_used_cached   = 0;
static void fs_usage_invalidate(void) { s_fs_cache_valid = false; }

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
        ESP_LOGE(TAG, "/api/settings: config_to_json FAILED — internal heap free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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
    wifi_manager_reconnect_sta();
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
    uint8_t old_invert_mask;
    uint8_t old_init_profile[6];
    uint8_t old_vcom[6];
    float   old_gamma[6];
    int8_t  old_col_offset[6], old_row_offset[6];
    uint8_t old_tube_brightness[6];
    config_lock();
    const nextube_config_t *old_cfg = config_get();
    strlcpy(old_ssid,     old_cfg->ssid,      sizeof(old_ssid));
    strlcpy(old_pass,     old_cfg->password,  sizeof(old_pass));
    strlcpy(old_hostname, old_cfg->hostname,  sizeof(old_hostname));
    old_weather_en   = old_cfg->weather_enabled;
    old_youtube_en   = old_cfg->youtube_enabled;
    old_mdns_en      = old_cfg->mdns_enabled;
    old_mic_en       = old_cfg->mic_enabled;
    old_audio_en     = old_cfg->audio_enabled;
    old_invert_mask  = old_cfg->lcd_invert_mask;
    memcpy(old_init_profile,    old_cfg->lcd_init_profile,    sizeof(old_init_profile));
    memcpy(old_vcom,            old_cfg->lcd_vcom,            sizeof(old_vcom));
    memcpy(old_gamma,           old_cfg->lcd_gamma,           sizeof(old_gamma));
    memcpy(old_col_offset,      old_cfg->lcd_col_offset,      sizeof(old_col_offset));
    memcpy(old_row_offset,      old_cfg->lcd_row_offset,      sizeof(old_row_offset));
    memcpy(old_tube_brightness, old_cfg->lcd_tube_brightness, sizeof(old_tube_brightness));
    config_unlock();

    bool ok = config_set_json(buf, len);
    free(buf);
    display_config_changed();  /* NVS write done — lift busy backoff + force re-render of live config changes */

    uint8_t new_brightness;
    bool    new_audio_enabled;
    char    new_ssid[64], new_pass[64], new_hostname[32];
    bool    new_weather_en, new_youtube_en, new_mdns_en, new_mic_en;
    float   new_sht30_offset;
    uint8_t new_invert_mask;
    uint8_t new_init_profile[6];
    uint8_t new_vcom[6];
    float   new_gamma[6];
    int8_t  new_col_offset[6], new_row_offset[6];
    uint8_t new_tube_brightness[6];
    config_lock();
    const nextube_config_t *new_cfg = config_get();
    new_brightness    = new_cfg->led_brightness;
    new_audio_enabled = new_cfg->audio_enabled;
    strlcpy(new_ssid,     new_cfg->ssid,      sizeof(new_ssid));
    strlcpy(new_pass,     new_cfg->password,  sizeof(new_pass));
    strlcpy(new_hostname, new_cfg->hostname,  sizeof(new_hostname));
    new_weather_en    = new_cfg->weather_enabled;
    new_youtube_en    = new_cfg->youtube_enabled;
    new_mdns_en       = new_cfg->mdns_enabled;
    new_mic_en        = new_cfg->mic_enabled;
    new_sht30_offset  = new_cfg->sht30_temp_offset;
    new_invert_mask   = new_cfg->lcd_invert_mask;
    memcpy(new_init_profile,    new_cfg->lcd_init_profile,    sizeof(new_init_profile));
    memcpy(new_vcom,            new_cfg->lcd_vcom,            sizeof(new_vcom));
    memcpy(new_gamma,           new_cfg->lcd_gamma,           sizeof(new_gamma));
    memcpy(new_col_offset,      new_cfg->lcd_col_offset,      sizeof(new_col_offset));
    memcpy(new_row_offset,      new_cfg->lcd_row_offset,      sizeof(new_row_offset));
    memcpy(new_tube_brightness, new_cfg->lcd_tube_brightness, sizeof(new_tube_brightness));
    config_unlock();

    leds_set_brightness(new_brightness);
    ntp_apply_timezone();
    ntp_apply_servers();
    audio_set_enabled(new_audio_enabled);
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
    if (needs_reboot) {
        send_json(r, ok ? "{\"status\":\"ok\",\"reboot\":true}"
                        : "{\"status\":\"error\",\"reboot\":false}");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return ESP_OK;
    }

    bool ssid_changed = (strcmp(old_ssid, new_ssid) != 0);
    bool pass_changed = (strcmp(old_pass, new_pass)  != 0);

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
    send_json(r, "{\"status\":\"ok\",\"message\":\"Resetting and rebooting...\"}");
    vTaskDelay(pdMS_TO_TICKS(500));

    auth_factory_reset();
    wifi_manager_factory_reset_ap_pin();
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
    /* Wipe the WiFi driver's own NVS namespace so the device cannot
     * reconnect to the old network after reboot.  Must be called while
     * the WiFi stack is running (before esp_restart). */
    esp_wifi_restore();
    config_reset();
    send_json(r, "{\"status\":\"ok\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* POST /api/reboot — restart the device without touching the config */
static esp_err_t api_reboot(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    send_json(r, "{\"status\":\"ok\",\"message\":\"Rebooting...\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t api_status(httpd_req_t *r)
{
    /* Timing instrumentation — issue #82 (community-reported multi-second
     * /api/status latency, captured via PCAPdroid: consistently ~5s, while
     * the much larger static GET / is ~2s). The payload-size mismatch argues
     * against a pure network/TCP-ACK cause and points at server-side time in
     * this handler; these checkpoints narrow down which section is slow.
     * DEBUG level — silent by default, silent even under the System tab's
     * per-subsystem "enabled" checkboxes (those only reach INFO). Visible
     * once the "Debug logging" checkbox in System → Device Logs is checked,
     * which raises the runtime default to DEBUG via /api/debug/loglevel. */
    int64_t t0 = esp_timer_get_time();
    cJSON *root = cJSON_CreateObject();
    struct tm t; ntp_get_local(&t);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &t);
    cJSON_AddStringToObject(root, "time", ts);
    cJSON_AddBoolToObject(root, "ntp_synced",      ntp_time_synced());
    cJSON_AddBoolToObject(root, "rtc_battery_ok",  ntp_rtc_battery_ok());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_manager_is_connected());
    cJSON_AddStringToObject(root, "ip", wifi_manager_get_ip());
    int64_t t_wifi = esp_timer_get_time();
    const weather_data_t *w = weather_get();
    if (w && w->valid) {
        cJSON *wj = cJSON_AddObjectToObject(root, "weather");
        cJSON_AddNumberToObject(wj, "temp_c", w->temp_c);
        cJSON_AddNumberToObject(wj, "humidity", w->humidity);
        cJSON_AddStringToObject(wj, "condition", w->condition);
    }
    int64_t t_weather = esp_timer_get_time();
    sht30_reading_t sensor;
    if (sht30_get(&sensor)) {
        cJSON *sj = cJSON_AddObjectToObject(root, "sensor");
        cJSON_AddNumberToObject(sj, "temp_c",   sensor.temp_c);
        cJSON_AddNumberToObject(sj, "humidity", sensor.humidity);
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
    cJSON_AddNumberToObject(root, "heap_largest",
                            (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "psram_free",
                            (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "psram_largest",
                            (double)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    int64_t t_heap = esp_timer_get_time();
    {
        if (!s_fs_cache_valid) {
            esp_littlefs_info("littlefs", &s_fs_total_cached, &s_fs_used_cached);
            s_fs_cache_valid = true;
        }
        cJSON_AddNumberToObject(root, "fs_total", (double)s_fs_total_cached);
        cJSON_AddNumberToObject(root, "fs_used",  (double)s_fs_used_cached);
    }
    int64_t t_fsinfo = esp_timer_get_time();
    cJSON_AddStringToObject(root, "firmware", FW_VERSION_STR);
    /* expected_fs: the LittleFS version this firmware binary was built against.
     * Baked in at compile time from version.json → fs_version.
     * fs_version: the version actually present on the device, read at
     * runtime from /spiffs/web/version.txt (written when littlefs.bin was flashed).
     * The UI shows a mismatch banner when these two differ — i.e. the LittleFS
     * image on the device is not the one this firmware expects. */
    cJSON_AddStringToObject(root, "expected_fs", FS_VERSION_STR);
    /* Theme JPEG decode error — non-null when the active theme has an image
     * that couldn't be decoded (wrong size, truncated file, etc.).
     * Cleared automatically when the theme changes. */
    const char *te = display_get_theme_error();
    if (te) cJSON_AddStringToObject(root, "theme_error", te);
    else    cJSON_AddNullToObject  (root, "theme_error");
    char fs_ver[32] = "unknown";
    FILE *vf = fopen("/spiffs/web/version.txt", "r");
    if (vf) {
        if (fgets(fs_ver, sizeof(fs_ver), vf))
            fs_ver[strcspn(fs_ver, "\r\n")] = '\0';
        fclose(vf);
    }
    cJSON_AddStringToObject(root, "fs_version", fs_ver);
    int64_t t_fsver = esp_timer_get_time();
    app_mode_t status_mode;
    bool       status_mic_cal;
    config_lock();
    const nextube_config_t *scfg = config_get();
    status_mode    = scfg->current_mode;
    status_mic_cal = scfg->mic_calibration_saved;
    config_unlock();
    int64_t t_config = esp_timer_get_time();
    cJSON_AddStringToObject(root, "mode", app_mode_name(status_mode));
    cJSON_AddBoolToObject(root, "mic_calibration_saved", status_mic_cal);
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
                    st == ESP_OTA_IMG_ABORTED) {
                rollback = true;
                esp_app_desc_t desc;
                if (esp_ota_get_partition_description(inactive, &desc) == ESP_OK)
                    cJSON_AddStringToObject(root, "ota_rollback_ver", desc.version);
            }
        }
        cJSON_AddBoolToObject(root, "ota_rollback", rollback);
    }
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
 * fetches it only when the Network Info panel is expanded.  Auth-open, same
 * tier as /api/status (REQUIRE_AUTH is reserved for mutation handlers). */
static esp_err_t api_network_info(httpd_req_t *r)
{
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
 * Suspend every non-essential background task before any flash write so we
 * reduce CPU/bus contention during esp_ota_write()'s sector erase+program
 * cycles.  Each erase (≈25 ms) disables the ESP32 data cache; concurrent SPI
 * DMA (LED RMT), I2S (mic), HTTPS polls (weather / subscribers / ntp) and I2C
 * reads (sht30) all compete for CPU during those windows, contributing to TCP
 * packet loss that stretches OTA transfers and eventually trips the recv timeout.
 *
 * We resolve tasks by their registered FreeRTOS name — no task handle needs to
 * be exposed from each component (INCLUDE_xTaskGetHandle is always 1 in the
 * ESP-IDF FreeRTOS build).  Since both OTA paths always end with esp_restart(),
 * there is no matching resume call.
 *
 * The display task is handled separately by display_show_wait(), which also
 * ensures the SPI bus is free before the first flash write. */
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

    static const char *const k_tasks[] = {
        "weather",    /* HTTPS polling — competes with OTA TCP stream + heap */
        "subscribers", /* HTTPS polling — competes with OTA TCP stream + heap */
        "ntp",        /* SNTP/UDP      — adds lwIP load during flash writes  */
        "sht30",      /* I2C sensor    — periodic wakeups, CPU cycles        */
        "leds",       /* RMT DMA       — Core 1 bus traffic during writes    */
        "mic",        /* I2S/ADC DMA   — continuous DMA, CPU interrupts      */
        "audio_play", /* DAC/I2S       — ephemeral, only present if playing  */
        "ha_mqtt",    /* MQTT broker connect/publish — was missing entirely;
                       * observed in the field retrying esp_mqtt_client_start()
                       * every 5 s throughout an entire webui-pull download +
                       * extraction window, each attempt trying to spawn a new
                       * internal esp-mqtt task and failing ("Error create mqtt
                       * task") — the same transient internal-SRAM contention
                       * class as the mDNS receive-buffer failures, just
                       * landing on MQTT's task creation instead.  Suspending
                       * this only pauses OUR wrapper task (its own connect
                       * retry loop + 60 s publish loop); if a connection was
                       * ALREADY established before this call, the underlying
                       * esp-mqtt library's own internal worker task is a
                       * separate task not covered here and keeps running —
                       * acceptable, since an idle established connection is
                       * far lighter than the active-connect-attempt storm
                       * this fixes. */
        NULL,
    };
    for (int i = 0; k_tasks[i]; i++) {
        TaskHandle_t h = xTaskGetHandle(k_tasks[i]);
        if (h) {
            vTaskSuspend(h);
            ESP_LOGI(TAG, "OTA: suspended '%s'", k_tasks[i]);
        }
    }
    /* Brief yield so any task currently executing its current time-slice
     * can finish its current instruction and reach a safe stack state before
     * the flash cache is disabled by the first esp_ota_write(). */
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void webui_resume_tasks(void)
{
    static const char *const k_tasks[] = {
        "weather", "subscribers", "ntp", "sht30", "leds", "mic", "audio_play", "ha_mqtt", NULL,
    };
    for (int i = 0; k_tasks[i]; i++) {
        TaskHandle_t h = xTaskGetHandle(k_tasks[i]);
        if (h) {
            vTaskResume(h);
            ESP_LOGI(TAG, "[webui] resumed '%s'", k_tasks[i]);
        }
    }
}

/* ── OTA double-flash guard + clean deferred reboot ────────────────────────
 * Two protections against an OTA being applied twice (observed when the
 * browser tab is backgrounded during a flash):
 *
 *  1. s_ota_active — rejects a second /api/update_firmware (or /api/update_fs)
 *     while one is already running, returning HTTP 409.  Set by the api_ota /
 *     api_fs_ota wrappers and cleared only if the flash FAILS; a successful
 *     flash reboots, so it intentionally stays set until then.
 *
 *  2. Deferred reboot — calling esp_restart() inside the handler tears the TCP
 *     socket with an RST before a slow/backgrounded browser has read the 200.
 *     The browser then sees a broken connection with no response and
 *     transparently retries the POST → a second flash.  Instead we send the
 *     response, set "Connection: close", return ESP_OK so the HTTP server
 *     flushes the body and closes the socket cleanly, and fire esp_restart()
 *     from a one-shot timer ~1.5 s later — by which time the browser has its
 *     answer and has no reason to retry.  (The esp_timer task is not suspended
 *     by ota_suspend_tasks(), so this callback still runs.) */
static volatile bool s_ota_active    = false;
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
            return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin fail"), ESP_FAIL;
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
        if (esp_ota_begin(upd, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK)
            return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin fail"), ESP_FAIL;

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
                FS_OTA_FAIL("Erase failed");
            if (esp_partition_write(part, written, buf, FS_SECTOR) != ESP_OK)
                FS_OTA_FAIL("Write failed");
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

static esp_err_t api_fs_hotpatch(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
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

    /* Invalidate the shell cache NOW so any request arriving during extraction
     * gets a cache-miss and re-reads from LittleFS after the new file lands,
     * not the stale pre-extraction version. */
    shell_cache_flush();

    cJSON *files_arr = cJSON_CreateArray();
    int ok = 0, skipped = 0, failed = 0;

    const uint8_t *p   = zip;
    const uint8_t *end = zip + rx;

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
        if (fname[fnl - 1] == '/') continue;

        /* Reject path traversal. */
        if (strstr(fname, "..") || strstr(fname, "//")) {
            ESP_LOGW(TAG, "hotpatch: rejected unsafe path '%s'", fname);
            failed++;
            continue;
        }

        /* Protect user config — it must never be overwritten by a hotpatch. */
        if (strcmp(fname, "config.json") == 0) {
            ESP_LOGI(TAG, "hotpatch: skipped protected file config.json");
            skipped++;
            continue;
        }

        /* Only STORE (uncompressed) entries are supported. */
        if (h->method != 0) {
            ESP_LOGW(TAG, "hotpatch: skipped '%s' (method=%u; only STORE supported — "
                          "regenerate ZIP with 'zip -0')", fname, h->method);
            skipped++;
            continue;
        }

        /* Build VFS path and create any missing parent dirs. */
        char vpath[320];
        snprintf(vpath, sizeof(vpath), "/spiffs/%s", fname);
        hp_mkdir_p(vpath);

        FILE *f = fopen(vpath, "wb");
        if (!f) {
            ESP_LOGE(TAG, "hotpatch: fopen failed for '%s': %s", vpath, strerror(errno));
            failed++;
            continue;
        }
        size_t written = fwrite(data, 1, h->comp_sz, f);
        fclose(f);

        if (written != h->comp_sz) {
            ESP_LOGE(TAG, "hotpatch: short write '%s' (%u/%u)", fname,
                     (unsigned)written, (unsigned)h->comp_sz);
            failed++;
        } else {
            ESP_LOGI(TAG, "hotpatch: wrote '%s' (%u B)", fname, (unsigned)h->comp_sz);
            cJSON_AddItemToArray(files_arr, cJSON_CreateString(fname));
            ok++;
        }
        vTaskDelay(1);   /* yield between writes — flash HAL disables IRQs, starves IDLE
                          * (same fix as webui_pull_task's identical extraction loop) */
    }

    free(zip);
    hp_drop_stale_index();
    display_theme_cache_flush();   /* re-probe PNG format on next render */
    fs_usage_invalidate();
    ESP_LOGI(TAG, "hotpatch complete: %d written, %d skipped, %d failed",
             ok, skipped, failed);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", failed == 0 ? "ok" : "partial");
    cJSON_AddNumberToObject(resp, "written", ok);
    cJSON_AddNumberToObject(resp, "skipped", skipped);
    cJSON_AddNumberToObject(resp, "failed",  failed);
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

    /* ota_suspend_tasks() suspends weather/subscribers, which independently
     * take this SAME tls_sem for their own HTTPS fetches — it must run AFTER
     * we've already acquired the semaphore above, never before.  If it ran
     * first and one of them happened to be mid-handshake (holding tls_sem)
     * at that instant, vTaskSuspend would freeze it there forever without
     * releasing the semaphore, and our tls_sem_take() above would then burn
     * its full 30 s timeout and proceed UNSERIALIZED — reintroducing the
     * internal-RAM-fragmentation risk tls_sem exists to prevent.  Suspending
     * them here, once we already hold the semaphore ourselves, closes that
     * window entirely while still keeping them off the bus for the entire
     * download loop below (not just the flash write, as before). */
    ota_suspend_tasks();

    char s_ota_ua[128];
    { const char *r = config_get()->update_repo;
      snprintf(s_ota_ua, sizeof(s_ota_ua), HTTP_UA_BASE "%s", r[0] ? r : HTTP_UA_REPO_DEFAULT); }
    esp_http_client_config_t hcfg = {
        .url                   = s_pull.url,
        .timeout_ms            = OTA_PULL_TIMEOUT_MS,
        .user_agent            = s_ota_ua,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .max_redirection_count = 3,
        .buffer_size           = 16384,   /* Azure Blob returns many x-ms-* headers */
        .buffer_size_tx        = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) {
        snprintf(s_pull.error, sizeof(s_pull.error), "HTTP client init failed");
        ESP_LOGE(TAG, "[ota] %s", s_pull.error);
        goto pull_err_notls;
    }
    ESP_LOGD(TAG, "[ota] HTTP client created");

    /* Open + follow redirects: each hop needs a fresh open() to the new host. */
    int content_len = -1, status_code = -1;
    for (int rd = 0; rd <= 5; rd++) {
        ESP_LOGD(TAG, "[ota] HTTP open hop %d…", rd);
        if (esp_http_client_open(client, 0) != ESP_OK) {
            snprintf(s_pull.error, sizeof(s_pull.error), "HTTP connect failed (hop %d)", rd);
            ESP_LOGE(TAG, "[ota] %s", s_pull.error);
            esp_http_client_cleanup(client);
            goto pull_err_notls;
        }
        content_len = esp_http_client_fetch_headers(client);
        status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "[ota] hop %d → HTTP %d, content-length=%d", rd, status_code, content_len);
        if (status_code == 200) break;
        if (status_code != 301 && status_code != 302 &&
            status_code != 307 && status_code != 308) break;
        ESP_LOGD(TAG, "[ota] following redirect…");
        esp_http_client_set_redirection(client);
    }

    if (status_code != 200 || content_len <= 0) {
        snprintf(s_pull.error, sizeof(s_pull.error),
                 "HTTP %d (content-length=%d)", status_code, content_len);
        ESP_LOGE(TAG, "[ota] %s", s_pull.error);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto pull_err_notls;
    }
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

    int received = 0;
    int last_pct = -1;
    while (received < content_len) {
        int want = content_len - received;
        if (want > 4096) want = 4096;
        int n = esp_http_client_read(client, (char *)img + received, want);
        if (n < 0) {
            snprintf(s_pull.error, sizeof(s_pull.error),
                     "Download error at %d/%d B", received, content_len);
            ESP_LOGE(TAG, "[ota] %s", s_pull.error);
            free(img);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            goto pull_err_notls;
        }
        if (n == 0) break;
        received += n;
        s_pull.bytes_received = received;
        s_pull.progress = (received * 60) / content_len;
        int pct = (received * 100) / content_len;
        if (pct / 10 != last_pct / 10) {
            last_pct = pct;
            ESP_LOGI(TAG, "[ota] download %d%% (%d/%d B)", pct, received, content_len);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
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
        snprintf(s_pull.error, sizeof(s_pull.error), "OTA begin failed");
        ESP_LOGE(TAG, "[ota] %s", s_pull.error);
        free(img);
        goto pull_err;
    }
    ESP_LOGD(TAG, "[ota] esp_ota_begin OK — flashing %d B…", received);

    const uint8_t *p = img;
    int rem = received;
    last_pct = -1;
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
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return;

pull_err_notls:
    tls_sem_give();
pull_err:
    ESP_LOGE(TAG, "[ota] FAILED: %s", s_pull.error);
    s_pull.state = OTA_PULL_ERROR;
    s_ota_active = false;
    vTaskDelete(NULL);
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

    int len = r->content_len;
    if (len <= 0 || len > 1024)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad body"), ESP_FAIL;

    char *body = malloc(len + 1);
    if (!body) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;

    int rx = 0;
    while (rx < len) {
        int n = httpd_req_recv(r, body + rx, len - rx);
        if (n <= 0) { free(body); return ESP_FAIL; }
        rx += n;
    }
    body[len] = '\0';

    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad JSON"), ESP_FAIL;

    cJSON *url_j = cJSON_GetObjectItem(j, "url");
    if (!cJSON_IsString(url_j) || !url_j->valuestring || !url_j->valuestring[0]) {
        cJSON_Delete(j);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing url"), ESP_FAIL;
    }
    strncpy(s_pull.url, url_j->valuestring, sizeof(s_pull.url) - 1);
    s_pull.url[sizeof(s_pull.url) - 1] = '\0';

    cJSON *hash_j = cJSON_GetObjectItem(j, "sha256");
    if (cJSON_IsString(hash_j) && hash_j->valuestring && strlen(hash_j->valuestring) == 64) {
        strncpy(s_pull.sha256, hash_j->valuestring, 64);
        s_pull.sha256[64] = '\0';
    } else {
        s_pull.sha256[0] = '\0';
    }

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

    if (xTaskCreatePinnedToCore(ota_pull_task, "ota_pull", 8192, NULL, 5, NULL, 0) != pdPASS) {
        s_ota_active = false;
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Task create failed"), ESP_FAIL;
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
} s_webui;

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

    /* Suspend competing background tasks for the duration of the download and
     * extraction — reduces network/flash contention and prevents WDT triggers
     * during the long LittleFS write phase.  Resumed at all exit points.
     *
     * MUST run AFTER tls_sem_take() above, never before: ota_suspend_tasks()
     * suspends "weather"/"subscribers", which independently take this SAME
     * tls_sem for their own HTTPS fetches.  If it ran first and one of them
     * was mid-handshake (holding tls_sem) at that instant, vTaskSuspend would
     * freeze it there forever without releasing the semaphore, and our
     * tls_sem_take() above would then burn its full 30 s timeout and proceed
     * UNSERIALIZED — reintroducing the internal-RAM-fragmentation /
     * MBEDTLS_ERR_RSA_PUBLIC_FAILED risk tls_sem exists to prevent (see the
     * TLS memory strategy comment in sdkconfig.defaults).  Suspending them
     * here, once we already hold the semaphore ourselves, closes that
     * window entirely while still keeping them off the bus for the download
     * + extraction that follows. */
    ota_suspend_tasks();
    tasks_suspended = true;

    char s_webui_ua[128];
    { const char *r = config_get()->update_repo;
      snprintf(s_webui_ua, sizeof(s_webui_ua), HTTP_UA_BASE "%s", r[0] ? r : HTTP_UA_REPO_DEFAULT); }
    esp_http_client_config_t hcfg = {
        .url                   = s_webui.url,
        .timeout_ms            = OTA_PULL_TIMEOUT_MS,
        .user_agent            = s_webui_ua,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .max_redirection_count = 3,
        .buffer_size           = 16384,
        .buffer_size_tx        = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) {
        snprintf(s_webui.error, sizeof(s_webui.error), "HTTP client init failed");
        ESP_LOGE(TAG, "[webui] %s", s_webui.error);
        goto webui_err_notls;
    }
    ESP_LOGD(TAG, "[webui] HTTP client created");

    int content_len = -1, status_code = -1;
    for (int rd = 0; rd <= 5; rd++) {
        ESP_LOGD(TAG, "[webui] HTTP open hop %d…", rd);
        if (esp_http_client_open(client, 0) != ESP_OK) {
            snprintf(s_webui.error, sizeof(s_webui.error), "HTTP connect failed (hop %d)", rd);
            ESP_LOGE(TAG, "[webui] %s", s_webui.error);
            esp_http_client_cleanup(client);
            goto webui_err_notls;
        }
        content_len = esp_http_client_fetch_headers(client);
        status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "[webui] hop %d → HTTP %d, content-length=%d", rd, status_code, content_len);
        if (status_code == 200) break;
        if (status_code != 301 && status_code != 302 &&
            status_code != 307 && status_code != 308) break;
        ESP_LOGD(TAG, "[webui] following redirect…");
        esp_http_client_set_redirection(client);
    }

    if (status_code != 200 || content_len <= 0 || content_len > HP_MAX_ZIP) {
        snprintf(s_webui.error, sizeof(s_webui.error),
                 "HTTP %d (content-length=%d)", status_code, content_len);
        ESP_LOGE(TAG, "[webui] %s", s_webui.error);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        goto webui_err_notls;
    }
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

    int received = 0;
    int last_pct = -1;
    while (received < content_len) {
        int n = esp_http_client_read(client, (char *)zip + received, content_len - received);
        if (n < 0) {
            snprintf(s_webui.error, sizeof(s_webui.error),
                     "Download error at %d/%d B", received, content_len);
            ESP_LOGE(TAG, "[webui] %s", s_webui.error);
            free(zip);
            esp_http_client_close(client); esp_http_client_cleanup(client);
            goto webui_err_notls;
        }
        if (n == 0) break;
        received += n;
        int pct = (received * 100) / content_len;
        if (pct / 10 != last_pct / 10) {
            last_pct = pct;
            ESP_LOGI(TAG, "[webui] download %d%% (%d/%d B)", pct, received, content_len);
        }
    }
    esp_http_client_close(client); esp_http_client_cleanup(client);
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
        int ok = 0, skipped = 0, failed = 0;
        const uint8_t *p = zip, *end = zip + received;
        while (p + (int)sizeof(zip_lfh_t) <= end) {
            uint32_t sig = (uint32_t)(p[0]|p[1]<<8|p[2]<<16|p[3]<<24);
            if (sig == ZIP_CDH_SIG || sig == ZIP_EOCD_SIG) break;
            if (sig != ZIP_LFH_SIG) { p++; continue; }
            const zip_lfh_t *h = (const zip_lfh_t *)p;
            p += sizeof(zip_lfh_t);
            /* Size-based bounds checks — see the hotpatch extractor: the
             * `p + comp_sz > end` form can wrap on 32-bit (comp_sz is an
             * attacker/corruption-controlled uint32_t) and bypass the guard,
             * causing an OOB read in the fwrite below. */
            if ((size_t)(h->fname_len + h->extra_len) > (size_t)(end - p)) break;
            char fname[256] = {0};
            int fnl = h->fname_len < 255 ? h->fname_len : 255;
            memcpy(fname, p, fnl);
            p += h->fname_len + h->extra_len;
            if (h->comp_sz > (size_t)(end - p)) break;
            const uint8_t *data = p;
            p += h->comp_sz;
            if (fnl == 0) continue;
            if (fname[fnl-1] == '/') { ESP_LOGD(TAG, "[webui]  dir  %s", fname); continue; }
            if (strstr(fname, "..") || strstr(fname, "//")) {
                ESP_LOGW(TAG, "[webui]  SKIP (path traversal) %s", fname);
                failed++; continue;
            }
            if (strcmp(fname, "config.json") == 0) {
                ESP_LOGI(TAG, "[webui]  skip (config.json protected)");
                skipped++; continue;
            }
            if (h->method != 0) {
                ESP_LOGW(TAG, "[webui]  skip (compressed method=%u) %s", h->method, fname);
                skipped++; continue;
            }
            char vpath[320];
            snprintf(vpath, sizeof(vpath), "/spiffs/%s", fname);
            hp_mkdir_p(vpath);
            FILE *f = fopen(vpath, "wb");
            if (!f) {
                ESP_LOGE(TAG, "[webui]  FAIL fopen %s", vpath);
                failed++; continue;
            }
            size_t wr = fwrite(data, 1, h->comp_sz, f);
            fclose(f);
            if (wr != h->comp_sz) {
                ESP_LOGE(TAG, "[webui]  FAIL write %s (%u/%u B)", fname, (unsigned)wr, (unsigned)h->comp_sz);
                failed++;
            } else {
                ESP_LOGD(TAG, "[webui]  ok   %s (%u B)", fname, (unsigned)h->comp_sz);
                ok++;
            }
            vTaskDelay(1);   /* yield between writes — flash HAL disables IRQs, starves IDLE */
        }
        free(zip);
        hp_drop_stale_index();
        display_theme_cache_flush();   /* re-probe PNG format on next render */
        ESP_LOGI(TAG, "[webui] extraction done: %d written, %d skipped, %d failed", ok, skipped, failed);
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
    vTaskDelete(NULL);
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
    vTaskDelete(NULL);
}

static esp_err_t api_webui_pull_status(httpd_req_t *r)
{
    if (!post_ota_auth_valid()) { REQUIRE_AUTH(r); }
    static const char *const names[] = { "idle","running","done","error" };
    char buf[192];
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"error\":\"%s\"}",
             names[s_webui.state], s_webui.error);
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
    s_ota_active     = true;

    if (xTaskCreatePinnedToCore(webui_pull_task, "webui_pull", 16384, NULL, 5, NULL, 0) != pdPASS) {
        s_ota_active = false;
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Task create failed"), ESP_FAIL;
    }
    return send_json(r, "{\"status\":\"started\"}");
}

static esp_err_t api_webui_pull(httpd_req_t *r)
{
    /* Consume the one-time NVS bypass flag set before OTA reboot. */
    if (!s_post_ota_auth) s_post_ota_auth = consume_post_ota_flag();
    if (!post_ota_auth_valid()) { REQUIRE_AUTH(r); }
    if (s_ota_active) {
        httpd_resp_set_status(r, "409 Conflict");
        return send_json(r, "{\"error\":\"ota_in_progress\"}");
    }

    int len = r->content_len;
    if (len <= 0 || len > 1024)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad body"), ESP_FAIL;

    char *body = malloc(len + 1);
    if (!body) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;

    int rx = 0;
    while (rx < len) {
        int n = httpd_req_recv(r, body + rx, len - rx);
        if (n <= 0) { free(body); return ESP_FAIL; }
        rx += n;
    }
    body[len] = '\0';

    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad JSON"), ESP_FAIL;

    cJSON *url_j = cJSON_GetObjectItem(j, "url");
    if (!cJSON_IsString(url_j) || !url_j->valuestring || !url_j->valuestring[0]) {
        cJSON_Delete(j);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing url"), ESP_FAIL;
    }
    strncpy(s_webui.url, url_j->valuestring, sizeof(s_webui.url) - 1);
    s_webui.url[sizeof(s_webui.url) - 1] = '\0';

    s_webui.sha256[0] = '\0';
    cJSON *hash_j = cJSON_GetObjectItem(j, "sha256");
    if (cJSON_IsString(hash_j) && hash_j->valuestring && strlen(hash_j->valuestring) == 64)
        strncpy(s_webui.sha256, hash_j->valuestring, 64);
    cJSON_Delete(j);

    s_webui.state   = WEBUI_IDLE;
    s_webui.error[0] = '\0';
    s_ota_active     = true;

    if (xTaskCreatePinnedToCore(webui_pull_task, "webui_pull", 16384, NULL, 5, NULL, 0) != pdPASS) {
        s_ota_active = false;
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Task create failed"), ESP_FAIL;
    }
    return send_json(r, "{\"status\":\"started\"}");
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
     * dm_render_asset()/dm_draw_text()), independently on/off colourable.
     * Also ships with no asset folder, for the same reason. */
    strlcpy(names[count++], "DotMatrix", THEME_NAME_MAX);

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
 * overwriting the file.  Directory components must already exist (SPIFFS
 * creates them implicitly via path-prefix emulation). */
/* Hard cap on a single upload's Content-Length.  Prevents a slow-trickle or
 * runaway upload from holding a connection slot indefinitely while consuming
 * filesystem space.  2 MB comfortably accommodates audio clips and theme
 * JPEGs; the WebUI patch route has its own dedicated limit. */
#define MAX_UPLOAD_BYTES (2 * 1024 * 1024)

static esp_err_t api_file_upload(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char q[256], p[256] = {0}, spiffs_path[320];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "path", p, sizeof(p)) != ESP_OK || p[0] == '\0')
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing path"), ESP_FAIL;
    if (!url_decode_inplace(p) || strstr(p, ".."))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid path"), ESP_FAIL;

    /* Hard size cap — reject before opening the destination file so we don't
     * leave a partially-written file behind on rejection. */
    if (r->content_len > MAX_UPLOAD_BYTES)
        return httpd_resp_send_err(r, HTTPD_413_CONTENT_TOO_LARGE,
                                   "File too large (max 2 MB per upload)"), ESP_FAIL;

    snprintf(spiffs_path, sizeof(spiffs_path), "/spiffs%s", p);

    /* Reject the upload if the declared size exceeds available free space. */
    if (r->content_len > 0) {
        size_t total = 0, used = 0;
        esp_littlefs_info("littlefs", &total, &used);
        if ((size_t)r->content_len > (total - used))
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Not enough space"), ESP_FAIL;
    }

    FILE *f = fopen(spiffs_path, "wb");
    if (!f) {
        size_t total = 0, used = 0;
        esp_littlefs_info("littlefs", &total, &used);
        ESP_LOGE(TAG, "fopen(%s, wb) failed: errno=%d (%s)  littlefs total=%u used=%u free=%u",
                 spiffs_path, errno, strerror(errno),
                 (unsigned)total, (unsigned)used, (unsigned)(total - used));
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
    fs_usage_invalidate();
    ESP_LOGI(TAG, "Uploaded: %s (%d bytes)", spiffs_path, received);
    if (strncmp(p, "/images/album/", 14) == 0)
        display_album_invalidate();
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
 * Body: {"mask": <0–63>}
 *   mask is a 6-bit field, one bit per tube (bit 0 = tube 1 … bit 5 = tube 6).
 *   63 (0x3F) = all tubes white.  0 = restore all tubes to normal rendering.
 * While any bit is set the affected tubes show solid white at 100% backlight
 * indefinitely until mask=0 is sent. */
static esp_err_t api_debug_burnin(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char body[64] = {0};
    int blen = (int)r->content_len;
    if (blen <= 0 || blen >= (int)sizeof(body))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Body required"), ESP_FAIL;
    if (httpd_req_recv(r, body, (size_t)blen) != blen)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Read error"), ESP_FAIL;
    cJSON *root = cJSON_Parse(body);
    if (!root)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid JSON"), ESP_FAIL;
    cJSON *jm = cJSON_GetObjectItem(root, "mask");
    cJSON *jd = cJSON_GetObjectItem(root, "duration_s");
    uint8_t  mask       = cJSON_IsNumber(jm) ? (uint8_t)(jm->valueint & 0x3F) : 0;
    uint32_t duration_s = cJSON_IsNumber(jd) ? (uint32_t)jd->valueint : 0;
    cJSON_Delete(root);
    display_set_burnin_mask(mask, duration_s);
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
    char body[64] = {0};
    int blen = (int)r->content_len;
    if (blen <= 0 || blen >= (int)sizeof(body))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Body required"), ESP_FAIL;
    if (httpd_req_recv(r, body, (size_t)blen) != blen)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Read error"), ESP_FAIL;
    cJSON *root = cJSON_Parse(body);
    if (!root)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid JSON"), ESP_FAIL;
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
    char body[64] = {0};
    int blen = (int)r->content_len;
    if (blen <= 0 || blen >= (int)sizeof(body))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Body required (≤63 bytes)"), ESP_FAIL;
    if (httpd_req_recv(r, body, (size_t)blen) != blen)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Read error"), ESP_FAIL;
    cJSON *root = cJSON_Parse(body);
    if (!root)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid JSON"), ESP_FAIL;
    const cJSON *ja = cJSON_GetObjectItem(root, "active");
    bool active = cJSON_IsTrue(ja);
    cJSON_Delete(root);
    display_set_update_indicator(active);
    ESP_LOGI(TAG, "update_notify: clock-face indicator %s", active ? "ON" : "OFF");
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
    char body[256] = {0};
    int  blen = (int)r->content_len;
    if (blen <= 0 || blen >= (int)sizeof(body))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Body required (≤255 bytes)");
    if (httpd_req_recv(r, body, (size_t)blen) != blen)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Read error");

    cJSON *root = cJSON_Parse(body);
    if (!root)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid JSON");

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
    char body[256] = {0};
    int  blen = (int)r->content_len;
    if (blen <= 0 || blen >= (int)sizeof(body))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Body required (≤255 bytes)");
    if (httpd_req_recv(r, body, (size_t)blen) != blen)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Read error");

    cJSON *root = cJSON_Parse(body);
    if (!root)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid JSON");

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
    char buf[128] = {0};
    int  n = httpd_req_recv(r, buf, sizeof(buf) - 1);
    if (n <= 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "No body"), ESP_FAIL;
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad JSON"), ESP_FAIL;

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

    size_t cap = MIC_BAND_COUNT * 96 + 96;
    char  *out = malloc(cap);
    if (!out) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;

    size_t off = (size_t)snprintf(out, cap, "{\"bands\":[");
    for (int b = 0; b < MIC_BAND_COUNT; b++)
        off += (size_t)snprintf(out + off, cap - off,
            "%s{\"i\":%d,\"raw\":%.3f,\"floor\":%.3f,\"power\":%.3f,\"disp\":%.3f}",
            b ? "," : "", b, raw[b], floor_[b], power[b], bands[b]);
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
    off += (size_t)snprintf(out + off, cap - off, "],\"dec_rate_hz\":8000,\"dec\":[");
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

/* GET /api/debug/tasks — per-task CPU accounting (FreeRTOS runtime stats).
 *
 * `pct` is an **instantaneous** share measured over a short window inside the
 * handler: two uxTaskGetSystemState() snapshots ~250 ms apart, diffed per task
 * (matched by handle) against the wall-clock window.  This is robust to the
 * 32-bit runtime-counter wrap (~71 min) — an earlier "lifetime" pct (run/total)
 * produced garbage >100% once the counters wrapped.  pct is per-chip: both
 * cores accrue, so the sum across all tasks ≈ 200% (IDLE0+IDLE1 absorb slack).
 * `run_us` is still the raw 32-bit lifetime counter (µs) for manual diffing. */
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

static esp_err_t api_wifi_scan_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    uint16_t cnt = 0;
    esp_wifi_scan_get_ap_num(&cnt);
    if (cnt == 0) return send_json(r, "[]");
    if (cnt > 20) cnt = 20;
    wifi_ap_record_t *list = calloc(cnt, sizeof(wifi_ap_record_t));
    if (!list) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL;
    esp_wifi_scan_get_ap_records(&cnt, list);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < cnt; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (char*)list[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", list[i].rssi);
        cJSON_AddNumberToObject(ap, "auth", list[i].authmode);
        cJSON_AddItemToArray(arr, ap);
    }
    free(list);
    char *json = cJSON_PrintUnformatted(arr);
    esp_err_t ret = send_json(r, json);
    free(json); cJSON_Delete(arr);
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
    httpd_resp_set_hdr(r, "Cache-Control", "max-age=3600");

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
    R(HTTP_POST, "/api/weather",         api_post_weather),
    R(HTTP_POST, "/api/cx_image",        api_cx_image),
    R(HTTP_GET,  "/api/status",          api_status),
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
    R(HTTP_GET,  "/api/debug/tasks",     api_debug_tasks),
    R(HTTP_GET,  "/api/debug/micframe",  api_debug_micframe),
    R(HTTP_GET,  "/api/debug/micbands",  api_debug_micbands),
    R(HTTP_POST, "/api/mic/calibrate",          api_mic_calibrate),
    R(HTTP_POST, "/api/mic/reset_calibration",  api_mic_reset_calibration),
    R(HTTP_POST, "/api/update_notify",          api_update_notify),
    R(HTTP_POST, "/api/ota_pull",              api_ota_pull),
    R(HTTP_GET,  "/api/ota_pull_status",       api_ota_pull_status),
    R(HTTP_POST, "/api/webui_pull",            api_webui_pull),
    R(HTTP_GET,  "/api/webui_pull_auto",       api_webui_pull_auto),
    R(HTTP_GET,  "/api/webui_pull_status",     api_webui_pull_status),
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
 * Background: the WebUI ZIP is applied AFTER the firmware reboot.  The old
 * design relied on the browser re-connecting through the post-reboot network
 * churn (mDNS re-registration + httpd connection resets) to call
 * api_webui_pull_auto.  In practice that trigger lands during the unstable
 * window right after boot and gets reset (errno 104) before the handler runs,
 * so webui_pull_task never starts and the update silently stalls.
 *
 * Fix: the device starts the pull itself.  ota_pull_task already stored
 * webui_url + webui_sha256 in NVS before rebooting, so everything needed is on
 * hand.  This task waits for connectivity, then spawns webui_pull_task exactly
 * as api_webui_pull_auto would.  The browser's Phase 4 only has to poll
 * /api/webui_pull_status for progress — it no longer has to TRIGGER anything.
 *
 * s_ota_active is set before spawning, so a stray browser trigger that does
 * arrive hits the 409 guard in api_webui_pull_auto/api_webui_pull and cannot
 * double-spawn. */
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
    s_ota_active     = true;

    ESP_LOGI(TAG, "[post_ota] starting device-driven WebUI pull: %.80s", s_webui.url);
    if (xTaskCreatePinnedToCore(webui_pull_task, "webui_pull", 16384, NULL, 5, NULL, 0) != pdPASS) {
        s_ota_active = false;
        ESP_LOGE(TAG, "[post_ota] webui_pull task create failed");
    }
    vTaskDelete(NULL);
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
        esp_log_set_vprintf(log_vprintf_hook);
        /* Restart the HTTP server whenever STA obtains a (new) IP address,
         * so the listening socket is always fresh after a credential change. */
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   web_server_got_ip_handler, NULL);
        /* Initialise admin auth state (in-RAM session table, lockout counters).
         * NVS is already initialised by main.c::init_nvs() before web_server_start. */
        auth_init();

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
                 * browser to reach us through the post-reboot network churn. */
                if (xTaskCreatePinnedToCore(post_ota_autostart_task, "post_ota_auto",
                                           4096, NULL, 4, NULL, 0) != pdPASS)
                    ESP_LOGE(TAG, "[boot] post_ota_autostart_task creation failed");
            }
        }
    }

    if (s_server) return;   /* already running */

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* Route count + headroom.  MUST exceed the uris[] table size + 1 for the
     * static wildcard registered after it: when this cap is hit, the excess
     * registrations fail and — because the wildcard registers LAST — the
     * symptom is "Nothing matches the given URI" on every web UI page while
     * the APIs still work.  The loop below now logs any failure loudly. */
    cfg.max_uri_handlers = 64;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.stack_size       = 8192;
    /* Bump the open-socket cap so concurrent OTA + page reload + status polls
     * don't exhaust the default 7-slot pool.  Each socket costs ~1.3 KB
     * internal RAM; 12 sockets ≈ 16 KB total. */
    cfg.max_open_sockets  = 12;
    /* Default recv/send timeouts are 5 s; bump to 10 s so a slow LAN doesn't
     * abort mid-handler.  The OTA path overrides recv to 60 s per-request via
     * setsockopt — that override remains in api_ota(). */
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
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
