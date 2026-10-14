#pragma once

/* lwIP's operating system glue (see lwip/sys.h; the code is in net/port/sys_arch.c). */

#include <stdbool.h>
#include <stdint.h>

#include "sched/sync.h"

struct sys_sem {
    uint32_t count;
    struct wait_queue waiters;
    bool valid;
};
typedef struct sys_sem sys_sem_t;

struct sys_mbox {
    void **slots;
    int size, head, count;
    struct wait_queue not_empty, not_full;
    bool valid;
};
typedef struct sys_mbox sys_mbox_t;

typedef struct thread *sys_thread_t;

/* sys_arch_protect: interrupts off on this CPU (the big kernel lock keeps the other CPUs out). */
typedef uint64_t sys_prot_t;

#define sys_sem_valid(s)          ((s)->valid)
#define sys_sem_set_invalid(s)    ((s)->valid = false)
#define sys_mbox_valid(m)         ((m)->valid)
#define sys_mbox_set_invalid(m)   ((m)->valid = false)
