#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
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
    /* Cloud cover (%) and precipitation rate (mm) from the same Open-Meteo
     * block as wind_kph — used by WeatherLive's sky to scale cloud
     * density/opacity and rain/snow particle count continuously instead of
     * fixed per-icon values. Defaults to -1, not 0 like wind_kph: 0 would
     * render a clear sky for the first ~10 minutes even when the icon
     * already reports overcast/rain — worse than a few minutes of "no
     * wind". */
    float cloud_cover_pct;
    float precip_mm;
    /* US EPA Air Quality Index (0-500) + PM2.5 (µg/m³) from the keyless
     * Open-Meteo air-quality endpoint, fetched alongside the daily range using
     * the same geocoded location.  aqi_valid stays false until the first
     * successful fetch (so the panel can show a placeholder meanwhile). */
    int   us_aqi;
    int   european_aqi;
    float pm2_5;
    bool  aqi_valid;
    /* Next-24-hour precipitation outlook: the hourly arrays of the same Open-Meteo request as the daily range. Slot 0 is the hour that was
     * current when the data was fetched (outlook_slot0 = its start, epoch seconds); outlook_hours is 0 until the first successful fetch.
     * Read it through weather_get_outlook(), which lines the slots up with "now". */
    uint8_t outlook_hours;
    float   outlook_mm[24];        /* precipitation per hour (mm, rain plus the water in snow) */
    float   outlook_snow_cm[24];   /* snowfall per hour (cm) */
    uint8_t outlook_prob[24];      /* precipitation probability, % */
    time_t  outlook_slot0;
} weather_data_t;

/** The precipitation outlook lined up with the current hour (slot 0 = this hour), plus a summary of the first wet spell in it. An hour counts
 *  as wet from 0.2 mm, or from any measurable amount at a 60 % chance or more. */
typedef struct {
    bool    valid;          /* outlook data is present and not stale */
    bool    event;          /* a wet hour is expected within the window */
    bool    now;            /* it is already wet this hour */
    bool    snow_event;     /* the first wet hour has snowfall */
    uint8_t hours;          /* number of slots filled in mm[] / prob[] / snow[] */
    uint8_t duration_h;     /* consecutive wet hours of the first spell */
    int     start_min;      /* minutes until the first wet hour starts (0 when it is already wet) */
    int     start_hour;     /* local clock hour (0-23) the first wet hour starts */
    int     chance_pct;     /* highest probability during the spell */
    float   peak_mm;        /* heaviest hour of the spell */
    float   mm[24];
    uint8_t prob[24];
    bool    snow[24];
} weather_outlook_t;

/** Fills *o from the latest Open-Meteo hourly data. Returns false (and o->valid = false) before the first successful fetch, or when the data
 *  is older than its 24 hours. Thread-safe; keep *o off a small stack (about 170 bytes). */
bool weather_get_outlook(weather_outlook_t *o);
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
 *  pick the matching band colors.  Thread-safe. */
int weather_get_aqi(bool *is_european);
#ifdef __cplusplus
}
#endif
