#include <stdbool.h>
#include <time.h>
#include <unistd.h>

time_t time(time_t *out) {
    time_t t = (time_t)wall_time();
    if (out) {
        *out = t;
    }
    return t;
}

/* Days since 1970-01-01 -> year, month, day (Howard Hinnant's civil_from_days). */
struct tm *localtime_r(const time_t *t, struct tm *out) {
    int64_t secs = *t;
    int64_t days = secs / 86400, rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)(rem / 60 % 60);
    out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)((days % 7 + 11) % 7);   /* 1970-01-01 was a Thursday */

    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;

    out->tm_year = (int)(y - 1900);
    out->tm_mon = (int)(m - 1);
    out->tm_mday = (int)d;
    static const int before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    out->tm_yday = before[m - 1] + (int)d - 1 + (leap && m > 2);
    return out;
}
