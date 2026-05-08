/**
 * @file auth.c
 * @brief Admin auth implementation — see auth.h for the public contract.
 *
 * Layout in NVS namespace "nextube_sec":
 *   "admin_set"   u8       0 = no password yet, 1 = password configured
 *   "admin_salt"  blob[16] random per-device, generated on first set_password
 *   "admin_hash"  blob[32] PBKDF2-SHA256(password, salt, 100k iters)
 *
 * The same namespace also holds "ap_pin" written by wifi_manager (S1) — both
 * are bundled under one factory-reset target.
 *
 * Session tokens are kept in s_sessions[] in RAM only; reboot wipes them.
 * Token comparison is constant-time (mbedtls_ct_memcmp) to defeat timing
 * side-channels.
 *
 * Brute-force lockout is global (not per-IP) because httpd_req_t doesn't
 * expose the source address cleanly; for a single-admin home device, global
 * lockout is the right tradeoff.
 */
#include "auth.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"
#include "mbedtls/constant_time.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "auth";

#define NS              "nextube_sec"
#define K_ADMIN_SET     "admin_set"
#define K_ADMIN_SALT    "admin_salt"
#define K_ADMIN_HASH    "admin_hash"

#define PBKDF2_ITER     100000
#define HASH_LEN        32
#define SALT_LEN        16
#define TOKEN_LEN       32
#define TOKEN_HEX_LEN   (TOKEN_LEN * 2)

#define SESSION_SLOTS   4
/* 7-day sliding TTL.  Each authenticated request bumps last_seen_us. */
#define SESSION_TTL_US  (7LL * 24 * 3600 * 1000000LL)

#define LOCKOUT_MAX     5
#define LOCKOUT_US      (60LL * 1000000LL)

/* ── State ──────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  token[TOKEN_LEN];
    int64_t  last_seen_us;
    bool     used;
} session_t;

static session_t        s_sessions[SESSION_SLOTS];
static SemaphoreHandle_t s_lock = NULL;

/* Brute-force counters (touched only under s_lock). */
static int64_t s_lockout_until_us = 0;
static int     s_failed_count     = 0;

/* ── Helpers ────────────────────────────────────────────────────────── */

static inline void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static int pbkdf2_hash(const char *pw, size_t pw_len,
                       const uint8_t *salt, size_t salt_len,
                       uint8_t out[HASH_LEN])
{
    /* mbedtls 3.x: pkcs5_pbkdf2_hmac_ext takes the MD type directly,
     * no setup/free dance.  Returns 0 on success. */
    return mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256,
        (const unsigned char *)pw, pw_len,
        salt, salt_len,
        PBKDF2_ITER, HASH_LEN, out);
}

static void bin_to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i*2]     = hex[(in[i] >> 4) & 0xF];
        out[i*2 + 1] = hex[ in[i]       & 0xF];
    }
    out[n*2] = '\0';
}

static int hex_to_bin(const char *in, uint8_t *out, size_t n)
{
    if (!in || strlen(in) != n*2) return -1;
    for (size_t i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = in[i*2 + k];
            v <<= 4;
            if      (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return -1;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

/* Read salt+hash from NVS into the supplied buffers.  Returns true on
 * success; false if the password is not set or NVS read fails. */
static bool load_salt_hash(uint8_t salt[SALT_LEN], uint8_t hash[HASH_LEN])
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = false;
    uint8_t admin_set = 0;
    if (nvs_get_u8(h, K_ADMIN_SET, &admin_set) != ESP_OK || admin_set == 0)
        goto done;

    size_t s_len = SALT_LEN, h_len = HASH_LEN;
    if (nvs_get_blob(h, K_ADMIN_SALT, salt, &s_len) != ESP_OK) goto done;
    if (nvs_get_blob(h, K_ADMIN_HASH, hash, &h_len) != ESP_OK) goto done;
    if (s_len != SALT_LEN || h_len != HASH_LEN) goto done;
    ok = true;
done:
    nvs_close(h);
    return ok;
}

/* Pick a session slot: prefer an unused one, else the oldest by last_seen.
 * Caller holds s_lock. */
static int pick_slot(void)
{
    int oldest = 0;
    int64_t oldest_age = INT64_MAX;
    for (int i = 0; i < SESSION_SLOTS; i++) {
        if (!s_sessions[i].used) return i;
        if (s_sessions[i].last_seen_us < oldest_age) {
            oldest_age = s_sessions[i].last_seen_us;
            oldest     = i;
        }
    }
    return oldest;
}

/* ── Public API ─────────────────────────────────────────────────────── */

void auth_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    lock();
    memset(s_sessions, 0, sizeof(s_sessions));
    s_lockout_until_us = 0;
    s_failed_count     = 0;
    unlock();
    ESP_LOGI(TAG, "auth subsystem ready (sessions=%d, ttl=%lld s)",
             SESSION_SLOTS, (long long)(SESSION_TTL_US / 1000000));
}

bool auth_is_password_set(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, K_ADMIN_SET, &v);
    nvs_close(h);
    return (err == ESP_OK) && (v == 1);
}

