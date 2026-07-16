/**
 * @file sun_calc.h
 * @brief Shared sun/moon astronomical primitives.
 *
 * Extracted from the WeatherLive clock theme's sun/moon sky rendering
 * (components/display/display.c) so other components (the LED accent
 * "Follow Sun/Moon" mode) can reuse the same math without duplicating it or
 * depending on the display task's render cadence.
 */
#pragma once
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NOAA solar sunrise/sunset calculation. Returns local sunrise and sunset,
 * each as minutes past midnight (0-1439), via rise_min and set_min.
 * On polar day/night (sun never crosses the horizon), sets both to -1.
 */
void sun_calc_solar(float lat_deg, float lon_deg, const struct tm *t,
                     int *rise_min, int *set_min);

/**
 * Moon phase as a fraction of the synodic cycle: 0 = new, 0.25 = first
 * quarter, 0.5 = full, 0.75 = last quarter. Date-based low precision
 * (ignores timezone), matching the precision WeatherLive's stylised moon
 * shape needs.
 */
float sun_calc_moon_phase(const struct tm *t);

#ifdef __cplusplus
}
#endif
