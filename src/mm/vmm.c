/*
 * x86_64 4-level paging: PML4 -> PDPT -> PD -> PT -> 4 KiB page.
 * A PD entry with the PS bit maps a 2 MiB page directly.
 */

#include <limine.h>

#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/cpu.h"
#include "lib/panic.h"
#include "lib/string.h"

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PWT       (1ULL << 3)
#define PTE_PCD       (1ULL << 4)
#define PTE_HUGE      (1ULL << 7)   /* PS bit in a PD entry */
#define PTE_PAT_4K    (1ULL << 7)   /* PAT bit in a PT entry */
#define PTE_PAT_HUGE  (1ULL << 12)  /* PAT bit in a 2 MiB PD entry */
#define PTE_NOFREE    (1ULL << 9)   /* (bit for OS use) not ours to free: device memory */
#define PTE_NX        (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define MSR_EFER      0xC0000080
#define EFER_NXE      (1ULL << 11)
#define CPUID_PAT     (1u << 16)    /* CPUID.1:EDX */

uint64_t hhdm_offset;

static uint64_t *kernel_pml4;
static uint64_t kernel_pml4_phys;
static bool nx_enabled;
static bool pat_supported;
static uint64_t stack_slot_bitmap[KERNEL_STACK_SLOTS / 64];
static uint64_t mmio_next = KERNEL_MMIO_BASE;

/* Section boundaries from linker.ld. */
extern const char __kernel_start[], __text_start[], __text_end[];
extern const char __rodata_start[], __rodata_end[], __data_start[], __kernel_end[];

static size_t table_index(uint64_t virt, int level) {
    return (virt >> (12 + 9 * (level - 1))) & 0x1FF;
}

/*
 * PAT entry 5 is write-combining (Limine sets the PAT up this way); index 5
 * is selected by PAT=1, PCD=0, PWT=1.
 */
static uint64_t pte_flags(uint32_t flags, bool huge) {
    uint64_t pte = PTE_PRESENT;
    if (flags & VMM_WRITE) {
        pte |= PTE_WRITABLE;
    }
    if (flags & VMM_USER) {
        pte |= PTE_USER;
    }
    if (!(flags & VMM_EXEC) && nx_enabled) {
        pte |= PTE_NX;
    }
    if ((flags & VMM_WC) && pat_supported) {
        pte |= PTE_PWT | (huge ? PTE_PAT_HUGE : PTE_PAT_4K);
    } else if (flags & VMM_UC) {
        pte |= PTE_PCD | PTE_PWT;   /* PAT entry 3: uncached */
    }
    if (huge) {
        pte |= PTE_HUGE;
    }
    if (flags & VMM_DEVICE) {
        pte |= PTE_NOFREE;
    }
    return pte;
}

static uint64_t alloc_table(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        panic("vmm: out of memory for page tables");
    }
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys;
}

/* Follow (or create) the entry at `index` down to the next table level. */
static uint64_t *next_level(uint64_t *table, size_t index, bool create, bool user) {
    uint64_t entry = table[index];
    if (entry & PTE_PRESENT) {
        if (entry & PTE_HUGE) {
            return NULL; /* already covered by a large page */
        }
        if (user && !(entry & PTE_USER)) {
            table[index] = entry | PTE_USER;
        }
        return phys_to_virt(entry & PTE_ADDR_MASK);
    }
    if (!create) {
        return NULL;
    }
    uint64_t phys = alloc_table();
    table[index] = phys | PTE_PRESENT | PTE_WRITABLE | (user ? PTE_USER : 0);
    return phys_to_virt(phys);
}

/* Returns the page directory covering `virt`. */
static uint64_t *walk_to_pd(uint64_t *pml4, uint64_t virt, bool create, bool user) {
    uint64_t *pdpt = next_level(pml4, table_index(virt, 4), create, user);
    if (!pdpt) {
        return NULL;
    }
    return next_level(pdpt, table_index(virt, 3), create, user);
}

