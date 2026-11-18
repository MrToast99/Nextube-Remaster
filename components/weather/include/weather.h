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
    /* Station panel data. Pressure comes from a push or from Open-Meteo's pressure_msl; direction and gust likewise; rain-today only from a push
     * (a forecast total would not be "so far"). All default to -1 = not available. pressure_trend: +1 rising, -1 falling, 0 steady or not
     * enough history yet (worked out over the last 3 h of samples, or taken from the push). */
    float  pressure_hpa;
    float  wind_dir_deg;     /* direction the wind blows FROM, 0-359 */
    float  wind_gust_kph;
    float  rain_today_mm;
    int8_t pressure_trend;
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
/** Thread-safe copy for any task other than the display task (weather_get() hands out one shared buffer, which a second task would
 *  overwrite while the first is still reading it). */
void weather_get_copy(weather_data_t *out);

/* ── Energy (Home Assistant energy data pushed to POST /api/energy) ──────────────────────────────────────────────────────────────────
 * Every source is optional. A source that was never pushed (or was pushed as null) is absent and its zone is left out of the panel; one that
 * was pushed and then went stale reads as stale (shown as "--"). Power is in watts: grid > 0 imports, grid < 0 exports; battery > 0 charges. */
typedef struct {
    bool  has_grid, has_solar, has_home, has_batt;       /* ever pushed and not cleared */
    bool  stale_grid, stale_solar, stale_home, stale_batt; /* pushed, but older than max_age_min */
    float grid_w, solar_w, home_w;
    float batt_pct, batt_w;
    bool  has_batt_w;
    bool  has_tot[4], stale_tot[4];                       /* today's kWh: 0 solar produced, 1 grid import, 2 grid export, 3 home used */
    float tot_kwh[4];
    uint32_t seq;                                         /* bumped on every push, so the display can tell when to redraw */
} energy_data_t;

/** Values: NAN = not in this push (kept as is); clear_* = the push said null (the source is absent). max_age_min < 0 = unchanged, 0 = never stale. */
typedef struct {
    float grid_w, solar_w, home_w, batt_pct, batt_w;
    float tot_kwh[4];  bool clear_tot[4];    /* today's totals; NAN = not in this push */
    bool  clear_grid, clear_solar, clear_home, clear_batt, clear_batt_w;   /* clear_batt_w: battery stays, its power reading is gone */
    int   max_age_min;
} energy_push_t;
void energy_push_init(energy_push_t *p);
void energy_set(const energy_push_t *p);
void energy_get_copy(energy_data_t *out);

/** The JSON of POST /api/weather and POST /api/energy, parsed and applied. Shared by the HTTP routes and the MQTT input (nextube/<host>/weather/set and
 *  energy/set). An optional "ts" (epoch seconds) older than 10 minutes makes the message ignored: a retained MQTT message must not look fresh after
 *  a reboot. Returns false for bad JSON or a message that is too old. */
bool weather_set_json(const char *json);
bool energy_set_json(const char *json);

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

/** Optional extras of an external push: everything the Open-Meteo path fills in, so a push can drive every panel. Start from
 *  weather_ext_push_init() (marks all fields absent), set what the push carries, then call weather_set_external_extras() after
 *  weather_set_external(). While a pushed field is fresh the Open-Meteo gap-fill does not overwrite it. */
typedef struct {
    float   wind_kph;                /* NAN = absent */
    float   precip_mm;               /* rain rate, mm/h; NAN = absent */
    float   day_min_c, day_max_c;    /* both NAN = absent */
    float   cloud_cover_pct;         /* 0-100; NAN = absent */
    int     us_aqi, european_aqi;    /* < 0 = absent */
    float   pm2_5;                   /* NAN = absent */
    float   pressure_hpa;            /* NAN = absent */
    int     pressure_trend;          /* 2 = absent (worked out on the clock), else -1 / 0 / +1 */
    float   wind_dir_deg;            /* NAN = absent */
    float   wind_gust_kph;           /* NAN = absent */
    float   rain_today_mm;           /* NAN = absent */
    int     max_age_min;             /* < 0 = absent; 0 = readings never go stale */
    uint8_t outlook_hours;           /* 0 = absent; else slots filled below (slot 0 = the hour of the push) */
    float   outlook_mm[24];
    float   outlook_snow_cm[24];
    uint8_t outlook_prob[24];
} weather_ext_push_t;

void weather_ext_push_init(weather_ext_push_t *x);

/** Applies the extras. max_age_min is the number of minutes after the last push at which the reading counts as stale: weather_get() then
 *  reports valid = false and the AQI and outlook accessors report nothing. Held in RAM only. */
void weather_set_external_extras(const weather_ext_push_t *x);

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
