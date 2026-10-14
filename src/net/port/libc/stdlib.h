#pragma once

/* <stdlib.h> for lwIP (netdb.c): only atoi. */
static inline int atoi(const char *s) {
    int sign = 1, v = 0;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
    }
    return sign * v;
}
