#include "web_server.h"
#include "config_mgr.h"
#include "wifi_manager.h"
#include "ntp_time.h"
#include "weather.h"
#include "youtube_bili.h"
#include "display.h"
#include "leds.h"
#include "audio.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "cJSON.h"

#include "freertos/semphr.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "web_srv";
static httpd_handle_t s_server = NULL;

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
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t api_ping(httpd_req_t *r)       { return send_json(r, "{\"status\":\"ok\"}"); }
static esp_err_t api_fw_ver(httpd_req_t *r)      { return send_json(r, "{\"version\":\"" FW_VERSION_STR "\"}"); }
static esp_err_t api_hw_ver(httpd_req_t *r)      { return send_json(r, "{\"version\":\"" HW_VER "\"}"); }

/* POST /api/audio/play  { "file": "/spiffs/audio/bell.wav" }
 * Triggers a one-shot preview of the named audio file at the current volume. */
static esp_err_t api_audio_play(httpd_req_t *r)
{
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
    ESP_LOGI("web_srv", "Audio test: %s", f->valuestring);
    audio_play_file(f->valuestring);
    cJSON_Delete(root);
    return send_json(r, "{\"status\":\"ok\"}");
}

static esp_err_t api_get_settings(httpd_req_t *r)
{
    char *j = config_to_json();
    esp_err_t ret = send_json(r, j ? j : "{}");
    free(j);
    return ret;
}

/* One-shot esp_timer: reconnect WiFi after the HTTP response has been sent.
 * Calling esp_wifi_disconnect() inside the HTTP handler kills the live TCP
 * connection before the response reaches the browser and can also disrupt
 * SPI DMA in-flight, blanking the displays. */
#include "esp_timer.h"
static void reconnect_timer_cb(void *arg)
{
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
    esp_timer_start_once(s_reconnect_timer, 600 * 1000);  /* 600 ms in µs */
}

static esp_err_t api_post_settings(httpd_req_t *r)
{
    int len = r->content_len;
    if (len <= 0 || len > 4096) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad length"), ESP_FAIL;
    char *buf = malloc(len + 1);
    int rx = 0;
    while (rx < len) { int n = httpd_req_recv(r, buf+rx, len-rx); if (n<=0){free(buf);return ESP_FAIL;} rx+=n; }
    buf[len] = 0;
    bool ok = config_set_json(buf, len);
    free(buf);
    const nextube_config_t *cfg = config_get();
    display_set_brightness(cfg->lcd_brightness);
    leds_set_brightness(cfg->led_brightness);
    ntp_apply_timezone();      /* re-apply TZ immediately */
    schedule_wifi_reconnect(); /* reconnect AFTER response is sent */
    return send_json(r, ok ? "{\"status\":\"ok\"}" : "{\"status\":\"error\"}");
}

