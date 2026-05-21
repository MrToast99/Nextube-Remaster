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
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Start the MQTT client task.  Call once at boot if mqtt_enabled is set. */
void ha_mqtt_start(void);

#ifdef __cplusplus
}
#endif
