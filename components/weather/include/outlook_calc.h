#pragma once
/* The rain / snow outlook's pure logic: no FreeRTOS, no ESP-IDF, no globals, so it compiles and is tested on a plain host (test/host).
 * weather.c fills a weather_outlook_t with the hourly numbers and calls outlook_summarize(); the display formats the result with
 * outlook_eta_text() and outlook_fmt_hour(). Everything here is header-only (static inline) on purpose. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "weather.h"

/* An hour counts as wet from 0.2 mm, or from any measurable amount at a 60 % chance or more. */
#define OUTLOOK_WET(mm, prob) ((mm) >= 0.2f || ((mm) > 0.0f && (prob) >= 60))

/* Epoch seconds of an Open-Meteo local timestamp ("2026-10-24T23:00") given the response's utc_offset_seconds; 0 when it does not parse. */
static inline time_t wx_local_iso_to_epoch(const char *iso, int utc_off)
{
    int y, mo, d, h, mi;
    if (!iso || sscanf(iso, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59) return 0;
    y -= (mo <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long days = (long)era * 146097 + (yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
    return (time_t)((int64_t)days * 86400 + h * 3600 + mi * 60 - utc_off);   /* 64-bit: a 32-bit long overflows in 2038 */
}

/* Finds the first wet spell in o->mm / o->prob / o->snow (o->hours slots; slot 0 begins at `base`, an epoch second on an hour boundary)
 * and fills the summary fields. `now` is the current epoch second and `sec_of_day_now` the local time of day at `now`, so the start hour
 * is the one the clock face would show. Leaves o->event false when the whole window is dry. */
static inline void outlook_summarize(weather_outlook_t *o, time_t base, time_t now, long sec_of_day_now)
{
    int first = -1;
    for (int i = 0; i < o->hours; i++) {
        if (OUTLOOK_WET(o->mm[i], o->prob[i])) { first = i; break; }
    }
    if (first < 0) return;
    int last = first;
    while (last + 1 < o->hours && OUTLOOK_WET(o->mm[last + 1], o->prob[last + 1])) last++;
    float peak = 0.0f;
    int chance = 0;
    for (int i = first; i <= last; i++) {
        if (o->mm[i] > peak) peak = o->mm[i];
        if (o->prob[i] > chance) chance = o->prob[i];
    }
    const time_t start = base + (time_t)first * 3600;
    const long mins = (first == 0 || start <= now) ? 0 : (long)((start - now) / 60);
    const long start_s = (sec_of_day_now + (long)(start - now)) % 86400L;
    o->event      = true;
    o->now        = (first == 0);
    o->snow_event = o->snow[first];
    o->start_min  = (int)mins;
    o->start_hour = (int)(((start_s + 86400L) % 86400L) / 3600L);
    o->duration_h = (uint8_t)(last - first + 1);
    o->peak_mm    = peak;
    o->chance_pct = chance;
}

/* "in 45m", "in 3h20" and, from 10 hours on, "in 21h" (the minutes no longer matter). Numbers are masked to a known width so the compiler
 * can prove the longest result fits a 12-byte buffer. */
static inline void outlook_eta_text(char *buf, size_t n, int start_min)
{
    if (start_min < 0) start_min = 0;
    if (start_min < 60)       snprintf(buf, n, "in %um", (unsigned)start_min % 100u);
    else if (start_min < 600) snprintf(buf, n, "in %uh%02u", ((unsigned)start_min / 60u) % 10u, ((unsigned)start_min % 60u) % 100u);
    else                      snprintf(buf, n, "in %uh", ((unsigned)start_min / 60u) % 100u);
}

/* An hour of the day as "9 PM" or "21:00". */
static inline void outlook_fmt_hour(char *buf, size_t n, int hour24, bool h12)
{
    hour24 = ((hour24 % 24) + 24) % 24;
    if (h12) {
        int h = hour24 % 12;
        if (h == 0) h = 12;
        snprintf(buf, n, "%d %s", h, hour24 < 12 ? "AM" : "PM");
    } else {
        snprintf(buf, n, "%02d:00", hour24);
    }
}