static esp_err_t api_reset(httpd_req_t *r)
{
    config_reset();
    send_json(r, "{\"status\":\"ok\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* POST /api/reboot — restart the device without touching the config */
static esp_err_t api_reboot(httpd_req_t *r)
{
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
    cJSON_AddBoolToObject(root, "ntp_synced", ntp_time_synced());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_manager_is_connected());
    cJSON_AddStringToObject(root, "ip", wifi_manager_get_ip());
    const weather_data_t *w = weather_get();
    if (w->valid) {
        cJSON *wj = cJSON_AddObjectToObject(root, "weather");
        cJSON_AddNumberToObject(wj, "temp_c", w->temp_c);
        cJSON_AddNumberToObject(wj, "humidity", w->humidity);
        cJSON_AddStringToObject(wj, "condition", w->condition);
    }
    const sub_count_t *s = youtube_bili_get();
    if (s->valid) cJSON_AddNumberToObject(root, "subscribers", s->subscriber_count);
    cJSON_AddNumberToObject(root, "heap_free", esp_get_free_heap_size());
    cJSON_AddStringToObject(root, "firmware", FW_VERSION_STR);
    /* Read the SPIFFS-side version so the UI can warn when firmware and web
     * UI are from different builds (e.g. after a firmware-only OTA). */
    char spiffs_ver[32] = "unknown";
    FILE *vf = fopen("/spiffs/web/version.txt", "r");
    if (vf) {
        if (fgets(spiffs_ver, sizeof(spiffs_ver), vf))
            spiffs_ver[strcspn(spiffs_ver, "\r\n")] = '\0';
        fclose(vf);
    }
    cJSON_AddStringToObject(root, "spiffs_version", spiffs_ver);
    const nextube_config_t *cfg = config_get();
    const char *modes[] = {"Clock","Countdown","Scoreboard","Pomodoro","YouTube","CustomClock","Album","Weather"};
    int mode_idx = (int)cfg->current_mode;
    if (mode_idx < 0 || mode_idx >= (int)(sizeof(modes)/sizeof(modes[0]))) mode_idx = 0;
    cJSON_AddStringToObject(root, "mode", modes[mode_idx]);
    char *json = cJSON_PrintUnformatted(root);
    esp_err_t ret = send_json(r, json);
    free(json); cJSON_Delete(root);
    return ret;
}

static esp_err_t api_ota(httpd_req_t *r)
{
    const esp_partition_t *upd = esp_ota_get_next_update_partition(NULL);
    if (!upd) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition"), ESP_FAIL;
    esp_ota_handle_t h;
    if (esp_ota_begin(upd, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin fail"), ESP_FAIL;

    char *buf = malloc(4096);
    int rem = r->content_len;
    bool first_chunk = true;

    while (rem > 0) {
        int n = httpd_req_recv(r, buf, rem > 4096 ? 4096 : rem);
        if (n <= 0) { free(buf); esp_ota_abort(h); return ESP_FAIL; }

        /* Validate on the very first chunk: ESP32 app images start with magic
         * byte 0xE9.  The merged full-flash binary (nextube-fw-full.bin) starts
         * with the bootloader at offset 0x1000, not an app header, so its first
         * byte is NOT 0xE9.  Reject it early with a human-readable message. */
        if (first_chunk) {
            first_chunk = false;
            if ((uint8_t)buf[0] != 0xE9) {
                free(buf);
                esp_ota_abort(h);
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
    send_json(r, "{\"status\":\"ok\",\"message\":\"Rebooting...\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ── SPIFFS (web UI) OTA ───────────────────────────────────────────── */
/* Receives a spiffs.bin image and writes it to the SPIFFS partition in
 * 4 KB sectors.  Each sector is erased immediately before it is written
 * so the erase latency is interleaved with the network receive rather
 * than blocking the connection upfront.
 *
 * SPIFFS is unmounted before the first write and the device reboots
 * after a successful flash.  If the upload is interrupted the partition
 * is left partially erased; a retry will always fix this since erasing
 * before writing is idempotent. */
#define SPIFFS_SECTOR 4096
static esp_err_t api_spiffs_ota(httpd_req_t *r)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (!part)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "SPIFFS partition not found"), ESP_FAIL;

    int content_len = r->content_len;
    if (content_len <= 0 || (uint32_t)content_len > part->size)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "Bad content length"), ESP_FAIL;

    char *buf = malloc(SPIFFS_SECTOR);
    if (!buf)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of memory"), ESP_FAIL;

    /* Unmount SPIFFS before touching flash.  The HTTP server itself runs
     * from firmware (app partition), so it stays alive. */
    esp_vfs_spiffs_unregister("spiffs");

    int written = 0;
    while (written < content_len) {
        /* Fill one sector from the network stream */
        int to_recv = content_len - written;
        if (to_recv > SPIFFS_SECTOR) to_recv = SPIFFS_SECTOR;

        /* Pad the buffer with 0xFF (erased flash value) so the final
         * write is always a full sector and satisfies the 4-byte alignment
         * requirement for raw partition writes. */
        memset(buf, 0xFF, SPIFFS_SECTOR);

        int rx = 0;
        while (rx < to_recv) {
            int n = httpd_req_recv(r, buf + rx, to_recv - rx);
            if (n <= 0) { free(buf); return ESP_FAIL; }
            rx += n;
        }

        /* Erase this sector then write it */
        if (esp_partition_erase_range(part, written, SPIFFS_SECTOR) != ESP_OK) {
            free(buf);
            return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Erase failed"), ESP_FAIL;
        }
        if (esp_partition_write(part, written, buf, SPIFFS_SECTOR) != ESP_OK) {
            free(buf);
            return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Write failed"), ESP_FAIL;
        }
        written += rx;
    }
    free(buf);

    ESP_LOGI(TAG, "SPIFFS updated: %d bytes written", written);
    send_json(r, "{\"status\":\"ok\",\"message\":\"SPIFFS updated, rebooting...\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t api_file_ls(httpd_req_t *r)
{
    char path[128] = "/spiffs";
    char q[128];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
        char d[64];
        if (httpd_query_key_value(q, "dir", d, sizeof(d)) == ESP_OK && d[0] != '\0')
            snprintf(path, sizeof(path), "/spiffs%s", d);
    }
    /* Strip trailing slash — ESP-IDF SPIFFS opendir() is sensitive to it.
     * Never strip below "/spiffs" (len=7). */
    int plen = (int)strlen(path);
    while (plen > 7 && path[plen - 1] == '/')
        path[--plen] = '\0';

    cJSON *arr = cJSON_CreateArray();
    DIR *dp = opendir(path);
    if (dp) {
        struct dirent *e;
        while ((e = readdir(dp))) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "name", e->d_name);
            /* Use d_type from readdir — more reliable than stat() for SPIFFS
             * virtual directories where stat() fails with ENOENT. */
            if (e->d_type == DT_DIR) {
                cJSON_AddStringToObject(it, "type", "dir");
            } else {
                cJSON_AddStringToObject(it, "type", "file");
                char fp[400];
                snprintf(fp, sizeof(fp), "%s/%s", path, e->d_name);
                struct stat st;
                if (stat(fp, &st) == 0)
                    cJSON_AddNumberToObject(it, "size", st.st_size);
            }
            cJSON_AddItemToArray(arr, it);
        }
        closedir(dp);
    }
    char *json = cJSON_PrintUnformatted(arr);
    esp_err_t ret = send_json(r, json);
    free(json); cJSON_Delete(arr);
    return ret;
}

/* GET /api/file/download?path=/images/themes/foo/1.jpg
 * Streams the file as a download attachment. */
static esp_err_t api_file_download(httpd_req_t *r)
{
    char q[256], p[256] = {0}, spiffs_path[320];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "path", p, sizeof(p)) != ESP_OK || p[0] == '\0')
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing path"), ESP_FAIL;
    if (strstr(p, ".."))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid path"), ESP_FAIL;

    snprintf(spiffs_path, sizeof(spiffs_path), "/spiffs%s", p);
    FILE *f = fopen(spiffs_path, "rb");
    if (!f) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found"), ESP_FAIL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    httpd_resp_set_type(r, content_type(p));
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");
    const char *fname = strrchr(p, '/'); fname = fname ? fname + 1 : p;
    char disp[280];   /* 23 ("attachment; filename=\"\"") + 255 (max fname) + NUL */
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);
    httpd_resp_set_hdr(r, "Content-Disposition", disp);
    char clen[24]; snprintf(clen, sizeof(clen), "%ld", sz);
    httpd_resp_set_hdr(r, "Content-Length", clen);

    char *buf = malloc(2048);
    if (!buf) { fclose(f); return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL; }
    size_t rd;
    while ((rd = fread(buf, 1, 2048, f)) > 0)
        httpd_resp_send_chunk(r, buf, rd);
    httpd_resp_send_chunk(r, NULL, 0);
    free(buf); fclose(f);
    return ESP_OK;
}

/* POST /api/file/upload?path=/audio/click.wav
 * Writes the raw request body to the given SPIFFS path, creating or
 * overwriting the file.  Directory components must already exist (SPIFFS
 * creates them implicitly via path-prefix emulation). */
static esp_err_t api_file_upload(httpd_req_t *r)
{
    char q[256], p[256] = {0}, spiffs_path[320];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "path", p, sizeof(p)) != ESP_OK || p[0] == '\0')
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing path"), ESP_FAIL;
    if (strstr(p, ".."))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid path"), ESP_FAIL;

    snprintf(spiffs_path, sizeof(spiffs_path), "/spiffs%s", p);
    FILE *f = fopen(spiffs_path, "wb");
    if (!f) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file"), ESP_FAIL;

    char *buf = malloc(2048);
    if (!buf) { fclose(f); return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"), ESP_FAIL; }
    int received = 0, n;
    while ((n = httpd_req_recv(r, buf, 2048)) > 0) {
        fwrite(buf, 1, n, f);
        received += n;
    }
    free(buf); fclose(f);

    if (n < 0) { remove(spiffs_path); return ESP_FAIL; }
    ESP_LOGI(TAG, "Uploaded: %s (%d bytes)", spiffs_path, received);
    return send_json(r, "{\"status\":\"ok\"}");
}

