/**
 * @file update_check.h
 * @brief Autonomous firmware-update availability check.
 *
 * Periodically polls the GitHub releases API directly from the device (no
 * browser tab required) and compares the latest tag against the running
 * firmware version.  On a newer release, drives the existing tube-6 update
 * indicator (display_set_update_indicator()) and is read by ha_mqtt.c each
 * publish cycle to surface a Home Assistant "Nextube Update Available"
 * binary sensor + a diagnostic "Nextube Latest Version" sensor.
 *
 * Respects cfg->update_repo the same way the web UI does, so firmware and
 * browser always check the same repo.
 *
 * Call update_check_start() once from main() when update_check_enabled is
 * true.  Checks once shortly after boot, then every 24 hours.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start the update-check task.  Call once at boot if update_check_enabled is set. */
void update_check_start(void);

/**
 * Returns true if a newer firmware version is available; on true, copies
 * the semver tag (no leading 'v', e.g. "1.18.0") into @p out.  Returns false
 * (and leaves @p out untouched) when up to date or no check has succeeded
 * yet.  Thread-safe.
 */
bool update_check_get_status(char *out, size_t len);

#ifdef __cplusplus
}
#endif
