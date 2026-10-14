/*
 * Preemptive round-robin scheduler for kernel threads, on every CPU.
 *
 * Locking: the scheduler's lists are only touched with interrupts disabled
 * by a CPU holding the big kernel lock (smp.h), so nothing else touches them
 * meanwhile. There's one run queue: any CPU takes the next ready thread.
 * Each CPU has its own current thread and its own idle thread.
 *
 * A thread gives up the CPU only inside schedule(): voluntarily (yield,
 * sleep, block, exit) or at the end of an interrupt when its time slice is
 * used up or a sleeping/blocked thread was woken while the CPU was idle.
 */

#include "sched/sched.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/fpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/syscall.h"
#include "proc/process.h"
#include "drivers/timer.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "sched/sync.h"

#define THREAD_STACK_SIZE (64 * 1024)
#define TIME_SLICE_TICKS  2

/* context.S */
void context_switch(uint64_t *save_rsp, uint64_t load_rsp);
void thread_trampoline(void);

static struct thread *all_threads;
static struct thread *run_head, *run_tail;
static struct thread *sleepers;
static struct thread *zombies;
static uint64_t next_id;



/* --- run queue ------------------------------------------------------------------ */

static void enqueue(struct thread *t) {
    t->next = NULL;
    if (run_tail) {
        run_tail->next = t;
    } else {
        run_head = t;
    }
    run_tail = t;
}

static struct thread *dequeue(void) {
    struct thread *t = run_head;
    if (t) {
        run_head = t->next;
        if (!run_head) {
            run_tail = NULL;
        }
        t->next = NULL;
    }
    return t;
}

/* --- thread bookkeeping --------------------------------------------------------- */

static struct thread *thread_alloc(const char *name) {
    struct thread *t = kzalloc(sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->id = next_id++;
    size_t i = 0;
    for (; name[i] && i < sizeof(t->name) - 1; i++) {
        t->name[i] = name[i];
    }
    t->name[i] = '\0';
    return t;
}

static void link_thread(struct thread *t) {
    t->all_next = all_threads;
    all_threads = t;
}

static void unlink_thread(struct thread *t) {
    for (struct thread **p = &all_threads; *p; p = &(*p)->all_next) {
        if (*p == t) {
            *p = t->all_next;
            return;
        }
    }
}

/* Free threads that have exited. Never the current one: we'd be on its stack. */
static void reap_zombies(void) {
    struct thread **p = &zombies;
    while (*p) {
        struct thread *z = *p;
        if (z == this_cpu()->current) {
            p = &z->next;
            continue;
        }
        *p = z->next;
        unlink_thread(z);
        vmm_free_kernel_stack(z->stack_top, z->stack_size);
        if (z->process) {
            process_thread_reaped(z->process);   /* may free its address space */
        }
        fpu_state_free(z->fpu_state);
        kfree(z);
    }
}

/* --- the scheduler ----------------------------------------------------------------- */

static void schedule(void) {
    struct cpu *cpu = this_cpu();
    struct thread *prev = cpu->current;
    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        if (prev != cpu->idle) {
            enqueue(prev);
        }
    }

    struct thread *next = dequeue();
    if (!next) {
        next = cpu->idle;
    }
    next->state = THREAD_RUNNING;
    next->slice_left = TIME_SLICE_TICKS;
    cpu->need_resched = false;

    if (next != prev) {
        cpu->current = next;
        /* Where the CPU puts us when this thread enters the kernel from user mode. */
        gdt_set_kernel_stack(next->stack_top);
        syscall_set_kernel_stack(next->stack_top);

        uint64_t space = next->space ? next->space : vmm_kernel_space();
        if (space != cpu->active_space) {
            write_cr3(space);
            cpu->active_space = space;
        }

        /* The FPU/SSE registers still hold prev's values (the kernel never
           touches them), so park them and load next's. */
        if (prev->fpu_state) {
            fpu_save(prev->fpu_state);
        }
        if (next->fpu_state) {
            fpu_restore(next->fpu_state);
        }
        /* The kernel never uses FS: only programs that set it (Linux ones, for TLS) need it back. */
        if (next->fs_base != cpu->loaded_fs_base) {
            wrmsr(MSR_FS_BASE, next->fs_base);
            cpu->loaded_fs_base = next->fs_base;
        }
        context_switch(&prev->rsp, next->rsp);
        /* (Possibly on another CPU now: `cpu` is stale from here on.) */
        /* ...some time later, `prev` is scheduled again and resumes here. */
    }
    reap_zombies();
}

