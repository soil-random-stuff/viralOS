#include <stdbool.h>

#include <limine.h>

#include "mm/mm.h"
#include "mm/pmm.h"
#include "arch/x86_64/cpu.h"
#include "lib/panic.h"
#include "lib/string.h"

#define MAX_REGIONS 256

/* Our own copy, because Limine's memory map lives in reclaimable memory. */
static struct mem_region regions[MAX_REGIONS];
static size_t region_count;

static uint64_t *bitmap;        /* bit set = frame in use (or not RAM) */
static uint64_t bitmap_words;
static uint64_t frame_count;    /* frames covered by the bitmap */
static uint64_t usable_frames;
static uint64_t free_frames;
static uint64_t next_word;      /* where the next allocation search starts */

static bool frame_used(uint64_t f) { return bitmap[f / 64] & (1ULL << (f % 64)); }
static void mark_used(uint64_t f)  { bitmap[f / 64] |= 1ULL << (f % 64); }
static void mark_free(uint64_t f)  { bitmap[f / 64] &= ~(1ULL << (f % 64)); }

/* Mark whole frames inside [base, base+length) free; returns how many changed. */
static uint64_t free_range(uint64_t base, uint64_t length) {
    uint64_t first = ALIGN_UP(base, PAGE_SIZE) / PAGE_SIZE;
    uint64_t end = ALIGN_DOWN(base + length, PAGE_SIZE) / PAGE_SIZE;
    if (end > frame_count) {
        end = frame_count;
    }
    uint64_t count = 0;
    for (uint64_t f = first; f < end; f++) {
        if (f != 0 && frame_used(f)) { /* frame 0 stays reserved: 0 means "no page" */
            mark_free(f);
            count++;
        }
    }
    return count;
}

static void reserve_range(uint64_t base, uint64_t length) {
    uint64_t first = ALIGN_DOWN(base, PAGE_SIZE) / PAGE_SIZE;
    uint64_t end = ALIGN_UP(base + length, PAGE_SIZE) / PAGE_SIZE;
    for (uint64_t f = first; f < end && f < frame_count; f++) {
        if (!frame_used(f)) {
            mark_used(f);
            free_frames--;
        }
    }
}

void pmm_init(struct limine_memmap_response *memmap) {
    region_count = memmap->entry_count;
    if (region_count > MAX_REGIONS) {
        region_count = MAX_REGIONS;
    }
    uint64_t highest = 0;
    for (size_t i = 0; i < region_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        regions[i] = (struct mem_region){ e->base, e->length, e->type };
        if (e->type == LIMINE_MEMMAP_USABLE || e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            if (e->base + e->length > highest) {
                highest = e->base + e->length;
            }
        }
    }

    frame_count = ALIGN_UP(highest, PAGE_SIZE) / PAGE_SIZE;
    bitmap_words = (frame_count + 63) / 64;
    uint64_t bitmap_bytes = ALIGN_UP(bitmap_words * sizeof(uint64_t), PAGE_SIZE);

    /*
     * The bitmap itself goes in the first usable region big enough to hold
     * it. Usable memory can start at physical 0 (UEFI does this), but frame 0
     * is kept reserved, so start at 0x1000 at the earliest.
     */
    uint64_t bitmap_phys = 0;
    for (size_t i = 0; i < region_count; i++) {
        uint64_t start = ALIGN_UP(regions[i].base, PAGE_SIZE);
        uint64_t end = regions[i].base + regions[i].length;
        if (start < PAGE_SIZE) {
            start = PAGE_SIZE;
        }
        if (regions[i].type == LIMINE_MEMMAP_USABLE && end > start && end - start >= bitmap_bytes) {
            bitmap_phys = start;
            break;
        }
    }
    if (!bitmap_phys) {
        panic("pmm: no usable region can hold the %lu-byte frame bitmap", bitmap_bytes);
    }
    bitmap = phys_to_virt(bitmap_phys);
    memset(bitmap, 0xFF, bitmap_words * sizeof(uint64_t));

    for (size_t i = 0; i < region_count; i++) {
        if (regions[i].type == LIMINE_MEMMAP_USABLE) {
            usable_frames += free_range(regions[i].base, regions[i].length);
        }
    }
    free_frames = usable_frames;
    reserve_range(bitmap_phys, bitmap_bytes);
}

uint64_t pmm_alloc_page(void) {
    uint64_t irq = irq_save();
    uint64_t result = 0;
    for (uint64_t i = 0; i < bitmap_words; i++) {
        uint64_t w = (next_word + i) % bitmap_words;
        if (bitmap[w] == ~0ULL) {
            continue;
        }
        uint64_t frame = w * 64 + (uint64_t)__builtin_ctzll(~bitmap[w]);
        mark_used(frame);
        free_frames--;
        next_word = w;
        result = frame * PAGE_SIZE;
        break;
    }
    irq_restore(irq);
    return result;
}

/* `count` contiguous free frames, all below `limit` (for devices that can't address more). */
uint64_t pmm_alloc_contiguous(uint64_t count, uint64_t limit) {
    uint64_t irq = irq_save();
    uint64_t end = limit / PAGE_SIZE < frame_count ? limit / PAGE_SIZE : frame_count;
    uint64_t run = 0, result = 0;
    for (uint64_t frame = 1; frame < end; frame++) {
        run = frame_used(frame) ? 0 : run + 1;
        if (run == count) {
            uint64_t first = frame + 1 - count;
            for (uint64_t f = first; f <= frame; f++) {
                mark_used(f);
            }
            free_frames -= count;
            result = first * PAGE_SIZE;
            break;
        }
    }
    irq_restore(irq);
    return result;
}

void pmm_free_page(uint64_t phys) {
    uint64_t frame = phys / PAGE_SIZE;
    if (phys % PAGE_SIZE != 0 || frame == 0 || frame >= frame_count) {
        panic("pmm_free_page: bad address %p", (void *)phys);
    }
    uint64_t irq = irq_save();
    if (!frame_used(frame)) {
        panic("pmm_free_page: double free of %p", (void *)phys);
    }
    mark_free(frame);
    free_frames++;
    irq_restore(irq);
}

uint64_t pmm_reclaim_bootloader_memory(void) {
    uint64_t reclaimed = 0;
    for (size_t i = 0; i < region_count; i++) {
        if (regions[i].type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            reclaimed += free_range(regions[i].base, regions[i].length);
        }
    }
    usable_frames += reclaimed;
    free_frames += reclaimed;
    return reclaimed * PAGE_SIZE;
}

uint64_t pmm_release_range(uint64_t base, uint64_t length) {
    uint64_t irq = irq_save();
    uint64_t released = free_range(base, length);
    usable_frames += released;
    free_frames += released;
    irq_restore(irq);
    return released * PAGE_SIZE;
}

uint64_t pmm_total_bytes(void) {
    return usable_frames * PAGE_SIZE;
}

uint64_t pmm_free_bytes(void) {
    return free_frames * PAGE_SIZE;
}

const struct mem_region *pmm_regions(size_t *count) {
    *count = region_count;
    return regions;
}

const char *pmm_region_type_name(uint64_t type) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE:                 return "usable";
    case LIMINE_MEMMAP_RESERVED:               return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "ACPI reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS:               return "ACPI NVS";
    case LIMINE_MEMMAP_BAD_MEMORY:             return "bad memory";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "bootloader reclaimable";
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "kernel and modules";
    case LIMINE_MEMMAP_FRAMEBUFFER:            return "framebuffer";
    case LIMINE_MEMMAP_RESERVED_MAPPED:        return "reserved (mapped)";
    default:                                   return "unknown";
    }
}
