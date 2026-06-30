#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    float temp_c;
    float humidity;
    char  condition[32];
    char  icon[16];
    bool  valid;
    /* Today's forecast high/low (°C) from the keyless Open-Meteo daily endpoint,
     * fetched independently of the current-conditions source.  day_range_valid
     * stays false until the first successful daily fetch. */
    float day_min_c;
    float day_max_c;
    bool  day_range_valid;
    /* Current wind speed (km/h) from the Open-Meteo current_weather block,
     * fetched alongside the daily range.  0 until first fetch / when calm. */
    float wind_kph;
    /* US EPA Air Quality Index (0-500) + PM2.5 (µg/m³) from the keyless
     * Open-Meteo air-quality endpoint, fetched alongside the daily range using
     * the same geocoded location.  aqi_valid stays false until the first
     * successful fetch (so the panel can show a placeholder meanwhile). */
    int   us_aqi;
    int   european_aqi;
    float pm2_5;
    bool  aqi_valid;
} weather_data_t;
void weather_start(void);
const weather_data_t *weather_get(void);

/** Inject externally-sourced weather data (e.g. POST /api/weather).  Lets a
 *  home-automation system that already averages several providers push the
 *  final result instead of having the firmware fetch a single service.
 *
 *  Partial updates are supported, mirroring the network providers:
 *    - temp_c / humidity : pass NAN to leave the current value unchanged.
 *    - icon              : one of "sun", "fewClouds", "overcastClouds", "fog",
 *                          "rain", "snow", "squalls", "thunderstorm".  Pass
 *                          NULL/"" to derive from wmo_code, or leave unchanged.
 *    - condition         : free text (e.g. "Cloudy").  NULL/"" to derive from
 *                          wmo_code or leave unchanged.
 *    - wmo_code          : WMO weather code used to fill icon/condition when
 *                          those are absent; pass <0 if not supplied.
 *
 *  Only takes lasting effect when weather_source == "external"; otherwise the
 *  next provider poll (≤10 min) overwrites it.  Thread-safe. */
void weather_set_external(float temp_c, float humidity,
                          const char *condition, const char *icon, int wmo_code);

/** Set the location (lat/lon) used by the Sunrise & Sunset panel from an
 *  external push.  Thread-safe. */
void weather_set_external_location(float lat, float lon);
/** Returns true and fills lat/lon once geocoding has succeeded at least once.
 *  Returns false (leaving lat/lon unchanged) until the first successful city
 *  lookup.  Safe to call from any task. */
bool weather_get_location(float *lat, float *lon);

/** Effective air-quality index per the configured standard (config aqi_standard:
 *  "us" / "eu" / "auto"; auto picks the European AQI inside the Europe bounding
 *  box and the US AQI elsewhere).  Returns -1 if no AQI data has been fetched
 *  yet.  *is_european (if non-NULL) is set to the chosen scale so callers can
 *  pick the matching band colours.  Thread-safe. */
int weather_get_aqi(bool *is_european);
#ifdef __cplusplus
}
#endif
