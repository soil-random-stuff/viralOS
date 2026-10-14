#pragma once

#include <stdint.h>

/* For SO_RCVTIMEO / SO_SNDTIMEO (the same layout as struct os_timeval). */
struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};
