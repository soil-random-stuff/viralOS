#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kernel heap: first-fit free list, 16-byte aligned blocks, grows on demand. */
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *ptr);
/* Resize an allocation (may move it). krealloc(NULL, n) == kmalloc(n). */
void *krealloc(void *ptr, size_t size);

struct heap_stats {
    uint64_t mapped_bytes;   /* virtual memory backing the heap */
    uint64_t used_bytes;     /* in allocated blocks, headers included */
    uint64_t free_blocks;
    uint64_t allocations;    /* live allocations */
};
void heap_get_stats(struct heap_stats *out);

/* Allocate/verify/free a pile of blocks; true if the heap behaved. */
bool heap_self_test(void);
