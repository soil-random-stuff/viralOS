#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Print a message and backtrace, then halt. */
__attribute__((noreturn, format(printf, 1, 2)))
void panic(const char *fmt, ...);

/* Disable interrupts, stop the other CPUs, take the screen back and show the end of the kernel's log;
   then the console is in panic colours for the report. */
void panic_begin(void);
/* (The NMI handler of the other CPUs) Is a panic being reported? */
bool panic_in_progress(void);

/* Walk saved frame pointers starting at `rbp`; `rip` (if non-zero) is printed first. */
void print_backtrace(uint64_t rip, uint64_t rbp);
