/*
 * Every block, free or used, starts with a 16-byte header. Free blocks also
 * hold a `next` pointer and sit on a list sorted by address, which makes
 * merging with neighbours on kfree() a matter of checking adjacency.
 *
 * The heap only ever grows; pages are not given back to the PMM yet.
 */

#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "arch/x86_64/cpu.h"
#include "lib/panic.h"
#include "lib/string.h"

#define ALIGNMENT      16
#define MAGIC_USED     0xA110CA7Eu
#define MAGIC_FREE     0xF4EEB10Cu
#define MIN_GROWTH     (64 * 1024)

struct block {
    uint64_t size;   /* whole block, header included; multiple of ALIGNMENT */
    uint32_t magic;
    uint32_t reserved;
};

struct free_block {
    struct block header;
    struct free_block *next;
};

#define HEADER_SIZE sizeof(struct block)
#define MIN_BLOCK   ALIGN_UP(sizeof(struct free_block), ALIGNMENT)

_Static_assert(sizeof(struct block) == ALIGNMENT, "header must keep payloads aligned");

static struct free_block *free_list;
static uint64_t heap_end = KERNEL_HEAP_BASE;
static uint64_t used_bytes;
static uint64_t live_allocations;

static bool adjacent(struct free_block *a, struct free_block *b) {
    return (char *)a + a->header.size == (char *)b;
}

/* Insert into the address-sorted free list, merging with neighbours. */
static void insert_free(struct free_block *b) {
    b->header.magic = MAGIC_FREE;

    struct free_block *prev = NULL, *next = free_list;
    while (next && next < b) {
        prev = next;
        next = next->next;
    }

    b->next = next;
    if (next && adjacent(b, next)) {
        b->header.size += next->header.size;
        b->next = next->next;
    }
    if (prev && adjacent(prev, b)) {
        prev->header.size += b->header.size;
        prev->next = b->next;
    } else if (prev) {
        prev->next = b;
    } else {
        free_list = b;
    }
}

static bool grow(uint64_t min_bytes) {
    uint64_t bytes = ALIGN_UP(min_bytes > MIN_GROWTH ? min_bytes : MIN_GROWTH, PAGE_SIZE);
    if (heap_end + bytes > KERNEL_HEAP_BASE + KERNEL_HEAP_MAX) {
        return false;
    }
    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys || !vmm_map(heap_end + off, phys, VMM_WRITE)) {
            return false; /* the pages mapped so far stay mapped, but unused */
        }
    }
    struct free_block *b = (struct free_block *)heap_end;
    b->header.size = bytes;
    heap_end += bytes;
    insert_free(b);
    return true;
}

static void *kmalloc_locked(size_t size);

/* The heap is shared by all threads: every entry point runs with interrupts off. */
void *kmalloc(size_t size) {
    uint64_t irq = irq_save();
    void *p = kmalloc_locked(size);
    irq_restore(irq);
    return p;
}

static void *kmalloc_locked(size_t size) {
    if (size == 0 || size > KERNEL_HEAP_MAX) {
        return NULL;
    }
    uint64_t need = ALIGN_UP(size + HEADER_SIZE, ALIGNMENT);
    if (need < MIN_BLOCK) {
        need = MIN_BLOCK;
    }

    for (;;) {
        struct free_block **link = &free_list;
        for (struct free_block *b = free_list; b; link = &b->next, b = b->next) {
            if (b->header.size < need) {
                continue;
            }
            if (b->header.size - need >= MIN_BLOCK) { /* split, keep the tail free */
                struct free_block *rest = (struct free_block *)((char *)b + need);
                rest->header.size = b->header.size - need;
                rest->header.magic = MAGIC_FREE;
                rest->next = b->next;
                *link = rest;
                b->header.size = need;
            } else {
                *link = b->next;
            }
            b->header.magic = MAGIC_USED;
            used_bytes += b->header.size;
            live_allocations++;
            return (char *)b + HEADER_SIZE;
        }
        if (!grow(need)) {
            return NULL;
        }
    }
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

void kfree(void *ptr) {
    if (!ptr) {
        return;
    }
    struct block *b = (struct block *)((char *)ptr - HEADER_SIZE);
    if ((uint64_t)b < KERNEL_HEAP_BASE || (uint64_t)b >= heap_end) {
        panic("kfree: %p is not a heap pointer", ptr);
    }
    uint64_t irq = irq_save();
    if (b->magic == MAGIC_FREE) {
        panic("kfree: double free of %p", ptr);
    }
    if (b->magic != MAGIC_USED) {
        panic("kfree: heap corruption at %p (bad block header)", ptr);
    }
    used_bytes -= b->size;
    live_allocations--;
    insert_free((struct free_block *)b);
    irq_restore(irq);
}

void *krealloc(void *ptr, size_t size) {
    if (!ptr) {
        return kmalloc(size);
    }
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    struct block *b = (struct block *)((char *)ptr - HEADER_SIZE);
    if (b->magic != MAGIC_USED) {
        panic("krealloc: %p is not a live heap allocation", ptr);
    }
    uint64_t capacity = b->size - HEADER_SIZE;
    if (size <= capacity) {
        return ptr;
    }
    void *bigger = kmalloc(size);
    if (bigger) {
        memcpy(bigger, ptr, capacity);
        kfree(ptr);
    }
    return bigger;
}

void heap_get_stats(struct heap_stats *out) {
    uint64_t irq = irq_save();
    out->mapped_bytes = heap_end - KERNEL_HEAP_BASE;
    out->used_bytes = used_bytes;
    out->allocations = live_allocations;
    out->free_blocks = 0;
    for (struct free_block *b = free_list; b; b = b->next) {
        out->free_blocks++;
    }
    irq_restore(irq);
}

bool heap_self_test(void) {
    enum { COUNT = 64 };
    uint8_t *blocks[COUNT];
    size_t sizes[COUNT];
    uint32_t seed = 12345;

    struct heap_stats before;
    heap_get_stats(&before);

    for (int i = 0; i < COUNT; i++) {
        seed = seed * 1103515245 + 12345;
        sizes[i] = 1 + (seed >> 16) % 3000;
        blocks[i] = kmalloc(sizes[i]);
        if (!blocks[i] || (uint64_t)blocks[i] % ALIGNMENT != 0) {
            return false;
        }
        memset(blocks[i], i, sizes[i]);
    }

    /* Free every other block, then check the survivors weren't overwritten. */
    for (int i = 0; i < COUNT; i += 2) {
        kfree(blocks[i]);
    }
    for (int i = 1; i < COUNT; i += 2) {
        for (size_t j = 0; j < sizes[i]; j++) {
            if (blocks[i][j] != (uint8_t)i) {
                return false;
            }
        }
    }

    /* A large allocation forces the heap to grow. */
    uint8_t *big = kmalloc(1024 * 1024);
    if (!big) {
        return false;
    }
    big[0] = big[1024 * 1024 - 1] = 0xAB;
    kfree(big);

    for (int i = 1; i < COUNT; i += 2) {
        kfree(blocks[i]);
    }

    /* Everything freed: usage is back where it was and neighbours merged again. */
    struct heap_stats after;
    heap_get_stats(&after);
    return after.used_bytes == before.used_bytes &&
           after.allocations == before.allocations &&
           after.free_blocks <= before.free_blocks + 1;
}