/* --- timeouts -------------------------------------------------------------------------
 *
 * `sleepers` holds every thread with a deadline: plain sleeps, and threads
 * blocked on a wait queue with a timeout. It has its own link (sleep_next)
 * because a timed waiter is on a wait queue at the same time.
 */

static void sleepers_add(struct thread *t, uint64_t wake_tick) {
    t->wake_tick = wake_tick;
    t->sleep_next = sleepers;
    sleepers = t;
    t->has_deadline = true;
}

static void sleepers_remove(struct thread *t) {
    for (struct thread **p = &sleepers; *p; p = &(*p)->sleep_next) {
        if (*p == t) {
            *p = t->sleep_next;
            break;
        }
    }
    t->has_deadline = false;
}

void sched_tick(void) {
    struct cpu *cpu = this_cpu();
    uint64_t now = timer_ticks();
    cpu->ticks++;

    /* Deadlines are the BSP's job (the others would only find the same ones). */
    for (struct thread *t = cpu->index == 0 ? sleepers : NULL, *next; t; t = next) {
        next = t->sleep_next;
        if (t->wake_tick > now) {
            continue;
        }
        if (t->state == THREAD_BLOCKED && t->waiting_on) {
            /* Timed out before anyone woke it: leave the wait queue empty-handed. */
            wait_queue_remove(t->waiting_on, t);
            t->waiting_on = NULL;
            t->timed_out = true;
        }
        sched_wake(t);   /* also takes it off `sleepers` */
    }

    struct thread *cur = cpu->current;
    if (!cur) {
        return;
    }
    cur->cpu_ticks++;
    if (cur != cpu->idle && cur->slice_left > 0 && --cur->slice_left == 0) {
        cpu->need_resched = true;
    }
}

void sched_preempt(void) {
    struct cpu *cpu = this_cpu();
    if (cpu->need_resched && cpu->current) {
        schedule();
    }
}

void sched_wake(struct thread *t) {
    if (t->has_deadline) {
        sleepers_remove(t);
    }
    t->state = THREAD_READY;
    enqueue(t);
    /* Someone should run it: this CPU if it's idle, else another idle one (woken by an interrupt). */
    struct cpu *cpu = this_cpu();
    if (cpu->current == cpu->idle) {
        cpu->need_resched = true;   /* don't let the CPU sit in hlt */
        return;
    }
    for (unsigned i = 0; i < MAX_CPUS; i++) {
        struct cpu *other = smp_cpu(i);
        if (other && other != cpu && other->online && other->current == other->idle && !other->need_resched) {
            other->need_resched = true;
            smp_kick(other);
            return;
        }
    }
}

void sched_block_current(uint64_t timeout_ticks) {
    this_cpu()->current->state = THREAD_BLOCKED;
    if (timeout_ticks) {
        sleepers_add(this_cpu()->current, timer_ticks() + timeout_ticks);
    }
    schedule();
}

/* --- public API ------------------------------------------------------------------ */

void thread_entry(thread_fn fn, void *arg);

/* What a CPU does with nothing to run: lets go of the kernel lock and sleeps until an interrupt.
   (Interrupts take the lock and, if there's work, switch to it; see interrupt_dispatch.) */
void sched_idle(void) {
    interrupts_disable();
    kernel_unlock();
    for (;;) {
        wait_for_interrupt();
    }
}

static void idle_loop(void *arg) {
    (void)arg;
    sched_idle();
}

uint64_t sched_idle_ms(void) {
    uint64_t ticks = 0;
    for (unsigned i = 0; smp_cpu(i); i++) {
        struct cpu *c = smp_cpu(i);
        if (c->online && c->idle) {
            ticks += c->idle->cpu_ticks;
        }
    }
    return ticks * 1000 / timer_hz();
}

/* Allocate a thread with a stack set up to start running fn(arg). */
static struct thread *thread_new(const char *name, thread_fn fn, void *arg) {
    struct thread *t = thread_alloc(name);
    if (!t) {
        return NULL;
    }
    t->stack_size = THREAD_STACK_SIZE;
    t->stack_top = vmm_alloc_kernel_stack(t->stack_size);
    if (!t->stack_top) {
        kfree(t);
        return NULL;
    }

