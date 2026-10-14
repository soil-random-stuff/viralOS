#pragma once

/* <string.h> for lwIP: the kernel's own, plus what lwIP needs beyond it. */
#include "lib/string.h"

static inline char *strncpy(char *dest, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = 0;
    }
    return dest;
}