/*
 * The page tables are shared by every thread, so updates run with
 * interrupts off (see irq_save) to keep a preempted update from being
 * interleaved with another one.
 */
static bool map_in(uint64_t *pml4, uint64_t virt, uint64_t phys, uint32_t flags) {
    uint64_t irq = irq_save();
    bool user = flags & VMM_USER;
    uint64_t *pd = walk_to_pd(pml4, virt, true, user);
    uint64_t *pt = pd ? next_level(pd, table_index(virt, 2), true, user) : NULL;
    bool changed = false;
    if (pt) {
        changed = pt[table_index(virt, 1)] & PTE_PRESENT;
        pt[table_index(virt, 1)] = (phys & PTE_ADDR_MASK) | pte_flags(flags, false);
        __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    }
    if (changed && pml4 == kernel_pml4 && virt >= USER_SPACE_END) {
        smp_kernel_mappings_changed();   /* other CPUs may have the old one cached */
    }
    irq_restore(irq);
    return pt != NULL;
}

bool vmm_map(uint64_t virt, uint64_t phys, uint32_t flags) {
    return map_in(kernel_pml4, virt, phys, flags);
}

static bool map_huge(uint64_t virt, uint64_t phys, uint32_t flags) {
    uint64_t irq = irq_save();
    bool ok = false;
    uint64_t *pd = walk_to_pd(kernel_pml4, virt, true, flags & VMM_USER);
    if (pd) {
        uint64_t *entry = &pd[table_index(virt, 2)];
        /* Refuse if a page table with 4 KiB mappings is already here. */
        if (!(*entry & PTE_PRESENT) || (*entry & PTE_HUGE)) {
            *entry = (phys & PTE_ADDR_MASK) | pte_flags(flags, true);
            __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
            ok = true;
        }
    }
    irq_restore(irq);
    return ok;
}

bool vmm_map_range(uint64_t virt, uint64_t phys, uint64_t length, uint32_t flags) {
    uint64_t end = virt + length;
    while (virt < end) {
        bool huge_ok = virt % HUGE_PAGE_SIZE == 0 && phys % HUGE_PAGE_SIZE == 0 &&
                       end - virt >= HUGE_PAGE_SIZE;
        if (huge_ok && map_huge(virt, phys, flags)) {
            virt += HUGE_PAGE_SIZE;
            phys += HUGE_PAGE_SIZE;
            continue;
        }
        if (!vmm_map(virt, phys, flags)) {
            return false;
        }
        virt += PAGE_SIZE;
        phys += PAGE_SIZE;
    }
    return true;
}

void vmm_unmap(uint64_t virt) {
    uint64_t irq = irq_save();
    uint64_t *pd = walk_to_pd(kernel_pml4, virt, false, false);
    uint64_t *pt = pd ? next_level(pd, table_index(virt, 2), false, false) : NULL;
    if (pt) {
        pt[table_index(virt, 1)] = 0;
        __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    }
    smp_kernel_mappings_changed();
    irq_restore(irq);
}

bool vmm_translate(uint64_t virt, uint64_t *phys) {
    uint64_t *pdpt = next_level(kernel_pml4, table_index(virt, 4), false, false);
    if (!pdpt) {
        return false;
    }
    uint64_t pdpt_entry = pdpt[table_index(virt, 3)];
    if (!(pdpt_entry & PTE_PRESENT) || (pdpt_entry & PTE_HUGE)) {
        return false; /* we never create 1 GiB pages */
    }
    uint64_t *pd = phys_to_virt(pdpt_entry & PTE_ADDR_MASK);
    uint64_t pd_entry = pd[table_index(virt, 2)];
    if (!(pd_entry & PTE_PRESENT)) {
        return false;
    }
    if (pd_entry & PTE_HUGE) {
        *phys = (pd_entry & PTE_ADDR_MASK & ~(HUGE_PAGE_SIZE - 1)) + virt % HUGE_PAGE_SIZE;
        return true;
    }
    uint64_t *pt = phys_to_virt(pd_entry & PTE_ADDR_MASK);
    uint64_t pt_entry = pt[table_index(virt, 1)];
    if (!(pt_entry & PTE_PRESENT)) {
        return false;
    }
    *phys = (pt_entry & PTE_ADDR_MASK) + virt % PAGE_SIZE;
    return true;
}