    /*
     * Fake the frame context_switch() expects to pop: six callee-saved
     * registers, then a return address pointing at thread_trampoline.
     * rbp = 0 ends backtraces here.
     */
    uint64_t *sp = (uint64_t *)t->stack_top;
    *--sp = (uint64_t)thread_trampoline;  /* ret target */
    *--sp = 0;                            /* rbx */
    *--sp = 0;                            /* rbp */
    *--sp = (uint64_t)fn;                 /* r12 */
    *--sp = (uint64_t)arg;                /* r13 */
    *--sp = 0;                            /* r14 */
    *--sp = 0;                            /* r15 */
    t->rsp = (uint64_t)sp;
    t->state = THREAD_READY;
    return t;
}

void sched_init(uint64_t current_stack_top, size_t current_stack_size) {
    struct cpu *cpu = this_cpu();
    /* The idle thread (id 0) is never queued: schedule() falls back to it. */
    struct thread *idle = thread_new("idle0", idle_loop, NULL);
    struct thread *boot = thread_alloc("kmain");
    if (!idle || !boot) {
        panic("sched: out of memory");
    }
    link_thread(idle);
    cpu->idle = idle;

    boot->state = THREAD_RUNNING;
    boot->stack_top = current_stack_top;
    boot->stack_size = current_stack_size;
    boot->slice_left = TIME_SLICE_TICKS;
    link_thread(boot);
    cpu->current = boot;
    cpu->active_space = vmm_kernel_space();
    syscall_set_kernel_stack(current_stack_top);
}

void sched_init_cpu(struct cpu *cpu) {
    /* What this CPU is running now (its boot stack) becomes its idle thread. */
    char name[16] = "idle";
    uint32_t n = cpu->index, len = 4;
    char digits[8];
    int k = 0;
    do {
        digits[k++] = (char)('0' + n % 10);
        n /= 10;
    } while (n && k < 7);
    while (k && len < sizeof(name) - 1) {
        name[len++] = digits[--k];
    }
    name[len] = '\0';
    struct thread *idle = thread_alloc(name);
    if (!idle) {
        panic("sched: out of memory");
    }
    idle->state = THREAD_RUNNING;
    idle->stack_top = cpu->kernel_rsp;
    idle->stack_size = 64 * 1024;
    link_thread(idle);
    cpu->idle = cpu->current = idle;
    cpu->active_space = read_cr3();
    gdt_set_kernel_stack(cpu->kernel_rsp);
}

/* C entry point of every new thread; see thread_trampoline in context.S. */
__attribute__((noreturn)) void thread_entry(thread_fn fn, void *arg) {
    /* We arrive here from schedule(), with interrupts still disabled. */
    interrupts_enable();
    fn(arg);
    thread_exit();
}

struct thread *thread_create(const char *name, thread_fn fn, void *arg) {
    return thread_create_in(name, fn, arg, NULL, 0);
}

struct thread *thread_create_in(const char *name, thread_fn fn, void *arg,
                                struct process *process, uint64_t space) {
    struct thread *t = thread_new(name, fn, arg);
    if (t && process) {
        /* User threads get FPU/SSE/AVX registers of their own. */
        t->fpu_state = fpu_state_alloc();
        if (!t->fpu_state) {
            vmm_free_kernel_stack(t->stack_top, t->stack_size);
            kfree(t);
            return NULL;
        }
    }
    if (t) {
        t->process = process;
        t->space = space;
        uint64_t irq = irq_save();
        link_thread(t);
        sched_wake(t);
        irq_restore(irq);
    }
    return t;
}

void thread_exit(void) {
    interrupts_disable();
    this_cpu()->current->state = THREAD_DEAD;
    this_cpu()->current->next = zombies;
    zombies = this_cpu()->current;
    schedule();
    panic("thread_exit: dead thread was scheduled again");
}

void sched_yield(void) {
    uint64_t irq = irq_save();
    schedule();
    irq_restore(irq);
}

void sched_sleep_ms(uint64_t ms) {
    uint64_t irq = irq_save();
    this_cpu()->current->state = THREAD_SLEEPING;
    sleepers_add(this_cpu()->current, timer_ticks() + timer_ms_to_ticks(ms));
    schedule();
    irq_restore(irq);
}

struct thread *sched_current(void) {
    return this_cpu()->current;
}

const struct thread *sched_all_threads(void) {
    return all_threads;
}

const char *thread_state_name(enum thread_state state) {
    switch (state) {
    case THREAD_READY:    return "ready";
    case THREAD_RUNNING:  return "running";
    case THREAD_SLEEPING: return "sleeping";
    case THREAD_BLOCKED:  return "blocked";
    case THREAD_DEAD:     return "dead";
    }
    return "?";
}
