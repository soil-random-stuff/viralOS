#include <stddef.h>

#include "sched/sync.h"
#include "sched/sched.h"
#include "arch/x86_64/cpu.h"
#include "drivers/timer.h"
#include "lib/panic.h"

bool wait_queue_sleep_timeout(struct wait_queue *wq, uint64_t timeout_ms) {
    struct thread *t = sched_current();
    t->next = NULL;
    if (wq->tail) {
        wq->tail->next = t;
    } else {
        wq->head = t;
    }
    wq->tail = t;
    t->waiting_on = wq;
    t->timed_out = false;
    sched_block_current(timeout_ms ? timer_ms_to_ticks(timeout_ms) : 0);
    return !t->timed_out;
}

void wait_queue_sleep(struct wait_queue *wq) {
    wait_queue_sleep_timeout(wq, 0);
}

void wait_queue_remove(struct wait_queue *wq, struct thread *t) {
    struct thread *prev = NULL;
    for (struct thread *c = wq->head; c; prev = c, c = c->next) {
        if (c != t) {
            continue;
        }
        if (prev) {
            prev->next = c->next;
        } else {
            wq->head = c->next;
        }
        if (wq->tail == c) {
            wq->tail = prev;
        }
        c->next = NULL;
        return;
    }
}

static struct thread *pop(struct wait_queue *wq) {
    struct thread *t = wq->head;
    if (t) {
        wq->head = t->next;
        if (!wq->head) {
            wq->tail = NULL;
        }
        t->waiting_on = NULL;
    }
    return t;
}

void wait_queue_wake_one(struct wait_queue *wq) {
    uint64_t irq = irq_save();
    struct thread *t = pop(wq);
    if (t) {
        sched_wake(t);
    }
    irq_restore(irq);
}

void wait_queue_wake_all(struct wait_queue *wq) {
    uint64_t irq = irq_save();
    struct thread *t;
    while ((t = pop(wq))) {
        sched_wake(t);
    }
    irq_restore(irq);
}

void mutex_lock(struct mutex *m) {
    uint64_t irq = irq_save();
    struct thread *self = sched_current();
    if (m->owner == self) {
        panic("mutex_lock: thread '%s' already holds this mutex", self->name);
    }
    /* Loop: another thread may grab the mutex between our wakeup and running. */
    while (m->owner) {
        wait_queue_sleep(&m->waiters);
    }
    m->owner = self;
    irq_restore(irq);
}

void mutex_unlock(struct mutex *m) {
    uint64_t irq = irq_save();
    if (m->owner != sched_current()) {
        panic("mutex_unlock: mutex not held by the current thread");
    }
    m->owner = NULL;
    wait_queue_wake_one(&m->waiters);
    irq_restore(irq);
}
