/* Shell commands about the machine: memory, threads, timers, crash tests. */

#include <stddef.h>
#include <stdint.h>

#include "shell/commands.h"
#include "arch/x86_64/cpu.h"
#include "drivers/console.h"
#include "drivers/timer.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"
#include "proc/process.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include "sched/sync.h"
#include <paths.h>

void cmd_help(char *args) {
    (void)args;
    kprintf("Built-in commands:\n"
            "  cd [path]          change directory (default /)\n"
            "  pwd, tree [path]   where you are / what's below\n"
            "  mkdir, touch, rm   create a directory / empty file, remove one\n"
            "  df                 filesystem usage\n"
            "  ps, kill <pid>     list threads and processes / stop a process\n"
            "  clear, uptime, meminfo, memmap\n"
            "  sleep [ms], spin [s], demo   scheduler tests\n"
            "  crash <kind>       kernel fault test: pf gp de ud bp so ro nx df\n"
            "Running programs:\n"
            "  name args          run /usr/bin/name   (Ctrl+C stops it)\n"
            "  ... > file         output to a file (>> appends), < file for input\n"
            "  ... &              run in the background\n"
            "Programs in /usr/bin:\n ");

    struct vnode *dir;
    if (vfs_resolve(NULL, _PATH_BIN, &dir) == 0) {
        struct vnode *child;
        for (size_t i = 0; vfs_readdir(dir, i, &child) == 0; i++) {
            kprintf(" %s", child->name);
            vfs_put(child);
        }
        vfs_put(dir);
    }
    kprintf("\n");
}

void cmd_kill(char *args) {
    uint64_t pid = parse_u64(args, 0);
    if (pid == 0) {
        kprintf("usage: kill <pid>\n");
    } else if (!process_kill(pid)) {
        kprintf("kill: no process %lu\n", pid);
    }
}

void cmd_clear(char *args) {
    (void)args;
    console_clear();
}

void cmd_uptime(char *args) {
    (void)args;
    uint64_t ms = uptime_ms();
    kprintf("up %lu.%03lu s (%lu timer ticks)\n", ms / 1000, ms % 1000, timer_ticks());
}

void cmd_meminfo(char *args) {
    (void)args;
    struct heap_stats hs;
    heap_get_stats(&hs);
    uint64_t total = pmm_total_bytes(), free = pmm_free_bytes();
    kprintf("Physical: %lu KiB total, %lu KiB used, %lu KiB free\n",
            total / 1024, (total - free) / 1024, free / 1024);
    kprintf("Heap:     %lu KiB mapped, %lu bytes in %lu allocation(s), %lu free block(s)\n",
            hs.mapped_bytes / 1024, hs.used_bytes, hs.allocations, hs.free_blocks);
}

void cmd_memmap(char *args) {
    (void)args;
    size_t count;
    const struct mem_region *r = pmm_regions(&count);
    for (size_t i = 0; i < count; i++) {
        kprintf("  %016lx-%016lx %8lu KiB  %s\n", r[i].base, r[i].base + r[i].length,
                r[i].length / 1024, pmm_region_type_name(r[i].type));
    }
}

void cmd_ps(char *args) {
    (void)args;
    kprintf(" TID   PID  STATE     CPU(ms)  NAME\n");
    uint64_t irq = irq_save();   /* threads can't come and go while we walk the list */
    for (const struct thread *t = sched_all_threads(); t; t = t->all_next) {
        kprintf("%4lu  ", t->id);
        if (t->process) {
            kprintf("%4lu", t->process->pid);
        } else {
            kprintf("   -");   /* kernel thread */
        }
        kprintf("  %-8s %8lu  %s\n", thread_state_name(t->state),
                t->cpu_ticks * 1000 / timer_hz(), t->name);
    }
    irq_restore(irq);
}

void cmd_sleep(char *args) {
    uint64_t ms = parse_u64(args, 1000);
    sched_sleep_ms(ms);
    kprintf("slept %lu ms\n", ms);
}

/* `spin`: a thread that never sleeps, so only preemption lets others run. */
static void spin_thread(void *arg) {
    uint64_t end = uptime_ms() + (uint64_t)(uintptr_t)arg * 1000;
    uint64_t loops = 0;
    while (uptime_ms() < end) {
        loops++;
    }
    kprintf("[spin] done after %lu loops\n", loops);
}

void cmd_spin(char *args) {
    uint64_t seconds = parse_u64(args, 5);
    if (!thread_create("spin", spin_thread, (void *)(uintptr_t)seconds)) {
        kprintf("could not create thread\n");
        return;
    }
    kprintf("started a busy thread for %lu s; the shell should stay responsive\n", seconds);
}

/* `demo`: workers that sleep for different times and share a counter. */
static struct mutex demo_lock = MUTEX_INIT;
static uint64_t demo_counter;

static void demo_worker(void *arg) {
    uint64_t n = (uint64_t)(uintptr_t)arg;
    for (int step = 1; step <= 3; step++) {
        mutex_lock(&demo_lock);
        uint64_t value = ++demo_counter;
        mutex_unlock(&demo_lock);
        kprintf("[worker %lu] step %d, shared counter = %lu\n", n, step, value);
        sched_sleep_ms(200 * n);
    }
    kprintf("[worker %lu] exiting\n", n);
}

void cmd_demo(char *args) {
    (void)args;
    static const char *const names[] = { "worker-1", "worker-2", "worker-3" };
    for (uint64_t n = 1; n <= 3; n++) {
        if (!thread_create(names[n - 1], demo_worker, (void *)(uintptr_t)n)) {
            kprintf("could not create thread\n");
        }
    }
}

/* Never true at runtime; stops GCC from proving the recursion is infinite. */
static volatile int recursion_limit = 1 << 30;

__attribute__((noinline)) static int recurse(int depth) {
    volatile char pad[256];
    pad[0] = (char)depth;
    if (depth > recursion_limit) {
        return 0;
    }
    return recurse(depth + 1) + pad[0];
}

void cmd_crash(char *args) {
    char *kind;
    next_word(args, &kind);

    if (strcmp(kind, "pf") == 0) {
        volatile uint64_t *p = (volatile uint64_t *)0x0;  /* nothing is mapped at 0 */
        *p = 42;
    } else if (strcmp(kind, "gp") == 0) {
        volatile uint64_t *p = (volatile uint64_t *)0x8000000000000000ULL; /* non-canonical */
        (void)*p;
    } else if (strcmp(kind, "de") == 0) {
        /* Both operands volatile: GCC turns `1 / x` into compares, not a division. */
        volatile int one = 1, zero = 0;
        volatile int x = one / zero;
        (void)x;
    } else if (strcmp(kind, "ud") == 0) {
        __asm__ volatile ("ud2");
    } else if (strcmp(kind, "bp") == 0) {
        __asm__ volatile ("int3");
    } else if (strcmp(kind, "so") == 0) {      /* overflow the stack into its guard page */
        recurse(0);
    } else if (strcmp(kind, "ro") == 0) {      /* string literals live in read-only .rodata */
        volatile char *p = (volatile char *)"read-only";
        *p = 'X';
    } else if (strcmp(kind, "nx") == 0) {      /* .data is mapped no-execute */
        static uint8_t code[] = { 0xC3 };       /* ret */
        ((void (*)(void))(uintptr_t)code)();
    } else if (strcmp(kind, "df") == 0) {      /* heap double free, caught by kfree */
        void *p = kmalloc(32);
        kfree(p);
        kfree(p);
    } else {
        kprintf("usage: crash pf|gp|de|ud|bp|so|ro|nx|df\n");
    }
}
