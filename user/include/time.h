#pragma once

/*
 * Calendar time. The clock keeps local time (it's set from the PC's
 * real-time clock, which holds local time), so localtime_r just splits the
 * seconds into fields; there are no time zones.
 */

#include <stdint.h>

typedef int64_t time_t;

struct tm {
    int tm_sec;    /* 0-59 */
    int tm_min;    /* 0-59 */
    int tm_hour;   /* 0-23 */
    int tm_mday;   /* 1-31 */
    int tm_mon;    /* 0-11 */
    int tm_year;   /* years since 1900 */
    int tm_wday;   /* 0 = Sunday */
    int tm_yday;   /* 0-365 */
};

time_t time(time_t *out);
struct tm *localtime_r(const time_t *t, struct tm *out);
