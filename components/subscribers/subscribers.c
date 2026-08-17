#include "subscribers.h"
#include "config_mgr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"

static const char *TAG = "subscribers";

/* ── Per-platform state ───────────────────────────────────────────────── */
static sub_count_t       s_sub          = {0};
static SemaphoreHandle_t s_sub_mutex    = NULL;

static sub_count_t       s_insta        = {0};
static SemaphoreHandle_t s_insta_mutex  = NULL;

static sub_count_t       s_tiktok       = {0};
static SemaphoreHandle_t s_tiktok_mutex = NULL;

static sub_count_t       s_mastodon       = {0};
static SemaphoreHandle_t s_mastodon_mutex = NULL;

/* Binary semaphore used to wake the subscribers task early for a forced poll. */
static SemaphoreHandle_t s_refresh_sem = NULL;

/* Shared receive buffer — used only by YouTube and Bilibili fetches.
 * Instagram (internal API), TikTok (Research API) and Mastodon use a
 * separate shared heap buffer (see get_shared_rx_buf() below) to avoid
 * growing this BSS region (internal SRAM is shared with the weather TLS
 * stack); the relay paths for YouTube/Instagram/TikTok use their own small
 * stack buffer instead, since relay responses are compact JSON. */
static char s_http_buf[2048];
static int  s_http_buf_len = 0;

/* ── HTTP event handler (shared buffer) ──────────────────────────────── */
static esp_err_t http_event(esp_http_client_event_t *evt)
{
    /* Do NOT gate on !is_chunked_response: the HTTP client strips chunk
     * framing before invoking this callback, so both chunked and non-chunked
     * payloads arrive identically in evt->data.  The guard was silently
     * discarding all YouTube Data API v3 response data because Google's API
     * uses Transfer-Encoding: chunked, leaving the buffer empty and causing
     * cJSON_Parse to fail → subscriber count stuck at zero. (Issue #49) */
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (s_http_buf_len + copy >= (int)sizeof(s_http_buf))
            copy = sizeof(s_http_buf) - s_http_buf_len - 1;
        if (copy > 0) {
            memcpy(s_http_buf + s_http_buf_len, evt->data, copy);
            s_http_buf_len += copy;
        }
    }
    return ESP_OK;
}

/* ── HTTP event handler context (heap buffer, for Instagram / TikTok) ── */
typedef struct {
    char *buf;
    int   buf_size;
    int   buf_len;
} heap_rx_t;

static esp_err_t http_event_heap(esp_http_client_event_t *evt)
{
    heap_rx_t *ctx = (heap_rx_t *)evt->user_data;
    if (!ctx) return ESP_OK;
    /* Do NOT gate on !is_chunked_response: the HTTP client strips chunk
     * framing before invoking this callback, so both chunked and non-chunked
     * payloads arrive identically in evt->data.  Filtering on chunked was
     * silently discarding all Instagram/TikTok response data because those
     * servers use Transfer-Encoding: chunked. */
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (ctx->buf_len + copy >= ctx->buf_size)
            copy = ctx->buf_size - ctx->buf_len - 1;
        if (copy > 0) {
            memcpy(ctx->buf + ctx->buf_len, evt->data, copy);
            ctx->buf_len += copy;
        }
    }
    return ESP_OK;
}

/* Percent-encode a value for embedding in a URL query/path segment (RFC 3986
 * unreserved set kept literal; everything else — including '&', '?', '"',
 * spaces — escaped as %XX).  Small duplicate of weather.c's
 * url_encode_query() (that one is file-static there too, so this copy avoids
 * a larger cross-component refactor).  Used to safely embed usernames /
 * instance names sourced from user-editable config into request URLs. */
static void url_encode_query(const char *in, char *out, size_t out_sz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < out_sz; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
}

/* Shared heap RX buffer for the Instagram-internal, TikTok-API, and Mastodon
 * fetches — sized to the largest single need (Instagram's 16 KB).  All three
 * run serially from the single subscribers_task poll loop (see
 * subscribers_task below) and are never concurrent, so one lazily-allocated
 * static buffer avoids repeatedly malloc/free-ing a multi-KB block every
 * poll cycle (at least every 5 minutes, for the device's entire uptime) —
 * not a leak, just long-uptime heap-fragmentation risk.  No mutex needed:
 * single-task, serial use only. */
