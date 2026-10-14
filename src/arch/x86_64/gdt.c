/*
 * Global Descriptor Table + Task State Segment, one pair per CPU.
 *
 * In 64-bit mode segmentation is mostly vestigial (bases and limits are
 * ignored), but the CPU still needs code/data descriptors to know which
 * privilege level it runs at, and a TSS to know which stack to switch to
 * when an interrupt arrives from user mode or via the IST. Every CPU
 * switches to a different stack, so every CPU has its own TSS (and the
 * GDT that points at it).
 */

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/gdt.h"
#include "arch/x86_64/smp.h"
#include "lib/panic.h"
#include "mm/vmm.h"

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/*
 * 64-bit code/data descriptors, encoded by hand:
 *   0x00AF9A000000FFFF  present, DPL 0, code, readable, long mode (L), 4K granularity
 *   0x00CF92000000FFFF  present, DPL 0, data, writable
 *   ...F2... / ...FA... are the same with DPL 3 (user).
 */
static const uint64_t descriptors[5] = {
    0,                      /* 0x00 null */
    0x00AF9A000000FFFFULL,  /* 0x08 kernel code */
    0x00CF92000000FFFFULL,  /* 0x10 kernel data */
    0x00CFF2000000FFFFULL,  /* 0x18 user data */
    0x00AFFA000000FFFFULL,  /* 0x20 user code */
};

/* The bootstrap processor's double fault stack (the others get theirs from vmm_alloc_kernel_stack). */
static uint8_t bsp_double_fault_stack[16384] __attribute__((aligned(16)));

static void set_tss_descriptor(uint64_t *gdt, const struct tss *tss) {
    uint64_t base = (uint64_t)tss;
    uint64_t limit = sizeof(*tss) - 1;

    gdt[5] = (limit & 0xFFFF)
           | ((base & 0xFFFFFF) << 16)
           | (0x89ULL << 40)                 /* present, type 9 = available 64-bit TSS */
           | (((limit >> 16) & 0xF) << 48)
           | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;
}

void gdt_init(struct cpu *cpu) {
    for (int i = 0; i < 5; i++) {
        cpu->gdt[i] = descriptors[i];
    }
    /* Dedicated stack for double faults, so a blown kernel stack still gets reported. */
    uint64_t df_top;
    if (cpu->index == 0) {
        df_top = (uint64_t)(bsp_double_fault_stack + sizeof(bsp_double_fault_stack));
    } else {
        df_top = cpu->double_fault_stack;   /* (allocated by the BSP: see smp_start_aps) */
    }
    cpu->tss.ist[IST_DOUBLE_FAULT - 1] = df_top;
    cpu->tss.iopb_offset = sizeof(cpu->tss); /* no I/O permission bitmap */
    set_tss_descriptor(cpu->gdt, &cpu->tss);

    struct gdt_pointer gdtr = {
        .limit = sizeof(cpu->gdt) - 1,
        .base = (uint64_t)cpu->gdt,
    };
    __asm__ volatile ("lgdt %0" : : "m"(gdtr));

    /*
     * CS can't be loaded with mov: push the new selector and a return
     * address, then do a far return. The data segments can be set directly.
     * (Loading GS clears its base: the caller sets the base afterwards.)
     */
    __asm__ volatile (
        "pushq %[cs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %[ds], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "xorw %%ax, %%ax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        : [cs] "i"(GDT_KERNEL_CODE), [ds] "i"(GDT_KERNEL_DATA)
        : "rax", "memory");

    __asm__ volatile ("ltr %0" : : "r"((uint16_t)GDT_TSS));
}

void gdt_set_kernel_stack(uint64_t stack_top) {
    this_cpu()->tss.rsp[0] = stack_top;
}