/* DELETE /api/file/delete?path=/audio/click.wav
 * Removes the file.  config.json is protected. */
static esp_err_t api_file_delete(httpd_req_t *r)
{
    char q[256], p[256] = {0}, spiffs_path[320];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "path", p, sizeof(p)) != ESP_OK || p[0] == '\0')
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing path"), ESP_FAIL;
    if (strstr(p, ".."))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid path"), ESP_FAIL;
    if (strcmp(p, "/config.json") == 0)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Protected file"), ESP_FAIL;

    snprintf(spiffs_path, sizeof(spiffs_path), "/spiffs%s", p);
    if (remove(spiffs_path) != 0)
        return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found"), ESP_FAIL;
    ESP_LOGI(TAG, "Deleted: %s", spiffs_path);
    return send_json(r, "{\"status\":\"ok\"}");
}

/* ── Log ring API ──────────────────────────────────────────────────── */
/* GET /api/logs  → {"lines":["I (12) tag: msg", ...]}  chronological  */
static esp_err_t api_get_logs(httpd_req_t *r)
{
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
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        s_log_head  = 0;
        s_log_count = 0;
        xSemaphoreGive(s_log_mutex);
    }
    return send_json(r, "{\"status\":\"ok\"}");
}

static esp_err_t api_wifi_scan_post(httpd_req_t *r)
{
    wifi_manager_scan_start();
    return send_json(r, "{\"status\":\"scanning\"}");
}