#define SHARED_RX_BUF_SIZE 16384
static char *s_shared_rx_buf = NULL;

static char *get_shared_rx_buf(void)
{
    if (!s_shared_rx_buf) {
        s_shared_rx_buf = heap_caps_malloc(SHARED_RX_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_shared_rx_buf) s_shared_rx_buf = malloc(SHARED_RX_BUF_SIZE);
    }
    return s_shared_rx_buf;
}

/* ── YouTube ──────────────────────────────────────────────────────────── */
/* Two fetch paths:
 *   1. Direct YouTube Data API v3  — when youtube_key is configured.
 *   2. Local relay (social_relay.py) — when only relay_host is set; the
 *      relay scrapes the channel page with a browser UA and returns
 *      {"subscribers": N} over plain HTTP (no TLS, same relay as TikTok).
 * If neither is available the function logs a hint and returns.            */
static void fetch_youtube(void)
{
    char youtube_id[48], youtube_key[48], relay_host[64];
    config_lock();
    const nextube_config_t *cfg = config_get();
    strncpy(youtube_id,  cfg->youtube_id,       sizeof(youtube_id)  - 1); youtube_id[sizeof(youtube_id)   - 1] = '\0';
    strncpy(youtube_key, cfg->youtube_key,       sizeof(youtube_key) - 1); youtube_key[sizeof(youtube_key) - 1] = '\0';
    strncpy(relay_host,  cfg->tiktok_relay_host, sizeof(relay_host)  - 1); relay_host[sizeof(relay_host)   - 1] = '\0';
    config_unlock();
    if (youtube_id[0] == '\0') return;

    if (youtube_key[0] != '\0') {
        /* ── Path 1: YouTube Data API v3 ───────────────────────────────── */
        char url[512];
        snprintf(url, sizeof(url),
            "https://www.googleapis.com/youtube/v3/channels?part=statistics&id=%s&key=%s",
            youtube_id, youtube_key);

        s_http_buf_len = 0;
        esp_http_client_config_t http_cfg = {
            .url = url, .event_handler = http_event, .timeout_ms = 10000,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        tls_sem_take();
        /* init returns NULL on heap exhaustion — perform/get_status would
         * deref it (same guard at every fetch site in this file). */
        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        esp_err_t err = client ? esp_http_client_perform(client) : ESP_ERR_NO_MEM;
        int status = client ? esp_http_client_get_status_code(client) : 0;
        esp_http_client_cleanup(client);   /* NULL-safe */
        tls_sem_give();

        if (err == ESP_OK && status == 200) {
            s_http_buf[s_http_buf_len] = 0;
            cJSON *root = cJSON_Parse(s_http_buf);
            if (root) {
                cJSON *items = cJSON_GetObjectItem(root, "items");
                if (cJSON_IsArray(items) && cJSON_GetArraySize(items) > 0) {
                    cJSON *stats = cJSON_GetObjectItem(cJSON_GetArrayItem(items, 0), "statistics");
                    cJSON *sc = cJSON_GetObjectItem(stats, "subscriberCount");
                    if (sc && sc->valuestring) {
                        uint32_t count = (uint32_t)atoi(sc->valuestring);
                        xSemaphoreTake(s_sub_mutex, portMAX_DELAY);
                        s_sub.subscriber_count = count;
                        s_sub.valid = true;
                        xSemaphoreGive(s_sub_mutex);
                        ESP_LOGI(TAG, "YouTube -> %lu subscribers (API)", (unsigned long)count);
                    }
                }
                cJSON_Delete(root);
            }
        } else {
            ESP_LOGW(TAG, "YouTube fetch failed: err=%d status=%d", err, status);
        }

    } else if (relay_host[0] != '\0') {
        /* ── Path 2: local relay (no API key required) ──────────────────
         * social_relay.py exposes /youtube?channel=<id> and returns
         * {"subscribers": N}.  Plain HTTP — no TLS, no semaphore.       */
        char relay_buf[128] = {0};
        heap_rx_t ctx = { .buf = relay_buf, .buf_size = sizeof(relay_buf), .buf_len = 0 };

        char url[192];
        snprintf(url, sizeof(url), "http://%s:8888/youtube?channel=%s", relay_host, youtube_id);

        esp_http_client_config_t http_cfg = {
            .url           = url,
            .event_handler = http_event_heap,
            .user_data     = &ctx,
            .timeout_ms    = 45000,   /* Playwright fetch can take up to ~30 s on cold start */
        };
        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        esp_err_t err    = client ? esp_http_client_perform(client) : ESP_ERR_NO_MEM;
        int       status = client ? esp_http_client_get_status_code(client) : 0;
        esp_http_client_cleanup(client);   /* NULL-safe */

        if (err == ESP_OK && status == 200) {
            relay_buf[ctx.buf_len] = '\0';
            const char *needle = "\"subscribers\":";
            const char *p = strstr(relay_buf, needle);
            if (p) {
                p += strlen(needle);
                uint32_t count = (uint32_t)strtoul(p, NULL, 10);
                xSemaphoreTake(s_sub_mutex, portMAX_DELAY);
                s_sub.subscriber_count = count;
                s_sub.valid = true;
                xSemaphoreGive(s_sub_mutex);
                ESP_LOGI(TAG, "YouTube -> %lu subscribers (via relay)", (unsigned long)count);
            } else {
                ESP_LOGW(TAG, "YouTube relay: unexpected response: %s", relay_buf);
            }
        } else {
            ESP_LOGW(TAG, "YouTube relay fetch failed: err=%d status=%d url=%s", err, status, url);
        }
        /* relay_buf is stack-allocated — nothing to free */

    } else {
        ESP_LOGW(TAG, "YouTube: no API key and no relay host configured — skipping. "
                      "Either enter an API key in Settings, or run helpers/social_relay.py "
                      "and enter its IP under Settings -> TikTok -> Relay host.");
    }
}

/* ── Bilibili ─────────────────────────────────────────────────────────── */
static void fetch_bilibili(void)
{
    char bili_uid[24];
    config_lock();
    strncpy(bili_uid, config_get()->bili_uid, sizeof(bili_uid) - 1);
    bili_uid[sizeof(bili_uid) - 1] = '\0';
    config_unlock();
    if (bili_uid[0] == '\0') return;

    char url[256];
    snprintf(url, sizeof(url),
        "https://api.bilibili.com/x/web-interface/card?mid=%s", bili_uid);

    s_http_buf_len = 0;
    esp_http_client_config_t http_cfg = {
        .url = url, .event_handler = http_event, .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    tls_sem_take();
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_err_t err = client ? esp_http_client_perform(client) : ESP_ERR_NO_MEM;
    int status = client ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);   /* NULL-safe */
    tls_sem_give();

    if (err == ESP_OK && status == 200) {
        s_http_buf[s_http_buf_len] = 0;
        cJSON *root = cJSON_Parse(s_http_buf);
        if (root) {
            cJSON *data = cJSON_GetObjectItem(root, "data");
            cJSON *card = cJSON_GetObjectItem(data, "card");
            cJSON *fans = cJSON_GetObjectItem(card, "fans");
            if (cJSON_IsNumber(fans)) {   /* a format change to string must not read garbage */
                xSemaphoreTake(s_sub_mutex, portMAX_DELAY);
                s_sub.subscriber_count = (uint32_t)fans->valueint;
                s_sub.valid = true;
                xSemaphoreGive(s_sub_mutex);
                ESP_LOGI(TAG, "Bilibili -> %lu fans", (unsigned long)s_sub.subscriber_count);
            }
            cJSON_Delete(root);
        }
    } else {
        ESP_LOGW(TAG, "Bilibili fetch failed: err=%d status=%d", err, status);
    }
}

/* ── Instagram ────────────────────────────────────────────────────────────
 * Method is selected by instagram_method config field:
 *   "relay"    — GET http://<tiktok_relay_host>:8888/instagram?user=<u>
 *                Plain HTTP, no TLS, no bot-check. Requires social_relay.py.
 *   "internal" — GET https://i.instagram.com/api/v1/users/web_profile_info/
 *                Direct Instagram API; may require valid headers to avoid 401. */
static void fetch_instagram(void)
{
    char user[48], method[16], relay_host[64];
    config_lock();
    strncpy(user,       config_get()->instagram_user,    sizeof(user)       - 1);
    strncpy(method,     config_get()->instagram_method,  sizeof(method)     - 1);
    strncpy(relay_host, config_get()->tiktok_relay_host, sizeof(relay_host) - 1);
    config_unlock();
    user[sizeof(user) - 1]             = '\0';
    method[sizeof(method) - 1]         = '\0';
    relay_host[sizeof(relay_host) - 1] = '\0';

    if (user[0] == '\0') {
        ESP_LOGI(TAG, "Instagram: no username configured — skipping");
        return;
    }

    if (strcmp(method, "relay") == 0) {
        /* ── Relay path ──────────────────────────────────────────────────── */
        if (relay_host[0] == '\0') {
            ESP_LOGW(TAG, "Instagram: method=relay but no relay host configured — skipping");
            return;
        }
        char relay_buf[128] = {0};
        heap_rx_t ctx = { .buf = relay_buf, .buf_size = sizeof(relay_buf), .buf_len = 0 };

        char enc_user[144];
        url_encode_query(user, enc_user, sizeof(enc_user));
        char url[256];
        snprintf(url, sizeof(url), "http://%s:8888/instagram?user=%s", relay_host, enc_user);

        esp_http_client_config_t http_cfg = {
            .url = url, .event_handler = http_event_heap,
            .user_data = &ctx, .timeout_ms = 15000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        esp_err_t err = client ? esp_http_client_perform(client) : ESP_ERR_NO_MEM;
        int status    = client ? esp_http_client_get_status_code(client) : 0;
        esp_http_client_cleanup(client);   /* NULL-safe */

        if (err == ESP_OK && status == 200) {
            relay_buf[ctx.buf_len] = '\0';
            const char *p = strstr(relay_buf, "\"followers\":");
            if (p) {
                p += strlen("\"followers\":");
                uint32_t count = (uint32_t)strtoul(p, NULL, 10);
                xSemaphoreTake(s_insta_mutex, portMAX_DELAY);
                s_insta.subscriber_count = count;
                s_insta.valid = true;
                xSemaphoreGive(s_insta_mutex);
                ESP_LOGI(TAG, "Instagram @%s -> %lu followers (relay)", user, (unsigned long)count);
            } else {
                ESP_LOGW(TAG, "Instagram relay: unexpected response: %s", relay_buf);
            }
        } else {
            ESP_LOGW(TAG, "Instagram relay failed: err=%d status=%d", err, status);
        }

    } else {
        /* ── Internal API path (default) ─────────────────────────────────── */
        /* Receive buffer — Instagram responses run 50–100 KB; we only need
         * the first ~16 KB, since "edge_followed_by":{"count":N} always
         * appears before the bulky media-edge arrays.  Shared/reused buffer,
         * see get_shared_rx_buf(). */
        heap_rx_t ctx = {
            .buf      = get_shared_rx_buf(),
            .buf_size = SHARED_RX_BUF_SIZE,
            .buf_len  = 0,
        };
        if (!ctx.buf) { ESP_LOGW(TAG, "Instagram: no memory for RX buffer"); return; }

        char enc_user[144];
        url_encode_query(user, enc_user, sizeof(enc_user));
        char url[320];
        snprintf(url, sizeof(url),
            "https://i.instagram.com/api/v1/users/web_profile_info/?username=%s", enc_user);

        esp_http_client_config_t http_cfg = {
            .url = url, .event_handler = http_event_heap, .user_data = &ctx,
            .timeout_ms = 10000, .crt_bundle_attach = esp_crt_bundle_attach,
        };
        tls_sem_take();
        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        if (client) {
            esp_http_client_set_header(client, "x-ig-app-id", "936619743392459");
            esp_http_client_set_header(client, "User-Agent",
                "Instagram 219.0.0.12.117 Android (28/9; 420dpi; 1080x2148; samsung; SM-G977B)");
        }
        esp_err_t err = client ? esp_http_client_perform(client) : ESP_ERR_NO_MEM;
        int status = client ? esp_http_client_get_status_code(client) : 0;
        esp_http_client_cleanup(client);   /* NULL-safe */
        tls_sem_give();

        if (err == ESP_OK && status == 200) {
            ctx.buf[ctx.buf_len] = '\0';
            const char *needle = "\"edge_followed_by\":{\"count\":";
            const char *p = strstr(ctx.buf, needle);
            if (p) {
                p += strlen(needle);
                uint32_t count = (uint32_t)strtoul(p, NULL, 10);
                xSemaphoreTake(s_insta_mutex, portMAX_DELAY);
                s_insta.subscriber_count = count;
                s_insta.valid = true;
                xSemaphoreGive(s_insta_mutex);
                ESP_LOGI(TAG, "Instagram @%s -> %lu followers", user, (unsigned long)count);
            } else {
                ESP_LOGW(TAG, "Instagram: 'edge_followed_by' not found in %d bytes "
                              "(truncated=%s) — prefix: %.80s",
                         ctx.buf_len,
                         ctx.buf_len >= ctx.buf_size - 1 ? "yes" : "no",
                         ctx.buf);
            }
        } else {
            ESP_LOGW(TAG, "Instagram fetch failed: err=%d status=%d", err, status);
        }
        /* ctx.buf is the shared buffer — not freed here, see get_shared_rx_buf(). */
    }
}

/* ── TikTok ───────────────────────────────────────────────────────────── */
/* Two fetch paths:
 *   1. TikTok Research API — when tiktok_key (bearer token) is configured.
 *      POST https://open.tiktokapis.com/v2/research/user/info/?fields=follower_count
 *      Authorization: Bearer {token}
 *      Body: {"username": "<user>"}
 *      Response: {"data":{"follower_count":N},"error":{"code":"ok"}}
 *   2. Local relay (social_relay.py) — when only relay_host is set; the
 *      relay fetches the profile page with a real browser UA and returns
 *      {"followers": N} over plain HTTP (no TLS, no cert issues).
 * Apply for Research API access at developers.tiktok.com.              */
static void fetch_tiktok(void)
{
    char user[48], tiktok_key[64], relay_host[64];
    config_lock();
    strncpy(user,       config_get()->tiktok_user,       sizeof(user)       - 1); user[sizeof(user)             - 1] = '\0';
    strncpy(tiktok_key, config_get()->tiktok_key,        sizeof(tiktok_key) - 1); tiktok_key[sizeof(tiktok_key) - 1] = '\0';
    strncpy(relay_host, config_get()->tiktok_relay_host, sizeof(relay_host) - 1); relay_host[sizeof(relay_host) - 1] = '\0';
    config_unlock();

    if (user[0] == '\0') {
        ESP_LOGI(TAG, "TikTok: no username configured — skipping");
        return;
    }

    if (tiktok_key[0] != '\0') {
        /* ── Path 1: TikTok Research API ───────────────────────────────── */
        heap_rx_t ctx = {
            .buf      = get_shared_rx_buf(),
            .buf_size = SHARED_RX_BUF_SIZE,
            .buf_len  = 0,
        };
        if (!ctx.buf) { ESP_LOGW(TAG, "TikTok: no memory for RX buffer"); return; }

        /* Build JSON body: {"username":"<user>"} via cJSON rather than hand
         * string-interpolation — user comes straight from config, and a
         * '"' or '\' in it would otherwise corrupt/inject into the POST
         * body sent to TikTok's API. */
        cJSON *body_json = cJSON_CreateObject();
        cJSON_AddStringToObject(body_json, "username", user);
        char *body = cJSON_PrintUnformatted(body_json);
        cJSON_Delete(body_json);
        if (!body) {
            ESP_LOGW(TAG, "TikTok: failed to build JSON POST body");
            return;
        }

        esp_http_client_config_t http_cfg = {
            .url               = "https://open.tiktokapis.com/v2/research/user/info/?fields=follower_count",
            .event_handler     = http_event_heap,
            .user_data         = &ctx,
            .timeout_ms        = 10000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .method            = HTTP_METHOD_POST,
        };
        tls_sem_take();
        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        if (client) {
            char auth[80];
            snprintf(auth, sizeof(auth), "Bearer %s", tiktok_key);
            esp_http_client_set_header(client, "Authorization", auth);
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, body, strlen(body));
        }
        esp_err_t err = client ? esp_http_client_perform(client) : ESP_ERR_NO_MEM;
        int status    = client ? esp_http_client_get_status_code(client) : 0;
        esp_http_client_cleanup(client);   /* NULL-safe */
        tls_sem_give();
        free(body);

        if (err == ESP_OK && status == 200) {
            ctx.buf[ctx.buf_len] = '\0';
            const char *needle = "\"follower_count\":";
            const char *p = strstr(ctx.buf, needle);
            if (p) {
                p += strlen(needle);
                uint32_t count = (uint32_t)strtoul(p, NULL, 10);
                xSemaphoreTake(s_tiktok_mutex, portMAX_DELAY);
                s_tiktok.subscriber_count = count;
                s_tiktok.valid = true;
                xSemaphoreGive(s_tiktok_mutex);
                ESP_LOGI(TAG, "TikTok @%s -> %lu followers (API)", user, (unsigned long)count);
            } else {
                ESP_LOGW(TAG, "TikTok API: follower_count not found — response: %.120s", ctx.buf);
            }
        } else {
            ESP_LOGW(TAG, "TikTok API fetch failed: err=%d status=%d", err, status);
        }
        /* ctx.buf is the shared buffer — not freed here, see get_shared_rx_buf(). */

    } else if (relay_host[0] != '\0') {
        /* ── Path 2: local relay ────────────────────────────────────────── */
        char relay_buf[128] = {0};
        heap_rx_t ctx = { .buf = relay_buf, .buf_size = sizeof(relay_buf), .buf_len = 0 };

        char enc_user[144];
        url_encode_query(user, enc_user, sizeof(enc_user));
        char url[256];
        snprintf(url, sizeof(url), "http://%s:8888/tiktok?user=%s", relay_host, enc_user);

        esp_http_client_config_t http_cfg = {
            .url = url, .event_handler = http_event_heap,
            .user_data = &ctx, .timeout_ms = 10000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        esp_err_t err = client ? esp_http_client_perform(client) : ESP_ERR_NO_MEM;
        int status    = client ? esp_http_client_get_status_code(client) : 0;
        esp_http_client_cleanup(client);   /* NULL-safe */

        if (err == ESP_OK && status == 200) {
            relay_buf[ctx.buf_len] = '\0';
            const char *p = strstr(relay_buf, "\"followers\":");
            if (p) {
                p += strlen("\"followers\":");
                uint32_t count = (uint32_t)strtoul(p, NULL, 10);
                xSemaphoreTake(s_tiktok_mutex, portMAX_DELAY);
                s_tiktok.subscriber_count = count;
                s_tiktok.valid = true;
                xSemaphoreGive(s_tiktok_mutex);
                ESP_LOGI(TAG, "TikTok @%s -> %lu followers (relay)", user, (unsigned long)count);
            } else {
                ESP_LOGW(TAG, "TikTok relay: unexpected response: %s", relay_buf);
            }
        } else {
            ESP_LOGW(TAG, "TikTok relay fetch failed: err=%d status=%d url=%s", err, status, url);
        }

    } else {
        ESP_LOGW(TAG, "TikTok: no API key and no relay host configured — skipping. "
                      "Either enter a Research API token in Settings, or run "
                      "helpers/social_relay.py and enter its IP under Settings → TikTok → Relay host.");
    }
}

/* ── Mastodon ─────────────────────────────────────────────────────────── */
/* Uses the official public Accounts API — no authentication required for
 * public accounts.
 * GET https://{instance}/api/v1/accounts/lookup?acct={user}
 * Returns compact JSON (~1–2 KB) with "followers_count":N at the top level. */
static void fetch_mastodon(void)
{
    char user[48];
    char instance[64];
    config_lock();
    strncpy(user,     config_get()->mastodon_user,     sizeof(user)     - 1);
    strncpy(instance, config_get()->mastodon_instance, sizeof(instance) - 1);
    user[sizeof(user) - 1]         = '\0';
    instance[sizeof(instance) - 1] = '\0';
    config_unlock();

    if (user[0] == '\0' || instance[0] == '\0') {
        ESP_LOGI(TAG, "Mastodon: username or instance not configured — skipping");
        return;
    }

    heap_rx_t ctx = {
        .buf      = get_shared_rx_buf(),
        .buf_size = SHARED_RX_BUF_SIZE,
        .buf_len  = 0,
    };
    if (!ctx.buf) { ESP_LOGW(TAG, "Mastodon: no memory for RX buffer"); return; }

    char enc_user[144], enc_instance[192];
    url_encode_query(user,     enc_user,     sizeof(enc_user));
    url_encode_query(instance, enc_instance, sizeof(enc_instance));
    char url[400];
    snprintf(url, sizeof(url),
             "https://%s/api/v1/accounts/lookup?acct=%s", enc_instance, enc_user);

    esp_http_client_config_t http_cfg = {
        .url = url, .event_handler = http_event_heap, .user_data = &ctx,
        .timeout_ms = 10000, .crt_bundle_attach = esp_crt_bundle_attach,
    };
    tls_sem_take();
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_err_t err    = client ? esp_http_client_perform(client) : ESP_ERR_NO_MEM;
    int       status = client ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);   /* NULL-safe */
    tls_sem_give();

    if (err == ESP_OK && status == 200) {
        ctx.buf[ctx.buf_len] = '\0';
        const char *needle = "\"followers_count\":";
        const char *p = strstr(ctx.buf, needle);
        if (p) {
            p += strlen(needle);
            uint32_t count = (uint32_t)strtoul(p, NULL, 10);
            xSemaphoreTake(s_mastodon_mutex, portMAX_DELAY);
            s_mastodon.subscriber_count = count;
            s_mastodon.valid = true;
            xSemaphoreGive(s_mastodon_mutex);
            ESP_LOGI(TAG, "Mastodon @%s@%s -> %lu followers", user, instance, (unsigned long)count);
        } else {
            ESP_LOGW(TAG, "Mastodon: 'followers_count' not found (len=%d): %.120s",
                     ctx.buf_len, ctx.buf);
        }
    } else {
        ESP_LOGW(TAG, "Mastodon fetch failed: err=%d status=%d", err, status);
    }
    /* ctx.buf is the shared buffer — not freed here, see get_shared_rx_buf(). */
}

/* ── Main task — polls all configured platforms at the configured interval ── */
static void subscribers_task(void *arg)
{
    /* 20 s head-start so NTP, weather, and mDNS can finish their own boot
     * sequences first.  Concurrent TLS is safe (tls_sem serialises access),
     * so this is just a courtesy yield — not a hard dependency. */
    vTaskDelay(pdMS_TO_TICKS(20000));
    while (1) {
        ESP_LOGI(TAG, "Social counter poll cycle starting");

        /* YouTube / Bilibili — mutually exclusive via video_site config */
        {
            bool youtube_enabled;
            char video_site[16], bili_uid[24], youtube_id[48];
            config_lock();
            const nextube_config_t *cfg = config_get();
            youtube_enabled = cfg->youtube_enabled;
            strncpy(video_site, cfg->video_site, sizeof(video_site) - 1); video_site[sizeof(video_site) - 1] = '\0';
            strncpy(bili_uid,   cfg->bili_uid,   sizeof(bili_uid)   - 1); bili_uid[sizeof(bili_uid)     - 1] = '\0';
            strncpy(youtube_id, cfg->youtube_id, sizeof(youtube_id) - 1); youtube_id[sizeof(youtube_id) - 1] = '\0';
            config_unlock();

            if (!youtube_enabled) {
                ESP_LOGI(TAG, "YouTube/Bilibili disabled in config — skipping");
            } else {
                bool is_bili    = (strcmp(video_site, "bilibili") == 0);
                /* For YouTube, only the channel ID is required here; fetch_youtube()
                 * decides whether to use the API key or the relay (and logs a hint
                 * if neither is configured).  For Bilibili, only the UID is needed. */
                bool configured = is_bili ? (bili_uid[0] != '\0') : (youtube_id[0] != '\0');
                if (configured) {
                    if (is_bili) fetch_bilibili();
                    else         fetch_youtube();
                } else {
                    ESP_LOGI(TAG, "%s enabled but ID/UID not set — skipping",
                             is_bili ? "Bilibili" : "YouTube");
                }
            }
        }

        /* Instagram */
        {
            bool insta_enabled;
            config_lock();
            insta_enabled = config_get()->instagram_enabled;
            config_unlock();
            if (!insta_enabled) {
                ESP_LOGI(TAG, "Instagram disabled in config — skipping");
            } else {
                fetch_instagram();
            }
        }

        /* TikTok */
        {
            bool tiktok_enabled;
            config_lock();
            tiktok_enabled = config_get()->tiktok_enabled;
            config_unlock();
            if (!tiktok_enabled) {
                ESP_LOGI(TAG, "TikTok disabled in config — skipping");
            } else {
                fetch_tiktok();
            }
        }

        /* Mastodon */
        {
            bool mastodon_enabled;
            config_lock();
            mastodon_enabled = config_get()->mastodon_enabled;
            config_unlock();
            if (!mastodon_enabled) {
                ESP_LOGI(TAG, "Mastodon disabled in config — skipping");
            } else {
                fetch_mastodon();
            }
        }

        config_lock();
        uint16_t interval_min = config_get()->sub_poll_interval_min;
        config_unlock();
        if (interval_min < 5) interval_min = 5;   /* floor: avoid hammering APIs */
        ESP_LOGI(TAG, "Social counter poll cycle done — sleeping %u min", (unsigned)interval_min);
        /* Sleep in 1-minute slices.  A semaphore give from subscribers_refresh_now()
         * breaks out of the loop early so a forced poll starts immediately. */
        for (uint16_t _m = 0; _m < interval_min; _m++) {
            if (s_refresh_sem &&
                xSemaphoreTake(s_refresh_sem, pdMS_TO_TICKS(60000)) == pdTRUE) {
                ESP_LOGI(TAG, "Force-refresh requested — restarting poll cycle");
                break;   /* exit the sleep loop and start the next poll immediately */
            }
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */
void subscribers_start(void)
{
    s_sub_mutex      = xSemaphoreCreateMutex();
    s_insta_mutex    = xSemaphoreCreateMutex();
    s_tiktok_mutex   = xSemaphoreCreateMutex();
    s_mastodon_mutex = xSemaphoreCreateMutex();
    s_refresh_sem    = xSemaphoreCreateBinary();
    if (xTaskCreate(subscribers_task, "subscribers", 8192, NULL, 3, NULL) != pdPASS)
        ESP_LOGE(TAG, "subscribers_task creation failed");
}

/** Wake the subscribers task immediately to start a fresh poll cycle. */
void subscribers_refresh_now(void)
{
    if (s_refresh_sem) xSemaphoreGive(s_refresh_sem);
}

const sub_count_t *subscribers_get(void)
{
    static sub_count_t copy;
    if (s_sub_mutex) {
        xSemaphoreTake(s_sub_mutex, portMAX_DELAY);
        copy = s_sub;
        xSemaphoreGive(s_sub_mutex);
    } else {
        copy = s_sub;
    }
    return &copy;
}

const sub_count_t *instagram_get(void)
{
    static sub_count_t copy;
    if (s_insta_mutex) {
        xSemaphoreTake(s_insta_mutex, portMAX_DELAY);
        copy = s_insta;
        xSemaphoreGive(s_insta_mutex);
    } else {
        copy = s_insta;
    }
    return &copy;
}

const sub_count_t *tiktok_get(void)
{
    static sub_count_t copy;
    if (s_tiktok_mutex) {
        xSemaphoreTake(s_tiktok_mutex, portMAX_DELAY);
        copy = s_tiktok;
        xSemaphoreGive(s_tiktok_mutex);
    } else {
        copy = s_tiktok;
    }
    return &copy;
}

const sub_count_t *mastodon_get(void)
{
    static sub_count_t copy;
    if (s_mastodon_mutex) {
        xSemaphoreTake(s_mastodon_mutex, portMAX_DELAY);
        copy = s_mastodon;
        xSemaphoreGive(s_mastodon_mutex);
    } else {
        copy = s_mastodon;
    }
    return &copy;
}
