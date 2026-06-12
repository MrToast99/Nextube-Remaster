/**
 * @file ha_mqtt.h
 * @brief Home Assistant MQTT integration.
 *
 * Exposes the SHT30 temperature/humidity sensor as HA entities and allows
 * the display mode, backlight, and brightness to be read and set from HA.
 * Uses HA MQTT auto-discovery so no manual HA configuration is required.
 *
 * Call ha_mqtt_start() once from main() when mqtt_enabled is true.
 * The task waits internally for WiFi before connecting.
 *
 * Ticker:
 *   Publish any UTF-8 string to  nextube/<host>/ticker/set  to display a
 *   scrolling marquee across all 6 tubes.  Publish an empty payload to cancel.
 *   The display task polls ha_mqtt_ticker_active() each tick and calls
 *   ha_mqtt_ticker_clear() once the scroll completes.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start the MQTT client task.  Call once at boot if mqtt_enabled is set. */
void ha_mqtt_start(void);

/**
 * Returns true and copies the active ticker message into @p out when a
 * ticker is pending.  Returns false when idle (no active ticker).
 * Thread-safe — protected by an internal mutex.
 */
bool ha_mqtt_ticker_active(char *out, size_t len);

/**
 * Clear the active ticker.  Called by the display task when the scroll
 * animation finishes, and by the MQTT handler when an empty payload arrives.
 * Publishes "" to nextube/<host>/ticker/state so HA sees the cleared state.
 * Thread-safe.
 */
void ha_mqtt_ticker_clear(void);

/**
 * Publish a touch-button press ("left" / "middle" / "right") to
 * nextube/<host>/button/state — surfaced in HA as device triggers for
 * automations.  No-ops unless MQTT is connected AND the optional
 * "button events" publishing group (cfg->mqtt_pub_buttons) is enabled.
 * Safe to call from the touch handler at any time.
 */
void ha_mqtt_publish_button(const char *btn);

#ifdef __cplusplus
}
#endif
