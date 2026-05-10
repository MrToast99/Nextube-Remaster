/**
 * @file auth.h
 * @brief Admin authentication for the web server (S5).
 *
 * Stores a PBKDF2-SHA256 password hash + random salt in the NVS namespace
 * "nextube_sec".  Issues random 32-byte session tokens kept in a small
 * RAM-only table; tokens are sent by the browser as
 *   Authorization: Bearer <hex>
 * and validated by auth_check_request() on every protected handler call.
 *
 * On reboot all sessions are lost (intentional — RAM-only avoids flash wear);
 * the user re-logs in once.  The password itself persists in NVS across
 * reboots and across normal firmware re-flashes (NVS at offset 0x9000 is
 * outside the merged-image regions).  Only an explicit erase_flash or the
 * "full factory reset" UI action wipes it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise the in-RAM session table.  Safe to call once at boot, after
 *  nvs_flash_init().  Does NOT generate or read any password material. */
void auth_init(void);

/** True if the admin password has ever been set on this device. */
bool auth_is_password_set(void);

/** Set the admin password.  Caller must enforce that this is only allowed
 *  while !auth_is_password_set() (first-boot flow), or have already verified
 *  the existing password via auth_change_password().
 *  Returns ESP_OK on success, ESP_ERR_INVALID_ARG if password is too short
 *  (< 6) or too long (> 64), or NVS errors on write failure. */
esp_err_t auth_set_password(const char *password);

/** Change the admin password.  Verifies old_pw matches the stored hash before
 *  writing the new one.  Returns ESP_ERR_INVALID_STATE if old_pw is wrong. */
esp_err_t auth_change_password(const char *old_pw, const char *new_pw);

/** Verify a candidate password against the stored hash (constant-time
 *  comparison).  Returns false if no password is set. */
bool auth_verify_password(const char *password);

/** Attempt a login.  On success, allocates a 64-char hex session token
 *  (caller must free()).  On failure (wrong password) or while in lockout,
 *  returns NULL.  Five wrong attempts in succession trigger a 60 s global
 *  lockout. */
char *auth_login(const char *password);

/** Invalidate a session by hex token.  Idempotent. */
void auth_logout(const char *token_hex);

/** Validate the Authorization: Bearer <hex> header on a request.
 *  Bumps the matched session's last-seen timestamp on success.
 *  Returns true if the request carries a valid, non-expired session. */
bool auth_check_request(httpd_req_t *r);

/** Clear all in-RAM sessions (logs everyone out).  Password remains. */
void auth_clear_all_sessions(void);

/** Wipe admin_set / admin_salt / admin_hash from NVS and clear all sessions.
 *  Used by the "Full factory reset" UI action.  After this call the device
 *  behaves as fresh-from-box: next browser session must set a new password. */
void auth_factory_reset(void);

/** True if authentication is currently required for mutation endpoints.
 *  Defaults to false (disabled) so existing devices are never locked out
 *  on firmware upgrade.  Persisted in NVS key "auth_en". */
bool auth_is_enabled(void);

/** Enable or disable the authentication requirement.
 *  Callers must enforce the permission check themselves:
 *  — disabling while auth is on  → call REQUIRE_AUTH before this
 *  — enabling while auth is off  → no existing token to check
 *  Takes effect immediately in RAM; survives reboots via NVS. */
esp_err_t auth_set_enabled(bool enabled);

/** True while the lockout timer is active.  Used by the login route to
 *  return 429 instead of 401 when the user keeps trying. */
bool auth_is_locked_out(void);

/** Seconds remaining in the current lockout, or 0 if not locked out. */
int auth_lockout_remaining_s(void);

#ifdef __cplusplus
}
#endif
