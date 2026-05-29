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
 * WLED Notifier (sync) packet layout (relevant bytes):
 *   byte  0  : 0          — notifier marker; the UDP realtime protocols
 *                           (WARLS=1, DRGB=2, DRGBW=3, DNRGB=4, DDP=5) share
 *                           the port but use 1-5 here and are discarded
 *   byte  2  : brightness — master brightness, 0 = off, 1-255 = scaling factor
 *   bytes 3-5: R, G, B    — primary colour (col[0]) of the current WLED segment
 *   byte  8  : effect ID  — 0 = Solid; non-zero = animation or palette effect
 *   byte 11  : version    — compatibility-version byte (used as a sanity field)
 *
 * Important: palette-based animation effects (Rainbow, Fire, Ocean, Color Cycle,
 * etc.) do not use col[0] for rendering.  WLED leaves col[0] at whatever it was
 * last set to, which is often (0,0,0) if the user never chose an explicit colour
 * for that effect.  The LED task uses the fx field to detect this case and falls
 * back to the local rainbow animation instead of writing black to the strip.
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
    uint8_t r, g, b;   /* primary colour (col[0]), pre-scaled by WLED brightness */
    uint8_t fx;         /* effect index: 0 = Solid, non-zero = animation/palette */
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
