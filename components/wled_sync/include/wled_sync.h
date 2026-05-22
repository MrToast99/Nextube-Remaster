/**
 * @file wled_sync.h
 * @brief WLED UDP Notifier v2 receiver — synchronise accent LEDs with WLED strips.
 *
 * When wled_sync_enabled is true in the config, call wled_sync_start() once at
 * boot (after WiFi is configured but before leds_task_start()).  The listener
 * task binds to UDP port wled_sync_port (default 21324) and updates the shared
 * sync state whenever a valid WLED Notifier v2 packet arrives.
 *
 * The LED task calls wled_sync_get() on every iteration.  When it returns true
 * the caller applies the received colour/brightness and skips normal effects.
 * When it returns false (no packet yet, or sync disabled) the LED task runs its
 * normal config-driven effects unchanged.
 *
 * WLED Notifier v2 packet layout (relevant bytes):
 *   byte  0  : 9          — protocol marker; other values are discarded
 *   bytes 1-3: R, G, B    — primary colour of the current WLED effect
 *   byte  4  : effect ID  — ignored; Nextube always shows the primary colour
 *   byte 11  : brightness — 0 = off, 1-255 = scaling factor
 *
 * The LED task receives pre-scaled RGB values (R × bri / 255) so it can apply
 * them directly without needing the raw brightness separately.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Sync state received from the most-recent WLED UDP Notifier packet. */
typedef struct {
    uint8_t r, g, b;   /* primary colour, pre-scaled by WLED brightness */
    bool    on;         /* false when WLED brightness == 0 (strip is off) */
} wled_sync_state_t;

/**
 * Start the UDP listener task.  Call once from main() when wled_sync_enabled.
 * No-op if called more than once.
 */
void wled_sync_start(void);

/**
 * Get the most-recently-received sync state.
 *
 * Returns true (and fills *out) when wled_sync_start() has been called AND at
 * least one valid WLED packet has been received.
 *
 * Returns false when sync was never started or no packet has arrived yet; in
 * that case *out is left unchanged and the caller should run normal effects.
 */
bool wled_sync_get(wled_sync_state_t *out);

#ifdef __cplusplus
}
#endif
