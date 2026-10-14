#pragma once

/*
 * Several CPUs. The bootloader (Limine's MP feature) starts the other processors ("APs"); each gets
 * a struct cpu of its own, reached through the GS segment while it runs kernel code.
 *
 * Locking: one big kernel lock. A CPU holds it whenever it runs kernel code, and lets go when it
 * goes back to a program (user mode) or has nothing to do (idle). So programs run on every CPU at
 * once, while the kernel runs on one at a time: everything written for one CPU, which keeps others
 * out by turning interrupts off (irq_save), stays correct. The lock is taken in the interrupt and
 * system call entry paths and in the idle loop; nothing else needs to think about it.
 */

#include <stdbool.h>
#include <stdint.h>

#include "arch/x86_64/gdt.h"

#define MAX_CPUS 64

struct thread;

struct cpu {
    /* user.S reaches these through %gs: keep the offsets. */
    uint64_t kernel_rsp;         /* 0: the stack a system call starts on */
    uint64_t user_rsp;           /* 8: scratch while switching stacks */
    struct cpu *self;            /* 16: this struct (this_cpu()) */

    uint32_t index;              /* 0 = the bootstrap processor */
    uint32_t lapic_id;
    bool online;

    /* The scheduler's, per CPU. */
    struct thread *current;
    struct thread *idle;
    volatile bool need_resched;
    uint64_t active_space;       /* what's in CR3 */
    uint64_t loaded_fs_base;     /* what's in the FS base MSR */

    bool holds_lock;             /* the big kernel lock */
    uint64_t tlb_generation;     /* kernel mappings seen (see smp_kernel_mappings_changed) */

    uint64_t gdt[7];
    struct tss tss;
    uint64_t ticks;              /* timer interrupts here */

    /* Starting up (APs): prepared by the BSP, so nothing is allocated on several CPUs at once. */
    uint64_t double_fault_stack; /* top of its double fault stack */
    volatile uint32_t stage;     /* how far ap_main got (for the report if it never arrives) */
    volatile bool left_out;      /* it didn't report in time: it must stay parked for good */
};

static inline struct cpu *this_cpu(void) {
    struct cpu *c;
    __asm__ volatile ("movq %%gs:16, %0" : "=r"(c));
    return c;
}

/* Set up the bootstrap processor's struct cpu (GS) as early as possible. */
void smp_init_bsp(void);
/* Ask the bootloader's parked processors to start; they set themselves up (own page tables, stack)
   and wait for smp_release_aps(). Call before bootloader memory is reclaimed. */
void smp_start_aps(void *limine_mp_response);
/* Once the scheduler, timer, FPU and system calls are ready on the BSP: let the APs join in. */
void smp_release_aps(void);

unsigned smp_cpu_count(void);    /* online */
struct cpu *smp_cpu(unsigned index);

/* The big kernel lock. */
void kernel_lock(void);
void kernel_unlock(void);
static inline bool kernel_lock_held(void) {
    return this_cpu()->holds_lock;
}

/* A kernel page mapping changed: other CPUs drop their cached translations before kernel code runs. */
void smp_kernel_mappings_changed(void);

/* Inter-processor interrupts. */
#define VECTOR_RESCHEDULE 0xF0
/* Make `cpu` look at its run queue (it may be idle). */
void smp_kick(struct cpu *cpu);
/* Stop every other CPU right now (panic): an NMI each. */
void smp_halt_others(void);
