#pragma once

#include <stdint.h>

#define PAGE_SIZE      4096ULL
#define HUGE_PAGE_SIZE (2ULL * 1024 * 1024)

#define ALIGN_UP(x, a)   (((uint64_t)(x) + (a) - 1) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((uint64_t)(x) & ~((uint64_t)(a) - 1))

/*
 * Virtual address space layout.
 *
 * User half (one per process):
 *   0x0000000000400000  program code and data (ELF segments)
 *   0x00007ffffff00000  top of the user stack (grows down, guard page below)
 *   0x0000800000000000  USER_SPACE_END: everything above is kernel-only
 *
 * Kernel half (shared by every process):
 *
 *   0xffff800000000000  HHDM: all of physical memory, at hhdm_offset + phys
 *   0xffffc00000000000  kernel heap (grows up, mapped on demand)
 *   0xffffd00000000000  device MMIO (uncached), handed out by vmm_map_mmio
 *   0xffffe00000000000  kernel stacks: 1 MiB slots, stack at the top of each
 *                       slot, everything below it left unmapped as a guard
 *   0xffffffff80000000  kernel image
 */
#define USER_SPACE_END     0x0000800000000000ULL
#define USER_FRAMEBUFFER   0x0000700000000000ULL   /* where display_claim maps the screen */
#define USER_SHM_BASE      0x0000600000000000ULL   /* shared memory mappings go up from here */
#define USER_STACK_TOP     0x00007ffffff00000ULL
#define USER_STACK_SIZE    (64 * 1024)

#define KERNEL_HEAP_BASE   0xffffc00000000000ULL
#define KERNEL_HEAP_MAX    (64ULL << 30)
#define KERNEL_MMIO_BASE   0xffffd00000000000ULL
#define KERNEL_MMIO_MAX    (64ULL << 30)
#define KERNEL_STACKS_BASE 0xffffe00000000000ULL
#define KERNEL_STACK_SLOT  (1ULL << 20)
#define KERNEL_STACK_SLOTS 1024

/* Offset of the higher-half direct map, provided by Limine. */
extern uint64_t hhdm_offset;

static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm_offset);
}
