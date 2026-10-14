/* qsort for lwext4 (sorting a folder's entries by hash when an htree folder grows): a heap sort,
   which needs no extra memory and no recursion on the kernel stack. */

#include <stddef.h>
#include <stdint.h>

#include "port/libc/stdlib.h"

static void swap(uint8_t *a, uint8_t *b, size_t size) {
    while (size--) {
        uint8_t t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void sift_down(uint8_t *base, size_t start, size_t end, size_t size, int (*cmp)(const void *, const void *)) {
    size_t root = start;
    while (root * 2 + 1 <= end) {
        size_t child = root * 2 + 1, top = root;
        if (cmp(base + top * size, base + child * size) < 0) {
            top = child;
        }
        if (child + 1 <= end && cmp(base + top * size, base + (child + 1) * size) < 0) {
            top = child + 1;
        }
        if (top == root) {
            return;
        }
        swap(base + root * size, base + top * size, size);
        root = top;
    }
}

void qsort(void *base, size_t count, size_t size, int (*cmp)(const void *, const void *)) {
    if (count < 2) {
        return;
    }
    uint8_t *b = base;
    for (size_t start = (count - 2) / 2 + 1; start-- > 0;) {
        sift_down(b, start, count - 1, size, cmp);
    }
    for (size_t end = count - 1; end > 0; end--) {
        swap(b, b + end * size, size);
        sift_down(b, 0, end - 1, size, cmp);
    }
}
