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

#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "auth.h"

static const char *TAG = "web_srv";
static httpd_handle_t s_server = NULL;
static bool s_server_restart_pending = false;   /* set when a WiFi reconnect stops the server */

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
    char *j = config_to_json();
    if (!j) return send_json(r, "{}");
    /* Strip WiFi password — it must never travel over the wire on a GET.
     * POST /api/settings accepts a new password only when the caller supplies
     * an explicit non-empty value, so the UI never needs to read it back. */
    cJSON *root = cJSON_Parse(j);
    free(j);
    if (root) {
        /* Replace the plaintext password with a boolean indicator so the UI
         * can show a masked placeholder without exposing the actual value. */
        const cJSON *pw = cJSON_GetObjectItem(root, "password");
        bool has_pw = cJSON_IsString(pw) && pw->valuestring && pw->valuestring[0] != '\0';
        cJSON_DeleteItemFromObject(root, "password");
        cJSON_AddBoolToObject(root, "has_password", has_pw);
        j = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
    } else {
        j = NULL;
    }
    esp_err_t ret = send_json(r, j ? j : "{}");
    free(j);
    return ret;
}

/* GET /api/backup — full config including WiFi password, for explicit user backup.
 * Separate from GET /api/settings so the password is not exposed on the general
 * settings endpoint but IS preserved in backup/restore round-trips. */
static esp_err_t api_backup(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char *j = config_to_json();
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
    /* Validate on the signed content_len first (catches negative/absent header),
     * then widen to size_t so all subsequent arithmetic is unsigned. */
    if (r->content_len <= 0 || r->content_len > 4096)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad length"), ESP_FAIL;
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
     * changing it live is not supported.  audio_enabled uses dac_oneshot at
     * boot when disabled; mixing oneshot and dac_continuous in the same session
     * is unreliable on original ESP32, so it also requires a reboot.
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
    cJSON *root = cJSON_CreateObject();
    struct tm t; ntp_get_local(&t);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &t);
    cJSON_AddStringToObject(root, "time", ts);
    cJSON_AddBoolToObject(root, "ntp_synced",      ntp_time_synced());
    cJSON_AddBoolToObject(root, "rtc_battery_ok",  ntp_rtc_battery_ok());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_manager_is_connected());
    cJSON_AddStringToObject(root, "ip", wifi_manager_get_ip());
    const weather_data_t *w = weather_get();
    if (w && w->valid) {
        cJSON *wj = cJSON_AddObjectToObject(root, "weather");
        cJSON_AddNumberToObject(wj, "temp_c", w->temp_c);
        cJSON_AddNumberToObject(wj, "humidity", w->humidity);
        cJSON_AddStringToObject(wj, "condition", w->condition);
    }
    const sht30_reading_t *sensor = sht30_get();
    if (sensor && sensor->valid) {
        cJSON *sj = cJSON_AddObjectToObject(root, "sensor");
        cJSON_AddNumberToObject(sj, "temp_c",   sensor->temp_c);
        cJSON_AddNumberToObject(sj, "humidity", sensor->humidity);
    }
    const sub_count_t *s = subscribers_get();
    if (s && s->valid) cJSON_AddNumberToObject(root, "subscribers", s->subscriber_count);
    const sub_count_t *insta = instagram_get();
    if (insta && insta->valid) cJSON_AddNumberToObject(root, "instagram_followers", insta->subscriber_count);
    const sub_count_t *tt = tiktok_get();
    if (tt && tt->valid) cJSON_AddNumberToObject(root, "tiktok_followers", tt->subscriber_count);
    const sub_count_t *mt = mastodon_get();
    if (mt && mt->valid) cJSON_AddNumberToObject(root, "mastodon_followers", mt->subscriber_count);
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
    {
        size_t fs_total = 0, fs_used = 0;
        esp_littlefs_info("littlefs", &fs_total, &fs_used);
        cJSON_AddNumberToObject(root, "fs_total", (double)fs_total);
        cJSON_AddNumberToObject(root, "fs_used",  (double)fs_used);
    }
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
    app_mode_t status_mode;
    bool       status_mic_cal;
    config_lock();
    const nextube_config_t *scfg = config_get();
    status_mode    = scfg->current_mode;
    status_mic_cal = scfg->mic_calibration_saved;
    config_unlock();
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
    static const char *const k_tasks[] = {
        "weather",    /* HTTPS polling — competes with OTA TCP stream + heap */
        "subscribers", /* HTTPS polling — competes with OTA TCP stream + heap */
        "ntp",        /* SNTP/UDP      — adds lwIP load during flash writes  */
        "sht30",      /* I2C sensor    — periodic wakeups, CPU cycles        */
        "leds",       /* RMT DMA       — Core 1 bus traffic during writes    */
        "mic",        /* I2S/ADC DMA   — continuous DMA, CPU interrupts      */
        "audio_play", /* DAC/I2S       — ephemeral, only present if playing  */
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
static volatile bool s_ota_active = false;

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

        if (p + h->fname_len + h->extra_len > end) break;
        char fname[256] = {0};
        int fnl = h->fname_len < 255 ? h->fname_len : 255;
        memcpy(fname, p, fnl);
        p += h->fname_len + h->extra_len;

        if (p + h->comp_sz > end) break;
        const uint8_t *data = p;
        p += h->comp_sz;

        /* Skip directory entries. */
        if (fnl > 0 && fname[fnl - 1] == '/') continue;

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
    }

    free(zip);
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
            /* stat() the file for its size — use full SPIFFS path. */
            char fp[384];
            snprintf(fp, sizeof(fp), "%s/%s", path, e->d_name);
            struct stat st;
            long sz = (stat(fp, &st) == 0) ? (long)st.st_size : 0;
            n = snprintf(chunk, sizeof(chunk),
                         "%s{\"name\":\"%s\",\"type\":\"file\",\"size\":%ld}",
                         first ? "" : ",", ename, sz);
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

