#include <stdbool.h>

#include "drivers/input.h"
#include "arch/x86_64/cpu.h"
#include "drivers/timer.h"
#include "ipc/ipc.h"
#include "lib/errno.h"
#include "lib/string.h"
#include "proc/process.h"
#include "sched/sync.h"

#define QUEUE_SIZE 256

static struct os_input_event queue[QUEUE_SIZE];
static uint32_t head, tail;   /* tail: oldest unread, head: next free */
static struct wait_queue readers = WAIT_QUEUE_INIT;
static uint64_t owner;

void input_set_owner(uint64_t pid) {
    uint64_t irq = irq_save();
    owner = pid;
    head = tail = 0;   /* a new owner starts with an empty queue */
    irq_restore(irq);
}

uint64_t input_owner(void) {
    return owner;
}

void input_wake_readers(void) {
    wait_queue_wake_all(&readers);
}

static bool empty(void) {
    return head == tail;
}

void input_push(struct os_input_event *event) {
    uint64_t irq = irq_save();
    if (!owner) {
        irq_restore(irq);
        return;
    }
    event->time_ms = uptime_ms();

    /* A mouse move right behind an unread one with the same buttons just
       replaces it (adding up the movement): no point queueing every pixel. */
    if (event->type == OS_EVENT_MOUSE && !empty() && event->wheel == 0) {
        struct os_input_event *last = &queue[(head + QUEUE_SIZE - 1) % QUEUE_SIZE];
        if (last->type == OS_EVENT_MOUSE && last->buttons == event->buttons && last->wheel == 0) {
            int32_t dx = last->dx + event->dx, dy = last->dy + event->dy;
            *last = *event;
            last->dx = dx;
            last->dy = dy;
            irq_restore(irq);
            return;
        }
    }

    uint32_t next = (head + 1) % QUEUE_SIZE;
    if (next == tail) {
        tail = (tail + 1) % QUEUE_SIZE;   /* full: drop the oldest */
    }
    queue[head] = *event;
    head = next;
    wait_queue_wake_all(&readers);
    ipc_notify();   /* someone may be poll()ing OS_FD_INPUT */
    irq_restore(irq);
}

bool input_pending(void) {
    return !empty();
}

int input_read(struct os_input_event *out, int max, int64_t timeout_ms) {
    uint64_t irq = irq_save();
    uint64_t deadline = timeout_ms > 0 ? uptime_ms() + (uint64_t)timeout_ms : 0;
    while (empty()) {
        if (timeout_ms == 0 || process_current_killed()) {
            irq_restore(irq);
            return process_current_killed() ? -EINTR : 0;
        }
        if (timeout_ms < 0) {
            wait_queue_sleep(&readers);
            continue;
        }
        uint64_t now = uptime_ms();
        if (now >= deadline || !wait_queue_sleep_timeout(&readers, deadline - now)) {
            if (empty()) {
                irq_restore(irq);
                return 0;
            }
        }
    }
    int n = 0;
    while (n < max && !empty()) {
        out[n++] = queue[tail];
        tail = (tail + 1) % QUEUE_SIZE;
    }
    irq_restore(irq);
    return n;
}
