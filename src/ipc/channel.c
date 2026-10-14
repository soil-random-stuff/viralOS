/*
 * Message channels: two ends, each with a queue of messages sent to it by
 * the other end. Messages keep their boundaries and arrive in order. When
 * one end closes, the other still gets what was already sent, then 0-byte
 * "closed" reads.
 */

#include "ipc/ipc.h"
#include "arch/x86_64/cpu.h"
#include "lib/errno.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/file.h"
#include "proc/process.h"

#define QUEUE_MAX 128   /* messages waiting at one end before senders have to wait */

struct message {
    struct message *next;
    struct file *attached;
    uint32_t len;
    uint8_t data[];
};

struct queue {
    struct message *head, *tail;
    uint32_t count;
};

struct channel {
    struct queue incoming[2];   /* incoming[i]: messages for end i */
    bool closed[2];
};

int channel_pair(struct file **a, struct file **b) {
    struct channel *c = kzalloc(sizeof(*c));
    struct file *fa = file_new(FILE_CHANNEL), *fb = file_new(FILE_CHANNEL);
    if (!c || !fa || !fb) {
        kfree(c);
        kfree(fa);
        kfree(fb);
        return -ENOMEM;
    }
    fa->object = fb->object = c;
    fa->end = 0;
    fb->end = 1;
    *a = fa;
    *b = fb;
    return 0;
}

int channel_send(struct file *f, const void *data, size_t len, struct file *attach, bool wait) {
    if (len == 0 || len > OS_MSG_MAX) {
        return len ? -EMSGSIZE : -EINVAL;   /* empty messages would look like "closed" */
    }
    struct message *m = kmalloc(sizeof(*m) + len);
    if (!m) {
        return -ENOMEM;
    }
    memcpy(m->data, data, len);
    m->len = (uint32_t)len;
    m->attached = attach;
    m->next = NULL;

    struct channel *c = f->object;
    int peer = 1 - f->end;
    uint64_t irq = irq_save();
    int err = 0;
    for (;;) {
        if (c->closed[peer]) {
            err = -EPIPE;
            break;
        }
        if (c->incoming[peer].count < QUEUE_MAX) {
            break;
        }
        if (!wait) {
            err = -EAGAIN;
            break;
        }
        if (process_current_killed()) {
            err = -EINTR;
            break;
        }
        ipc_wait(0);   /* full: wait for the receiver to catch up */
    }
    if (!err) {
        struct queue *q = &c->incoming[peer];
        if (q->tail) {
            q->tail->next = m;
        } else {
            q->head = m;
        }
        q->tail = m;
        q->count++;
        ipc_notify();
    }
    irq_restore(irq);
    if (err) {
        kfree(m);   /* the caller still owns `attach` */
    }
    return err;
}

int64_t channel_recv(struct file *f, void *buf, size_t size, struct file **attached) {
    struct channel *c = f->object;
    int me = f->end;
    struct queue *q = &c->incoming[me];
    uint64_t irq = irq_save();
    for (;;) {
        if (q->head) {
            break;
        }
        if (c->closed[1 - me]) {
            irq_restore(irq);
            return 0;   /* nothing left, and nothing more will come */
        }
        if (process_current_killed()) {
            irq_restore(irq);
            return -EINTR;
        }
        ipc_wait(0);
    }
    struct message *m = q->head;
    if (m->len > size) {
        irq_restore(irq);
        return -EMSGSIZE;   /* left in the queue: try again with a bigger buffer */
    }
    q->head = m->next;
    if (!q->head) {
        q->tail = NULL;
    }
    q->count--;
    ipc_notify();   /* a blocked sender may have room now */
    irq_restore(irq);

    memcpy(buf, m->data, m->len);
    *attached = m->attached;
    int64_t len = m->len;
    kfree(m);
    return len;
}

static void drain(struct queue *q) {
    struct message *m = q->head;
    q->head = q->tail = NULL;
    q->count = 0;
    while (m) {
        struct message *next = m->next;
        if (m->attached) {
            file_put(m->attached);
        }
        kfree(m);
        m = next;
    }
}

void channel_close(struct channel *c, int end) {
    uint64_t irq = irq_save();
    c->closed[end] = true;
    /* Messages for this end will never be read now. (Ones it sent stay
       readable by the other end.) */
    drain(&c->incoming[end]);
    bool both = c->closed[0] && c->closed[1];
    if (both) {
        drain(&c->incoming[1 - end]);
        kfree(c);
    }
    ipc_notify();
    irq_restore(irq);
}

uint16_t channel_poll(struct file *f) {
    struct channel *c = f->object;
    int me = f->end;
    uint16_t r = c->incoming[me].head ? OS_POLL_IN : 0;
    if (c->closed[1 - me]) {
        r |= OS_POLL_HUP | OS_POLL_IN;   /* a read returns at once (with 0) */
    }
    return r;
}
