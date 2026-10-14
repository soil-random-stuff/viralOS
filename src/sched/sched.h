#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wait_queue;

enum thread_state {
    THREAD_READY,      /* on the run queue */
    THREAD_RUNNING,
    THREAD_SLEEPING,   /* waiting for a tick count */
    THREAD_BLOCKED,    /* waiting on a wait queue */
    THREAD_DEAD,       /* exited, stack not freed yet */
};

struct thread {
    uint64_t id;
    char name[24];
    enum thread_state state;

    uint64_t rsp;            /* saved stack pointer while not running */
    uint64_t stack_top;
    size_t stack_size;

    uint64_t wake_tick;      /* deadline, if has_deadline */
    bool has_deadline;       /* on the timeout list (sleeping, or a timed wait) */
    struct thread *sleep_next;
    struct wait_queue *waiting_on;   /* blocked on this queue (with or without a timeout) */
    bool timed_out;          /* the last timed wait ran out */
    uint64_t cpu_ticks;      /* timer ticks spent running */
    int net_errno;           /* lwIP's errno while this thread is in a socket call */
    uint32_t slice_left;     /* ticks left before preemption */

    struct process *process; /* NULL for kernel threads */
    uint64_t space;          /* page tables (CR3) to run with; 0 = kernel only */
    void *fpu_state;         /* FXSAVE area for user threads; NULL for kernel threads */
    uint64_t fs_base;        /* the FS segment base (thread-local storage of Linux programs) */

    struct thread *next;     /* run queue, wait queue or sleep list */
    struct thread *all_next; /* list of every thread */
};

struct process;

typedef void (*thread_fn)(void *arg);

/*
 * Turn the code that is currently running into thread 1 ("kmain") on the
 * given stack, and create the idle thread. Call with interrupts disabled.
 */
void sched_init(uint64_t current_stack_top, size_t current_stack_size);

struct thread *thread_create(const char *name, thread_fn fn, void *arg);
/* Like thread_create, but the thread belongs to `process` and runs in `space`. */
struct thread *thread_create_in(const char *name, thread_fn fn, void *arg,
                                struct process *process, uint64_t space);
__attribute__((noreturn)) void thread_exit(void);

void sched_yield(void);
void sched_sleep_ms(uint64_t ms);

struct thread *sched_current(void);
const struct thread *sched_all_threads(void);
const char *thread_state_name(enum thread_state state);

struct cpu;
/* An application processor joins: what it runs now becomes its idle thread. */
void sched_init_cpu(struct cpu *cpu);
/* The idle loop (for the idle threads): never returns. */
__attribute__((noreturn)) void sched_idle(void);
/* Time the running CPUs have spent idle since they started, added up (ms). */
uint64_t sched_idle_ms(void);

/* Called from the timer interrupt. */
void sched_tick(void);
/* Called at the end of every interrupt: switches threads if one is due. */
void sched_preempt(void);

/* Low-level pieces for wait queues (sync.c). Interrupts must be disabled.
   A non-zero timeout puts the thread back on the run queue after that many ticks. */
void sched_block_current(uint64_t timeout_ticks);
void sched_wake(struct thread *t);
