#include "ipc/ipc.h"
#include "arch/x86_64/cpu.h"
#include "drivers/input.h"
#include "drivers/timer.h"
#include "lib/errno.h"
#include "proc/file.h"
#include "proc/process.h"
#include "proc/tty.h"
#include "net/net.h"
#include "sched/sync.h"

static struct wait_queue activity = WAIT_QUEUE_INIT;

void ipc_notify(void) {
    wait_queue_wake_all(&activity);
}

bool ipc_wait(uint64_t timeout_ms) {
    return wait_queue_sleep_timeout(&activity, timeout_ms);
}

static uint16_t readiness(struct process *p, int32_t fd) {
    if (fd == OS_FD_INPUT) {
        return input_owner() == p->pid && input_pending() ? OS_POLL_IN : 0;
    }
    struct file *f = fd >= 0 && fd < PROCESS_MAX_FILES ? p->files[fd] : NULL;
    if (!f) {
        return OS_POLL_HUP;   /* not open: nothing will ever come */
    }
    switch (f->kind) {
    case FILE_CHANNEL:  return channel_poll(f);
    case FILE_LISTENER: return service_poll(f);
    case FILE_PTY:        return pty_slave_poll(f->object);
    case FILE_PTY_MASTER: return pty_master_poll(f->object);
    case FILE_SOCKET:     return net_socket_poll(f);
    default:            return OS_POLL_IN;   /* files and the console: just read */
    }
}

int ipc_poll(struct process *p, struct os_pollfd *fds, int count, int64_t timeout_ms) {
    uint64_t irq = irq_save();
    uint64_t deadline = timeout_ms > 0 ? uptime_ms() + (uint64_t)timeout_ms : 0;
    int ready;
    for (;;) {
        ready = 0;
        for (int i = 0; i < count; i++) {
            uint16_t r = readiness(p, fds[i].fd);
            fds[i].revents = r & (fds[i].events | OS_POLL_HUP | OS_POLL_ERR);
            ready += fds[i].revents != 0;
        }
        if (ready || timeout_ms == 0) {
            break;
        }
        if (process_current_killed()) {
            ready = -EINTR;
            break;
        }
        if (timeout_ms < 0) {
            ipc_wait(0);
            continue;
        }
        uint64_t now = uptime_ms();
        if (now >= deadline) {
            break;
        }
        ipc_wait(deadline - now);
    }
    irq_restore(irq);
    return ready;
}
