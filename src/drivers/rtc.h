#pragma once

#include <stdint.h>

/*
 * The wall clock: the PC's battery-backed real-time clock (CMOS), read once
 * at boot; after that the timer keeps time.
 *
 * The RTC has no notion of time zones: it holds whatever the machine's
 * owner set it to. PCs running Windows (and QEMU with -rtc base=localtime)
 * keep local time there, which is what we show.
 */
void rtc_init(void);
uint64_t rtc_time(void);   /* seconds since 1970-01-01 00:00 */
