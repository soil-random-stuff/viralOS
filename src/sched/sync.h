#pragma once

#include <stdbool.h>
#include <stdint.h>

struct thread;

/* A list of threads blocked until someone wakes them. */
struct wait_queue {
    struct thread *head, *tail;
};

#define WAIT_QUEUE_INIT { 0, 0 }

/* Block the current thread on `wq`. Call with interrupts disabled (irq_save). */
void wait_queue_sleep(struct wait_queue *wq);
/* Same, but give up after timeout_ms (0 = never). Returns false if it timed out. */
bool wait_queue_sleep_timeout(struct wait_queue *wq, uint64_t timeout_ms);
/* Take a thread off the queue without waking it. Interrupts must be disabled. */
void wait_queue_remove(struct wait_queue *wq, struct thread *t);
/* Safe to call from interrupt handlers. */
void wait_queue_wake_one(struct wait_queue *wq);
void wait_queue_wake_all(struct wait_queue *wq);

/* Sleeping lock: waiters block instead of spinning. Not for interrupt handlers. */
struct mutex {
    struct thread *owner;
    struct wait_queue waiters;
};

#define MUTEX_INIT { 0, WAIT_QUEUE_INIT }

void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
