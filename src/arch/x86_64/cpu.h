#pragma once

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

/* A write to the unused POST port takes ~1us: lets slow devices like the PIC settle. */
static inline void outl_port(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint32_t inl_port(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

#define MSR_FS_BASE 0xC0000100

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

/* context.S: move onto a new stack and call fn there. */
__attribute__((noreturn)) void switch_stack_and_call(uint64_t stack_top, void (*fn)(void));

#define RFLAGS_IF (1ULL << 9)

/*
 * Disable interrupts and return the previous RFLAGS, for irq_restore().
 * On a single CPU this is our lock: nothing can preempt the code in between.
 */
static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    if (flags & RFLAGS_IF) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

static inline void interrupts_enable(void) {
    __asm__ volatile ("sti" : : : "memory");
}

static inline void interrupts_disable(void) {
    __asm__ volatile ("cli" : : : "memory");
}

/*
 * Enable interrupts and halt until the next one arrives. `sti` only takes
 * effect after the following instruction, so no interrupt can slip in
 * between the two and leave us sleeping on an event that already happened.
 */
static inline void wait_for_interrupt(void) {
    __asm__ volatile ("sti; hlt" : : : "memory");
}

/* Stop this CPU for good: interrupts off, then halt in a loop. */
__attribute__((noreturn)) static inline void halt_forever(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