esp_err_t auth_set_password(const char *password)
{
    if (!password) return ESP_ERR_INVALID_ARG;
    size_t pw_len = strlen(password);
    if (pw_len < 6 || pw_len > 64) return ESP_ERR_INVALID_ARG;

    uint8_t salt[SALT_LEN];
    uint8_t hash[HASH_LEN];
    esp_fill_random(salt, sizeof(salt));

    if (pbkdf2_hash(password, pw_len, salt, sizeof(salt), hash) != 0) {
        ESP_LOGE(TAG, "PBKDF2 failed");
        return ESP_FAIL;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(h, K_ADMIN_SALT, salt, sizeof(salt));
    if (err == ESP_OK) err = nvs_set_blob(h, K_ADMIN_HASH, hash, sizeof(hash));
    if (err == ESP_OK) err = nvs_set_u8  (h, K_ADMIN_SET, 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    /* Wipe local copies regardless of outcome. */
    memset(salt, 0, sizeof(salt));
    memset(hash, 0, sizeof(hash));

    if (err == ESP_OK) ESP_LOGI(TAG, "Admin password updated");
    return err;
}

bool auth_verify_password(const char *password)
{
    if (!password) return false;
    uint8_t salt[SALT_LEN];
    uint8_t stored[HASH_LEN];
    if (!load_salt_hash(salt, stored)) return false;

    uint8_t computed[HASH_LEN];
    int rc = pbkdf2_hash(password, strlen(password), salt, sizeof(salt), computed);
    bool ok = (rc == 0) && (mbedtls_ct_memcmp(stored, computed, HASH_LEN) == 0);

    memset(salt,     0, sizeof(salt));
    memset(stored,   0, sizeof(stored));
    memset(computed, 0, sizeof(computed));
    return ok;
}

esp_err_t auth_change_password(const char *old_pw, const char *new_pw)
{
    if (!auth_verify_password(old_pw)) return ESP_ERR_INVALID_STATE;
    return auth_set_password(new_pw);
}

bool auth_is_locked_out(void)
{
    lock();
    bool locked = (esp_timer_get_time() < s_lockout_until_us);
    unlock();
    return locked;
}

int auth_lockout_remaining_s(void)
{
    lock();
    int64_t now = esp_timer_get_time();
    int64_t rem = s_lockout_until_us - now;
    unlock();
    if (rem <= 0) return 0;
    return (int)((rem + 999999) / 1000000);
}

char *auth_login(const char *password)
{
    /* Lockout check first — even with the right password, refuse during
     * lockout so an attacker who eventually guesses correctly still has to
     * wait out the timer. */
    if (auth_is_locked_out()) return NULL;

    bool ok = auth_verify_password(password);

    lock();
    int64_t now = esp_timer_get_time();
    if (!ok) {
        s_failed_count++;
        if (s_failed_count >= LOCKOUT_MAX) {
            s_lockout_until_us = now + LOCKOUT_US;
            s_failed_count     = 0;
            ESP_LOGW(TAG, "Login lockout engaged for %lld s",
                     (long long)(LOCKOUT_US / 1000000));
        }
        unlock();
        return NULL;
    }
    s_failed_count = 0;

    /* Allocate token */
    int slot = pick_slot();
    esp_fill_random(s_sessions[slot].token, TOKEN_LEN);
    s_sessions[slot].last_seen_us = now;
    s_sessions[slot].used         = true;

    char *hex = malloc(TOKEN_HEX_LEN + 1);
    if (!hex) {
        s_sessions[slot].used = false;
        unlock();
        return NULL;
    }
    bin_to_hex(s_sessions[slot].token, TOKEN_LEN, hex);
    unlock();

    ESP_LOGI(TAG, "Login OK (slot=%d)", slot);
    return hex;
}

void auth_logout(const char *token_hex)
{
    if (!token_hex) return;
    uint8_t tok[TOKEN_LEN];
    if (hex_to_bin(token_hex, tok, TOKEN_LEN) != 0) return;

    lock();
    for (int i = 0; i < SESSION_SLOTS; i++) {
        if (!s_sessions[i].used) continue;
        if (mbedtls_ct_memcmp(s_sessions[i].token, tok, TOKEN_LEN) == 0) {
            memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
            ESP_LOGI(TAG, "Logout (slot=%d)", i);
            break;
        }
    }
    unlock();
}

bool auth_check_request(httpd_req_t *r)
{
    /* Pull the Authorization header.  ESP-IDF httpd returns the value-length
     * via httpd_req_get_hdr_value_len(), then we read into a stack buffer. */
    size_t hdr_len = httpd_req_get_hdr_value_len(r, "Authorization");
    if (hdr_len == 0 || hdr_len > 80) return false;
    char hdr[81];
    if (httpd_req_get_hdr_value_str(r, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    if (strncmp(hdr, "Bearer ", 7) != 0) return false;
    const char *hex = hdr + 7;

    uint8_t tok[TOKEN_LEN];
    if (hex_to_bin(hex, tok, TOKEN_LEN) != 0) return false;

    bool ok = false;
    lock();
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < SESSION_SLOTS; i++) {
        if (!s_sessions[i].used) continue;
        if (now - s_sessions[i].last_seen_us > SESSION_TTL_US) {
            /* Expired — clear the slot proactively. */
            memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
            continue;
        }
        if (mbedtls_ct_memcmp(s_sessions[i].token, tok, TOKEN_LEN) == 0) {
            s_sessions[i].last_seen_us = now;   /* sliding TTL */
            ok = true;
            break;
        }
    }
    unlock();
    return ok;
}

void auth_clear_all_sessions(void)
{
    lock();
    memset(s_sessions, 0, sizeof(s_sessions));
    unlock();
    ESP_LOGI(TAG, "All sessions cleared");
}

void auth_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        /* Erase only the auth keys; leave ap_pin alone — the wifi_manager
         * factory_reset entry point clears that.  Both clears together via
         * api_factory_reset_full keep responsibilities separated. */
        nvs_erase_key(h, K_ADMIN_SET);
        nvs_erase_key(h, K_ADMIN_SALT);
        nvs_erase_key(h, K_ADMIN_HASH);
        nvs_commit(h);
        nvs_close(h);
    }
    auth_clear_all_sessions();
    lock();
    s_lockout_until_us = 0;
    s_failed_count     = 0;
    unlock();
    ESP_LOGW(TAG, "Auth factory reset — admin password cleared");
}
