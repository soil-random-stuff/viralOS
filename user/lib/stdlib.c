#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

long strtol(const char *s, char **end, int base) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    int negative = 0;
    if (*s == '-' || *s == '+') {
        negative = *s++ == '-';
    }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = 10;
    }
    long v = 0;
    for (;; s++) {
        int digit;
        if (*s >= '0' && *s <= '9') {
            digit = *s - '0';
        } else if (*s >= 'a' && *s <= 'z') {
            digit = *s - 'a' + 10;
        } else if (*s >= 'A' && *s <= 'Z') {
            digit = *s - 'A' + 10;
        } else {
            break;
        }
        if (digit >= base) {
            break;
        }
        v = v * base + digit;
    }
    if (end) {
        *end = (char *)s;
    }
    return negative ? -v : v;
}

int atoi(const char *s) {
    return (int)strtol(s, NULL, 10);
}

int abs(int x) {
    return x < 0 ? -x : x;
}

static void swap_bytes(unsigned char *a, unsigned char *b, size_t size) {
    while (size--) {
        unsigned char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

/* Push item i down the heap of the first n items until both children are smaller. */
static void sift_down(unsigned char *base, size_t i, size_t n, size_t size,
                      int (*compare)(const void *, const void *)) {
    for (;;) {
        size_t largest = i, left = 2 * i + 1, right = left + 1;
        if (left < n && compare(base + left * size, base + largest * size) > 0) {
            largest = left;
        }
        if (right < n && compare(base + right * size, base + largest * size) > 0) {
            largest = right;
        }
        if (largest == i) {
            return;
        }
        swap_bytes(base + i * size, base + largest * size, size);
        i = largest;
    }
}

void qsort(void *array, size_t count, size_t size, int (*compare)(const void *, const void *)) {
    unsigned char *base = array;
    if (count < 2 || size == 0) {
        return;
    }
    for (size_t i = count / 2; i-- > 0;) {
        sift_down(base, i, count, size, compare);
    }
    for (size_t end = count - 1; end > 0; end--) {
        swap_bytes(base, base + end * size, size);
        sift_down(base, 0, end, size, compare);
    }
}

void __assert_fail(const char *expr, const char *file, int line) {
    dprintf(STDERR_FILENO, "%s:%d: assertion failed: %s\n", file, line, expr);
    exit(134);
}