/* --- user address spaces ------------------------------------------------------------ */

uint64_t vmm_kernel_space(void) {
    return kernel_pml4_phys;
}

uint64_t vmm_create_space(void) {
    uint64_t irq = irq_save();
    uint64_t phys = pmm_alloc_page();
    if (phys) {
        uint64_t *pml4 = phys_to_virt(phys);
        memset(pml4, 0, PAGE_SIZE / 2);                      /* user half: empty */
        memcpy(pml4 + 256, kernel_pml4 + 256, PAGE_SIZE / 2); /* kernel half: shared */
    }
    irq_restore(irq);
    return phys;
}

bool vmm_map_user(uint64_t space, uint64_t virt, uint64_t phys, uint32_t flags) {
    if (virt >= USER_SPACE_END) {
        return false;
    }
    return map_in(phys_to_virt(space), virt, phys, flags | VMM_USER);
}

bool vmm_user_lookup(uint64_t space, uint64_t virt, bool write, uint64_t *phys) {
    if (virt >= USER_SPACE_END) {
        return false;
    }
    uint64_t *pd = walk_to_pd(phys_to_virt(space), virt, false, false);
    uint64_t *pt = pd ? next_level(pd, table_index(virt, 2), false, false) : NULL;
    if (!pt) {
        return false;
    }
    uint64_t pte = pt[table_index(virt, 1)];
    if (!(pte & PTE_PRESENT) || !(pte & PTE_USER) || (write && !(pte & PTE_WRITABLE))) {
        return false;
    }
    *phys = (pte & PTE_ADDR_MASK) + virt % PAGE_SIZE;
    return true;
}

bool vmm_unmap_user(uint64_t space, uint64_t virt, uint64_t *phys) {
    if (virt >= USER_SPACE_END) {
        return false;
    }
    uint64_t irq = irq_save();
    bool found = false;
    uint64_t *pd = walk_to_pd(phys_to_virt(space), virt, false, false);
    uint64_t *pt = pd ? next_level(pd, table_index(virt, 2), false, false) : NULL;
    if (pt && (pt[table_index(virt, 1)] & PTE_PRESENT)) {
        /* Device pages report 0: there's nothing for the caller to free. */
        *phys = (pt[table_index(virt, 1)] & PTE_NOFREE) ? 0 : pt[table_index(virt, 1)] & PTE_ADDR_MASK;
        pt[table_index(virt, 1)] = 0;
        __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
        found = true;
    }
    irq_restore(irq);
    return found;
}

uint64_t vmm_count_user_pages(uint64_t space) {
    uint64_t irq = irq_save();
    uint64_t count = 0;
    uint64_t *pml4 = phys_to_virt(space);
    for (size_t i4 = 0; i4 < 256; i4++) {
        if (!(pml4[i4] & PTE_PRESENT)) {
            continue;
        }
        uint64_t *pdpt = phys_to_virt(pml4[i4] & PTE_ADDR_MASK);
        for (size_t i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT)) {
                continue;
            }
            uint64_t *pd = phys_to_virt(pdpt[i3] & PTE_ADDR_MASK);
            for (size_t i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & PTE_HUGE)) {
                    continue;
                }
                uint64_t *pt = phys_to_virt(pd[i2] & PTE_ADDR_MASK);
                for (size_t i1 = 0; i1 < 512; i1++) {
                    count += (pt[i1] & PTE_PRESENT) && !(pt[i1] & PTE_NOFREE);
                }
            }
        }
    }
    irq_restore(irq);
    return count;
}

