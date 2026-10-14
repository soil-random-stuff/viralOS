#pragma once

/* <stdlib.h> for lwext4: memory from the kernel heap, and qsort (htree folders sort their entries). */
#include <stddef.h>

#include "mm/heap.h"

static inline void *malloc(size_t size) {
    return kmalloc(size);
}

static inline void *calloc(size_t count, size_t size) {
    return kzalloc(count * size);
}

static inline void *realloc(void *ptr, size_t size) {
    return krealloc(ptr, size);
}

static inline void free(void *ptr) {
    kfree(ptr);
}

void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *));
