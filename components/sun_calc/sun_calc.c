#include "sun_calc.h"
#include <math.h>
#include <time.h>

void sun_calc_solar(float lat_deg, float lon_deg, const struct tm *t,
                     int *rise_min, int *set_min)
{
    int Y = t->tm_year + 1900, M = t->tm_mon + 1, D = t->tm_mday;
    /* Gregorian-to-JDN: months Jan/Feb treated as 13/14 of the previous year. */
    if (M <= 2) { Y--; M += 12; }
    int A = Y / 100;
    double JD = (int)(365.25*(Y+4716)) + (int)(30.6001*(M+1)) + D
              + (2 - A + A/4) - 1524.5;
    double JC  = (JD - 2451545.0) / 36525.0;
    double L0  = fmod(280.46646 + JC*(36000.76983 + JC*0.0003032), 360.0);
    double M0  = 357.52911 + JC*(35999.05029 - 0.0001537*JC);
    double Mr  = M0 * M_PI / 180.0;
    double e   = 0.016708634 - JC*(0.000042037 + 0.0000001267*JC);
    double C   = sin(Mr)*(1.914602 - JC*(0.004817 + 0.000014*JC))
               + sin(2*Mr)*(0.019993 - 0.000101*JC) + sin(3*Mr)*0.000289;
    double om  = 125.04 - 1934.136*JC;
    double lam = (L0 + C) - 0.00569 - 0.00478*sin(om*M_PI/180.0);
    double eps = (23.0 + (26.0 + (21.448 - JC*(46.815 + JC*(0.00059 - JC*0.001813)))/60.0)/60.0)
               + 0.00256*cos(om*M_PI/180.0);
    double decl = asin(sin(eps*M_PI/180.0)*sin(lam*M_PI/180.0));
    double yy   = tan((eps/2.0)*M_PI/180.0); yy *= yy;
    double L0r  = L0*M_PI/180.0, M0r = M0*M_PI/180.0;
    double eot  = 4.0*180.0/M_PI*(yy*sin(2*L0r) - 2*e*sin(M0r)
                + 4*e*yy*sin(M0r)*cos(2*L0r) - 0.5*yy*yy*sin(4*L0r)
                - 1.25*e*e*sin(2*M0r));
    double latr  = lat_deg * M_PI / 180.0;
    double cosHA = cos(90.833*M_PI/180.0) / (cos(latr)*cos(decl))
                 - tan(latr)*tan(decl);
    if (cosHA < -1.0 || cosHA > 1.0) { *rise_min = -1; *set_min = -1; return; }
    double HA   = acos(cosHA) * 180.0 / M_PI;
    double noon = 720.0 - 4.0*lon_deg - eot;
    /* UTC offset in minutes (east = positive), computed portably without
     * tm_gmtoff (GNU/BSD) or the 'timezone' global (not exported by ESP-IDF
     * newlib).  We compare localtime and gmtime for the same instant:
     *   tz_m = (local_hour*60 + local_min) - (utc_hour*60 + utc_min)
     *          + day_diff * 1440
     * tm_yday avoids month-boundary issues; multiplying by tm_year*365
     * handles the single edge case of a UTC offset spanning a year end.    */
    {
        time_t ts = time(NULL);
        struct tm ltm, utm;
        localtime_r(&ts, &ltm);
        gmtime_r(&ts, &utm);
        int day_diff = (ltm.tm_yday + ltm.tm_year * 365)
                     - (utm.tm_yday + utm.tm_year * 365);
        double tz_m = (double)(day_diff * 1440
                               + (ltm.tm_hour - utm.tm_hour) * 60
                               + (ltm.tm_min  - utm.tm_min));
        *rise_min = (int)(noon - HA*4.0 + tz_m + 0.5);
        *set_min  = (int)(noon + HA*4.0 + tz_m + 0.5);
    }
}

float sun_calc_moon_phase(const struct tm *t)
{
    int Y = t->tm_year + 1900, M = t->tm_mon + 1, D = t->tm_mday;
    if (M <= 2) { Y -= 1; M += 12; }
    int A = Y / 100, B = 2 - A + A / 4;             /* Gregorian correction */
    double jd = (double)(int)(365.25 * (Y + 4716))
              + (double)(int)(30.6001 * (M + 1))
              + D + B - 1524.5
              + (t->tm_hour - 12) / 24.0 + t->tm_min / 1440.0;
    double ph = fmod((jd - 2451550.1) / 29.530588853, 1.0);  /* since a known new moon */
    if (ph < 0) ph += 1.0;
    return (float)ph;
}
