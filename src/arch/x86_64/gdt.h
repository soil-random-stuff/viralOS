#pragma once

/* Segment selectors. User data must sit right below user code for SYSRET. */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

/* Interrupt Stack Table slot used for double faults (see idt.c). */
#define IST_DOUBLE_FAULT 1

#include <stdint.h>

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];     /* stack loaded on a switch to ring 0/1/2 */
    uint64_t reserved1;
    uint64_t ist[7];     /* Interrupt Stack Table: IST1..IST7 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct cpu;

/* Load this CPU's own GDT and TSS (each CPU has one: the TSS holds its kernel stack). */
void gdt_init(struct cpu *cpu);

/* Stack this CPU switches to when an interrupt arrives from user mode. */
void gdt_set_kernel_stack(uint64_t stack_top);
