/*
 * lwIP's operating system layer (lwip/sys.h) on the kernel's threads and wait queues, after the
 * reference ports in lwip/contrib/ports (unix sys_arch.c): counting semaphores, fixed-size mailboxes
 * of pointers, threads, the clock, and "protection" = interrupts off.
 *
 * Programs block in here inside socket calls (waiting for data, a connection...). Waits for data
 * (mailbox fetches) give up when the program is killed or stopped with Ctrl+C, so it isn't stuck in
 * recv() forever; lwIP then reports a timeout. Semaphore waits always run to the end: they wait for
 * the tcpip thread to finish something that refers to the caller's stack.
 */

#include "lwip/opt.h"
#include "lwip/sys.h"

#include "arch/x86_64/cpu.h"
#include "drivers/timer.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/process.h"
#include "sched/sched.h"

/* How often a program blocked in a mailbox wait checks whether it was killed. */
#define KILL_CHECK_MS 100

void sys_init(void) {
}

u32_t sys_now(void) {
    return (u32_t)uptime_ms();
}

sys_prot_t sys_arch_protect(void) {
    return irq_save();
}

void sys_arch_unprotect(sys_prot_t pval) {
    irq_restore(pval);
}

void *net_calloc(size_t count, size_t size) {
    return kzalloc(count * size);
}

int *net_errno_location(void) {
    return &sched_current()->net_errno;
}

uint32_t net_random(void) {
    static uint64_t state;
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    state ^= ((uint64_t)hi << 32 | lo) + 0x9E3779B97F4A7C15ull;
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return (uint32_t)(state >> 16);
}

/* --- threads ------------------------------------------------------------------------------------- */

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn fn, void *arg, int stacksize, int prio) {
    (void)stacksize;
    (void)prio;
    struct thread *t = thread_create(name, fn, arg);
    if (!t) {
        panic("lwip: can't start the %s thread", name);
    }
    return t;
}

/* --- semaphores ---------------------------------------------------------------------------------- */

err_t sys_sem_new(sys_sem_t *sem, u8_t count) {
    memset(sem, 0, sizeof(*sem));
    sem->count = count;
    sem->valid = true;
    return ERR_OK;
}

void sys_sem_free(sys_sem_t *sem) {
    sem->valid = false;
}

void sys_sem_signal(sys_sem_t *sem) {
    uint64_t irq = irq_save();
    sem->count++;
    wait_queue_wake_one(&sem->waiters);
    irq_restore(irq);
}

/* Returns how long it waited (ms), or SYS_ARCH_TIMEOUT. */
u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout) {
    uint64_t start = uptime_ms();
    uint64_t irq = irq_save();
    while (sem->count == 0) {
        if (timeout == 0) {
            wait_queue_sleep(&sem->waiters);
            continue;
        }
        uint64_t waited = uptime_ms() - start;
        if (waited >= timeout || !wait_queue_sleep_timeout(&sem->waiters, timeout - waited)) {
            if (sem->count == 0) {
                irq_restore(irq);
                return SYS_ARCH_TIMEOUT;
            }
        }
    }
    sem->count--;
    irq_restore(irq);
    return (u32_t)(uptime_ms() - start);
}

/* --- mailboxes ----------------------------------------------------------------------------------- */

err_t sys_mbox_new(sys_mbox_t *mbox, int size) {
    memset(mbox, 0, sizeof(*mbox));
    if (size <= 0) {
        size = 64;
    }
    mbox->slots = kmalloc((size_t)size * sizeof(void *));
    if (!mbox->slots) {
        return ERR_MEM;
    }
    mbox->size = size;
    mbox->valid = true;
    return ERR_OK;
}

void sys_mbox_free(sys_mbox_t *mbox) {
    kfree(mbox->slots);
    mbox->slots = NULL;
    mbox->valid = false;
}

static void put(sys_mbox_t *mbox, void *msg) {
    mbox->slots[(mbox->head + mbox->count) % mbox->size] = msg;
    mbox->count++;
    wait_queue_wake_one(&mbox->not_empty);
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg) {
    uint64_t irq = irq_save();
    while (mbox->count == mbox->size) {
        wait_queue_sleep(&mbox->not_full);
    }
    put(mbox, msg);
    irq_restore(irq);
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg) {
    uint64_t irq = irq_save();
    err_t err = ERR_MEM;
    if (mbox->count < mbox->size) {
        put(mbox, msg);
        err = ERR_OK;
    }
    irq_restore(irq);
    return err;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg) {
    return sys_mbox_trypost(mbox, msg);
}

static void *take(sys_mbox_t *mbox) {
    void *msg = mbox->slots[mbox->head];
    mbox->head = (mbox->head + 1) % mbox->size;
    mbox->count--;
    wait_queue_wake_one(&mbox->not_full);
    return msg;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout) {
    uint64_t start = uptime_ms();
    uint64_t irq = irq_save();
    while (mbox->count == 0) {
        /* A program waiting for data: let Ctrl+C or kill end the wait (as a timeout). */
        if (process_current_killed()) {
            irq_restore(irq);
            return SYS_ARCH_TIMEOUT;
        }
        uint64_t slice = sched_current()->process ? KILL_CHECK_MS : 0;
        if (timeout) {
            uint64_t waited = uptime_ms() - start;
            if (waited >= timeout) {
                irq_restore(irq);
                return SYS_ARCH_TIMEOUT;
            }
            if (!slice || timeout - waited < slice) {
                slice = timeout - waited;
            }
        }
        if (slice) {
            wait_queue_sleep_timeout(&mbox->not_empty, slice);
        } else {
            wait_queue_sleep(&mbox->not_empty);
        }
    }
    void *m = take(mbox);
    if (msg) {
        *msg = m;
    }
    irq_restore(irq);
    return (u32_t)(uptime_ms() - start);
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg) {
    uint64_t irq = irq_save();
    if (mbox->count == 0) {
        irq_restore(irq);
        return SYS_MBOX_EMPTY;
    }
    void *m = take(mbox);
    if (msg) {
        *msg = m;
    }
    irq_restore(irq);
    return 0;
}
