#include "drivers/timer.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/idt.h"
#include "sched/sched.h"

static volatile uint64_t ticks;
static uint32_t tick_hz;
static uint64_t tsc_start, tsc_hz;

static inline uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/* Every CPU's timer interrupt: its time slices (and, on the BSP, the sleepers). The tick count itself
   is the BSP's, counted before it takes the kernel lock (timer_count_tick, from idt.c). */
static void timer_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    sched_tick();
}

void timer_count_tick(void) {
    __atomic_add_fetch(&ticks, 1, __ATOMIC_RELAXED);
}

void timer_init(uint32_t hz) {
    tick_hz = hz;
    interrupt_register(VECTOR_TIMER, timer_interrupt);
    lapic_timer_start(hz, VECTOR_TIMER);
    tsc_start = read_tsc();
    tsc_hz = lapic_tsc_frequency();
}

void udelay(uint64_t us) {
    uint64_t end = uptime_us() + us;
    while (uptime_us() < end) {
        __asm__ volatile ("pause");
    }
}

uint64_t uptime_us(void) {
    if (!tsc_hz) {
        return uptime_ms() * 1000;
    }
    uint64_t t = read_tsc() - tsc_start;
    /* (t * 1000000 / tsc_hz without overflowing for years) */
    return t / tsc_hz * 1000000 + (t % tsc_hz) * 1000000 / tsc_hz;
}

uint64_t timer_ticks(void) {
    return ticks;
}

uint32_t timer_hz(void) {
    return tick_hz;
}

uint64_t uptime_ms(void) {
    return ticks * 1000 / tick_hz;
}

uint64_t timer_ms_to_ticks(uint64_t ms) {
    uint64_t t = (ms * tick_hz + 999) / 1000;
    return t ? t : 1;
}
