#pragma once

#include <stdint.h>

/* System tick, driven by the local APIC timer. Each tick runs the scheduler. */
void timer_init(uint32_t hz);

uint64_t timer_ticks(void);
/* (idt.c, on the BSP's timer interrupt) One more tick. */
void timer_count_tick(void);
uint32_t timer_hz(void);
uint64_t uptime_ms(void);
/* Finer: microseconds since the timer started (from the time stamp counter). */
uint64_t uptime_us(void);
/* Wait this many microseconds, spinning (for hardware that wants short pauses). */
void udelay(uint64_t us);

/* Ticks needed to cover `ms` milliseconds (rounded up, at least 1). */
uint64_t timer_ms_to_ticks(uint64_t ms);
