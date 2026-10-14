#pragma once

#include <stddef.h>
#include <stdint.h>

struct limine_memmap_response;

/* A copy of one Limine memory map entry (LIMINE_MEMMAP_* type). */
struct mem_region {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

/* Physical page frame allocator: one bit per 4 KiB frame. */
void pmm_init(struct limine_memmap_response *memmap);

/* Returns a physical address, or 0 if out of memory. Contents are not zeroed. */
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t phys);
/* `count` physically contiguous frames below `limit` (e.g. 4 GiB for 32-bit DMA), lowest first; 0 if none.
   Free them one by one with pmm_free_page. Not zeroed. */
uint64_t pmm_alloc_contiguous(uint64_t count, uint64_t limit);

/*
 * Hand the memory Limine used (its page tables, stack, boot info) to the
 * allocator. Only call this once none of it is in use any more.
 * Returns the number of bytes reclaimed.
 */
uint64_t pmm_reclaim_bootloader_memory(void);

/*
 * Give a range that was never part of usable memory (e.g. a boot module
 * we've finished with) to the allocator. Returns the bytes added.
 */
uint64_t pmm_release_range(uint64_t base, uint64_t length);

uint64_t pmm_total_bytes(void);
uint64_t pmm_free_bytes(void);

const struct mem_region *pmm_regions(size_t *count);
const char *pmm_region_type_name(uint64_t type);