static esp_err_t api_wifi_scan_get(httpd_req_t *r)
{
    uint16_t cnt = 0;
    esp_wifi_scan_get_ap_num(&cnt);
    if (cnt > 20) cnt = 20;
    wifi_ap_record_t *list = calloc(cnt, sizeof(wifi_ap_record_t));
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
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Headers", "Content-Type");
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

    FILE *f = fopen(fp, "r");
    if (!f) { f = fopen("/spiffs/web/index.html","r"); }
    if (!f) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found"), ESP_FAIL;

    httpd_resp_set_type(r, content_type(fp));
    httpd_resp_set_hdr(r, "Cache-Control", "max-age=3600");
    char *buf = malloc(1024);
    size_t rd;
    while ((rd = fread(buf, 1, 1024, f)) > 0) httpd_resp_send_chunk(r, buf, rd);
    httpd_resp_send_chunk(r, NULL, 0);
    free(buf); fclose(f);
    return ESP_OK;
}

/* ── URI registration ──────────────────────────────────────────────── */
#define R(m, p, h) { .uri=p, .method=m, .handler=h, .user_ctx=NULL }

static const httpd_uri_t uris[] = {
    R(HTTP_GET,  "/api/ping",            api_ping),
    R(HTTP_GET,  "/api/settings",        api_get_settings),
    R(HTTP_POST, "/api/settings",        api_post_settings),
    R(HTTP_GET,  "/api/firmwareVersion", api_fw_ver),
    R(HTTP_GET,  "/api/hardwareVersion", api_hw_ver),
    R(HTTP_POST, "/api/reset",           api_reset),
    R(HTTP_POST, "/api/reboot",          api_reboot),
    R(HTTP_POST, "/api/audio/play",      api_audio_play),
    R(HTTP_GET,  "/api/status",          api_status),
    R(HTTP_POST, "/api/update_firmware", api_ota),
    R(HTTP_POST, "/api/update_spiffs",   api_spiffs_ota),
    R(HTTP_GET,    "/api/file/ls",         api_file_ls),
    R(HTTP_GET,    "/api/file/download",  api_file_download),
    R(HTTP_POST,   "/api/file/upload",    api_file_upload),
    R(HTTP_DELETE, "/api/file/delete",    api_file_delete),
    R(HTTP_POST,   "/api/wifi/scan",      api_wifi_scan_post),
    R(HTTP_GET,  "/api/wifi/scan",       api_wifi_scan_get),
    R(HTTP_GET,  "/api/logs",            api_get_logs),
    R(HTTP_POST, "/api/logs/clear",      api_clear_logs),
    R(HTTP_OPTIONS, "/api/*",            api_cors),
};

void web_server_start(void)
{
    /* Install the log capture hook as early as possible so boot messages
     * after this point are captured.  All pre-hook messages still appear
     * on UART; only post-hook messages are buffered for the web viewer. */
    s_log_mutex = xSemaphoreCreateMutex();
    esp_log_set_vprintf(log_vprintf_hook);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    for (int i = 0; i < sizeof(uris)/sizeof(uris[0]); i++)
        httpd_register_uri_handler(s_server, &uris[i]);

    /* Wildcard static handler (must be last) */
    httpd_uri_t wildcard = R(HTTP_GET, "/*", serve_static);
    httpd_register_uri_handler(s_server, &wildcard);

    ESP_LOGI(TAG, "HTTP server started on port 80");
}

void web_server_stop(void)
{
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
}
