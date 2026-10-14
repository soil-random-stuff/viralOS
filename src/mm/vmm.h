#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Mapping flags. Without VMM_WRITE a page is read-only; without VMM_EXEC it is no-execute. */
#define VMM_WRITE (1u << 0)
#define VMM_EXEC  (1u << 1)
#define VMM_USER  (1u << 2)
#define VMM_WC    (1u << 3)   /* write-combining caching, for framebuffers */
#define VMM_UC    (1u << 4)   /* uncached, for device registers */
#define VMM_DEVICE (1u << 5)  /* user mapping of memory the process doesn't own (framebuffer):
                                 never freed with the address space */

/*
 * Build the kernel's own page tables (kernel image with per-section
 * permissions + the HHDM over all RAM) and switch CR3 to them.
 */
void vmm_init(uint64_t kernel_phys_base, uint64_t kernel_virt_base);

bool vmm_map(uint64_t virt, uint64_t phys, uint32_t flags);
/* Maps a range, using 2 MiB pages wherever alignment allows. */
bool vmm_map_range(uint64_t virt, uint64_t phys, uint64_t length, uint32_t flags);
void vmm_unmap(uint64_t virt);
bool vmm_translate(uint64_t virt, uint64_t *phys);

/* Map device registers uncached; returns the virtual address of `phys`. */
void *vmm_map_mmio(uint64_t phys, size_t size);

/* Allocate a kernel stack with an unmapped guard region below it; returns its top. */
uint64_t vmm_alloc_kernel_stack(size_t size);
void vmm_free_kernel_stack(uint64_t stack_top, size_t size);
bool vmm_is_stack_guard(uint64_t addr);

bool vmm_nx_enabled(void);

/*
 * User address spaces. A space is identified by the physical address of its
 * PML4 (what goes in CR3). Its kernel half is shared with every other space;
 * the user half (below USER_SPACE_END) is private.
 */
uint64_t vmm_kernel_space(void);
uint64_t vmm_create_space(void);             /* 0 if out of memory */
void vmm_destroy_space(uint64_t space);      /* frees all user pages; must not be active */
bool vmm_map_user(uint64_t space, uint64_t virt, uint64_t phys, uint32_t flags);
/* Physical address behind a user page, if it is mapped user-accessible (and writable if asked). */
bool vmm_user_lookup(uint64_t space, uint64_t virt, bool write, uint64_t *phys);
/* Remove a user page's mapping; returns its frame (for the caller to free). */
bool vmm_unmap_user(uint64_t space, uint64_t virt, uint64_t *phys);
uint64_t vmm_count_user_pages(uint64_t space);
