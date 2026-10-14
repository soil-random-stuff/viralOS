/*
 * Freestanding memory/string routines. GCC may emit calls to memcpy, memmove,
 * memset and memcmp on its own, so these must exist even if unused directly.
 * `rep movsb` / `rep stosb` are fast on modern CPUs (ERMS) and keep this short.
 */

#include <stdint.h>

#include "lib/string.h"

void *memcpy(void *dest, const void *src, size_t n) {
    void *d = dest;
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = dest;
    const uint8_t *s = src;
    if (d <= s || d >= s + n) { /* forward copy is safe */
        return memcpy(dest, src, n);
    }
    d += n;
    s += n;
    while (n--) {
        *--d = *--s;
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    void *d = s;
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return s;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++) {
        if (*p != *q) {
            return *p < *q ? -1 : 1;
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a) {
            return (unsigned char)*a - (unsigned char)*b;
        }
    }
    return 0;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    do {
        if (*s == (char)c) {
            last = s;
        }
    } while (*s++);
    return (char *)last;
}

size_t strlcpy(char *dest, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size > 0) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dest, src, n);
        dest[n] = '\0';
    }
    return len;
}