/* Free every page and page table in the user half, then the PML4 itself. */
void vmm_destroy_space(uint64_t space) {
    uint64_t irq = irq_save();
    uint64_t *pml4 = phys_to_virt(space);
    for (size_t i4 = 0; i4 < 256; i4++) {
        if (!(pml4[i4] & PTE_PRESENT)) {
            continue;
        }
        uint64_t *pdpt = phys_to_virt(pml4[i4] & PTE_ADDR_MASK);
        for (size_t i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT)) {
                continue;
            }
            uint64_t *pd = phys_to_virt(pdpt[i3] & PTE_ADDR_MASK);
            for (size_t i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & PTE_HUGE)) {
                    continue;   /* user mappings are never huge */
                }
                uint64_t *pt = phys_to_virt(pd[i2] & PTE_ADDR_MASK);
                for (size_t i1 = 0; i1 < 512; i1++) {
                    if ((pt[i1] & PTE_PRESENT) && !(pt[i1] & PTE_NOFREE)) {
                        pmm_free_page(pt[i1] & PTE_ADDR_MASK);
                    }
                }
                pmm_free_page(pd[i2] & PTE_ADDR_MASK);
            }
            pmm_free_page(pdpt[i3] & PTE_ADDR_MASK);
        }
        pmm_free_page(pml4[i4] & PTE_ADDR_MASK);
    }
    pmm_free_page(space);
    irq_restore(irq);
}

/* --- boot-time setup ------------------------------------------------------------------ */

static void map_kernel_section(const char *start, const char *end, uint32_t flags,
                               uint64_t phys_base, uint64_t virt_base) {
    uint64_t vs = ALIGN_DOWN(start, PAGE_SIZE);
    uint64_t ve = ALIGN_UP(end, PAGE_SIZE);
    if (!vmm_map_range(vs, vs - virt_base + phys_base, ve - vs, flags)) {
        panic("vmm: failed to map kernel section %p-%p", (void *)vs, (void *)ve);
    }
}

static bool hhdm_mapped_type(uint64_t type) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE:
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
    case LIMINE_MEMMAP_FRAMEBUFFER:
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
    case LIMINE_MEMMAP_ACPI_NVS:
    case LIMINE_MEMMAP_RESERVED_MAPPED:
        return true;
    default:
        return false;
    }
}

void vmm_init(uint64_t kernel_phys_base, uint64_t kernel_virt_base) {
    nx_enabled = rdmsr(MSR_EFER) & EFER_NXE;
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    pat_supported = d & CPUID_PAT;

    uint64_t pml4_phys = alloc_table();
    kernel_pml4 = phys_to_virt(pml4_phys);
    kernel_pml4_phys = pml4_phys;

    /*
     * Pre-create every kernel-half PDPT now. Future address spaces copy the
     * kernel half of this PML4, so later kernel mappings must never need a
     * new PML4 entry or processes would stop seeing them.
     */
    for (size_t i = 256; i < 512; i++) {
        next_level(kernel_pml4, i, true, false);
    }

    /* Kernel image, with permissions per section. */
    map_kernel_section(__kernel_start, __text_start, 0,
                       kernel_phys_base, kernel_virt_base);          /* Limine requests */
    map_kernel_section(__text_start, __text_end, VMM_EXEC,
                       kernel_phys_base, kernel_virt_base);
    map_kernel_section(__rodata_start, __rodata_end, 0,
                       kernel_phys_base, kernel_virt_base);
    map_kernel_section(__data_start, __kernel_end, VMM_WRITE,
                       kernel_phys_base, kernel_virt_base);

    /* Higher-half direct map of RAM and the other regions Limine maps. */
    size_t count;
    const struct mem_region *regions = pmm_regions(&count);
    for (size_t i = 0; i < count; i++) {
        if (!hhdm_mapped_type(regions[i].type)) {
            continue;
        }
        uint64_t start = ALIGN_DOWN(regions[i].base, PAGE_SIZE);
        uint64_t end = ALIGN_UP(regions[i].base + regions[i].length, PAGE_SIZE);
        uint32_t flags = VMM_WRITE;
        if (regions[i].type == LIMINE_MEMMAP_FRAMEBUFFER) {
            flags |= VMM_WC;
        }
        if (!vmm_map_range(start + hhdm_offset, start, end - start, flags)) {
            panic("vmm: failed to map HHDM region %p-%p", (void *)start, (void *)end);
        }
    }

    write_cr3(pml4_phys);
}

