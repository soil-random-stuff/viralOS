#pragma once

#include <stdint.h>

/*
 * 8253/8254 Programmable Interval Timer, used only as a known-frequency
 * reference to calibrate the local APIC timer. Polled, no interrupts.
 */
void pit_oneshot_start(uint32_t microseconds);   /* at most ~54900 us */
void pit_oneshot_wait(void);
