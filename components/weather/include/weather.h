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
} weather_data_t;
void weather_start(void);
const weather_data_t *weather_get(void);
/** Returns true and fills lat/lon once geocoding has succeeded at least once.
 *  Returns false (leaving lat/lon unchanged) until the first successful city
 *  lookup.  Safe to call from any task. */
bool weather_get_location(float *lat, float *lon);
#ifdef __cplusplus
}
#endif