void *vmm_map_mmio(uint64_t phys, size_t size) {
    uint64_t irq = irq_save();
    uint64_t base = ALIGN_DOWN(phys, PAGE_SIZE);
    uint64_t length = ALIGN_UP(phys + size, PAGE_SIZE) - base;
    uint64_t virt = mmio_next;
    void *result = NULL;
    if (virt + length <= KERNEL_MMIO_BASE + KERNEL_MMIO_MAX &&
        vmm_map_range(virt, base, length, VMM_WRITE | VMM_UC)) {
        mmio_next += length;
        result = (void *)(virt + (phys - base));
    }
    irq_restore(irq);
    return result;
}

static bool slot_used(uint64_t slot) {
    return stack_slot_bitmap[slot / 64] & (1ULL << (slot % 64));
}

static void unmap_and_free(uint64_t start, uint64_t end) {
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t phys;
        if (vmm_translate(va, &phys)) {
            vmm_unmap(va);
            pmm_free_page(phys);
        }
    }
}

uint64_t vmm_alloc_kernel_stack(size_t size) {
    size = ALIGN_UP(size, PAGE_SIZE);
    if (size + PAGE_SIZE > KERNEL_STACK_SLOT) {
        return 0;
    }

    uint64_t irq = irq_save();
    uint64_t top = 0;
    for (uint64_t slot = 0; slot < KERNEL_STACK_SLOTS; slot++) {
        if (slot_used(slot)) {
            continue;
        }
        uint64_t slot_top = KERNEL_STACKS_BASE + (slot + 1) * KERNEL_STACK_SLOT;
        bool ok = true;
        for (uint64_t va = slot_top - size; va < slot_top && ok; va += PAGE_SIZE) {
            uint64_t phys = pmm_alloc_page();
            ok = phys && vmm_map(va, phys, VMM_WRITE);
            if (phys && !ok) {
                pmm_free_page(phys);
            }
        }
        if (ok) {
            stack_slot_bitmap[slot / 64] |= 1ULL << (slot % 64);
            top = slot_top;
        } else {
            unmap_and_free(slot_top - size, slot_top);
        }
        break;
    }
    irq_restore(irq);
    return top;
}

void vmm_free_kernel_stack(uint64_t stack_top, size_t size) {
    uint64_t slot = (stack_top - KERNEL_STACKS_BASE) / KERNEL_STACK_SLOT - 1;
    uint64_t irq = irq_save();
    unmap_and_free(stack_top - ALIGN_UP(size, PAGE_SIZE), stack_top);
    stack_slot_bitmap[slot / 64] &= ~(1ULL << (slot % 64));
    irq_restore(irq);
}

bool vmm_is_stack_guard(uint64_t addr) {
    if (addr < KERNEL_STACKS_BASE ||
        addr >= KERNEL_STACKS_BASE + KERNEL_STACK_SLOTS * KERNEL_STACK_SLOT) {
        return false;
    }
    uint64_t phys;
    return slot_used((addr - KERNEL_STACKS_BASE) / KERNEL_STACK_SLOT) &&
           !vmm_translate(addr, &phys);
}

bool vmm_nx_enabled(void) {
    return nx_enabled;
}