static esp_err_t serve_static(httpd_req_t *r)
{
    const char *uri = r->uri;
    char fp[600];
    if (strcmp(uri,"/")==0) snprintf(fp,sizeof(fp),"/spiffs/web/index.html");
    else snprintf(fp,sizeof(fp),"/spiffs/web%s",uri);
    if (strstr(fp,"..")) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad path"), ESP_FAIL;

    FILE *f = fopen(fp, "rb");
    if (!f) { f = fopen("/spiffs/web/index.html","rb"); }
    if (!f) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found"), ESP_FAIL;

    httpd_resp_set_type(r, content_type(fp));
    httpd_resp_set_hdr(r, "Cache-Control", "max-age=3600");

    /* Use a larger read buffer to cut the number of flash-read iterations —
     * fread() on LittleFS is a synchronous SPI operation that never yields to
     * FreeRTOS, and httpd_resp_send_chunk() returns without blocking as long as
     * the lwIP TCP send buffer has room.  With a 1 KB buffer and a 300+ KB file
     * like index.html the tight fread→send loop holds CPU 0 for ~30 s, starving
     * IDLE0 and triggering the task WDT.
     *
     * Two-pronged fix:
     *   1. 8 KB buffer — reduces loop iterations ~8× vs 1 KB.
     *   2. vTaskDelay(1) after each chunk — blocks httpd for 1 tick so the
     *      scheduler can run IDLE0.  taskYIELD() is insufficient: it re-queues
     *      the caller at the back of its own priority level; IDLE (pri-0) only
     *      runs when NO task of priority ≥ 1 is ready, which never happens
     *      while httpd (pri-5) is continuously re-entering its ready queue. */
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
    R(HTTP_GET,  "/api/status",          api_status),
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
    R(HTTP_POST, "/api/mic/calibrate",          api_mic_calibrate),
    R(HTTP_POST, "/api/mic/reset_calibration",  api_mic_reset_calibration),
    R(HTTP_POST, "/api/update_notify",          api_update_notify),
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
    }

    if (s_server) return;   /* already running */

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 48;                /* current handlers + headroom for future routes */
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

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    for (int i = 0; i < sizeof(uris)/sizeof(uris[0]); i++)
        httpd_register_uri_handler(s_server, &uris[i]);

    /* Wildcard static handler (must be last) */
    httpd_uri_t wildcard = R(HTTP_GET, "/*", serve_static);
    httpd_register_uri_handler(s_server, &wildcard);

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
