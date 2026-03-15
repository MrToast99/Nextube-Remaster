#include "web_server.h"
#include "config_mgr.h"
#include "wifi_manager.h"
#include "ntp_time.h"
#include "weather.h"
#include "youtube_bili.h"
#include "display.h"
#include "leds.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "web_srv";
static httpd_handle_t s_server = NULL;

#define FW_VER "1.0.0-oss"
#define HW_VER "1.31"

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t api_ping(httpd_req_t *r)       { return send_json(r, "{\"status\":\"ok\"}"); }
static esp_err_t api_fw_ver(httpd_req_t *r)      { return send_json(r, "{\"version\":\"" FW_VER "\"}"); }
static esp_err_t api_hw_ver(httpd_req_t *r)      { return send_json(r, "{\"version\":\"" HW_VER "\"}"); }

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
    cJSON_AddStringToObject(root, "firmware", FW_VER);
    const nextube_config_t *cfg = config_get();
    const char *modes[] = {"Clock","Countdown","Scoreboard","Pomodoro","YouTube","CustomClock","Album"};
    cJSON_AddStringToObject(root, "mode", modes[cfg->current_mode]);
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

static esp_err_t api_file_ls(httpd_req_t *r)
{
    char path[128] = "/spiffs";
    char q[64];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
        char d[64];
        if (httpd_query_key_value(q, "dir", d, sizeof(d)) == ESP_OK)
            snprintf(path, sizeof(path), "/spiffs%s", d);
    }
    cJSON *arr = cJSON_CreateArray();
    DIR *dp = opendir(path);
    if (dp) {
        struct dirent *e;
        while ((e = readdir(dp))) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "name", e->d_name);
            char fp[400]; snprintf(fp, sizeof(fp), "%s/%s", path, e->d_name);
            struct stat st;
            if (stat(fp, &st)==0) cJSON_AddNumberToObject(it, "size", st.st_size);
            cJSON_AddItemToArray(arr, it);
        }
        closedir(dp);
    }
    char *json = cJSON_PrintUnformatted(arr);
    esp_err_t ret = send_json(r, json);
    free(json); cJSON_Delete(arr);
    return ret;
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
    R(HTTP_GET,  "/api/status",          api_status),
    R(HTTP_POST, "/api/update_firmware", api_ota),
    R(HTTP_GET,  "/api/file/ls",         api_file_ls),
    R(HTTP_POST, "/api/wifi/scan",       api_wifi_scan_post),
    R(HTTP_GET,  "/api/wifi/scan",       api_wifi_scan_get),
    R(HTTP_OPTIONS, "/api/*",            api_cors),
};

void web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
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
