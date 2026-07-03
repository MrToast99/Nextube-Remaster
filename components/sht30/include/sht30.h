#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float temp_c;     /* Temperature in °C  */
    float humidity;   /* Relative humidity % */
    bool  valid;      /* true once first successful read completes */
} sht30_reading_t;

/* Probe for the sensor and register it on the shared I²C bus.
 * Must be called after pcf8563_init() (which creates the bus).
 * Returns true if an SHT30 was found and initialised. */
bool sht30_init(void);

/* True if the sensor was detected at startup. */
bool sht30_is_present(void);

/* Trigger a single high-repeatability measurement synchronously.
 * Blocks for ~20 ms.  Returns false on I²C error or CRC mismatch. */
bool sht30_read(sht30_reading_t *out);

/* Start a background FreeRTOS task that reads the sensor every 30 s.
 * Call after sht30_init(); safe to call even when sensor is absent
 * (task exits immediately). */
void sht30_task_start(void);

/* Copy the last reading from the background task into *out (non-blocking,
 * thread-safe — each caller gets a private copy; the display task, ha_mqtt,
 * and web_server all call this concurrently).  Returns out->valid, which is
 * false until the first successful read completes.  The configured
 * temperature offset (see sht30_set_offset) is applied before returning —
 * callers always receive the corrected value. */
bool sht30_get(sht30_reading_t *out);

/* Set a fixed offset (°C) added to every temperature reading returned by
 * sht30_get().  Use a negative value to correct for ESP32 self-heating.
 * Safe to call at any time; takes effect on the next sht30_get() call.
 * Default 0.  Clamped to ±20 °C. */
void sht30_set_offset(float offset_c);

#ifdef __cplusplus
}
#endif
