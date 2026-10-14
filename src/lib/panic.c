#include <stdarg.h>
#include <stdbool.h>

#include "lib/panic.h"
#include "arch/x86_64/cpu.h"
#include "drivers/console.h"
#include "lib/kprintf.h"
#include "arch/x86_64/smp.h"

static volatile int panicking;

bool panic_in_progress(void) {
    return panicking;
}

static bool is_kernel_address(uint64_t addr) {
    return addr >= 0xFFFF800000000000ULL;
}

void print_backtrace(uint64_t rip, uint64_t rbp) {
    kprintf("Backtrace (resolve with: addr2line -fpe build/kernel.elf <addr>):\n");
    if (rip) {
        kprintf("  %p\n", (void *)rip);
    }
    /* Works because we build with -fno-omit-frame-pointer: [rbp] = caller's rbp, [rbp+8] = return address. */
    for (int depth = 0; depth < 16; depth++) {
        if (!is_kernel_address(rbp) || (rbp & 7) != 0) {
            break;
        }
        const uint64_t *frame = (const uint64_t *)rbp;
        if (frame[1] == 0) {
            break;
        }
        kprintf("  %p\n", (void *)frame[1]);
        rbp = frame[0];
    }
}

void panic_begin(void) {
    interrupts_disable();
    if (__atomic_exchange_n(&panicking, 1, __ATOMIC_SEQ_CST)) {
        halt_forever();   /* another CPU is already reporting one (or this is a panic within the panic) */
    }
    smp_halt_others();
    uint64_t log_end = klog_position();   /* (what comes next is the report itself) */

    /* Whatever program had the screen (the desktop), the report goes on it now: what the kernel said
       last, then what went wrong. */
    console_panic_screen(0xD8D8E0, 0x101018);
    console_set_color(0xFFFFFF, 0x8B1A1A);
    kprintf(" viralOS ran into a problem it can't recover from, and stopped. \n");
    console_set_color(0x9A9AA6, 0x101018);
    kprintf("Restart the computer to continue. (A photo of this screen helps find the cause.)\n\n");
    kprintf("The kernel's log, up to the problem:\n");
    console_set_color(0xC8C8D0, 0x101018);
    klog_replay(16, log_end);
    console_set_color(0xFFFFFF, 0x8B1A1A);
}

void panic(const char *fmt, ...) {
    panic_begin();
    kprintf("\n KERNEL PANIC: ");
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf(" \n");
    console_set_color(0xFFB0B0, 0x101018);

    print_backtrace(0, (uint64_t)__builtin_frame_address(0));
    kprintf("\nSystem halted.\n");
    halt_forever();
}
