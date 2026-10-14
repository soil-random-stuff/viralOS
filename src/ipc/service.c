/*
 * Named services: the meeting point for channels. A server registers a
 * name; each connect creates a fresh channel, handing one end to the client
 * and queueing the other for the server's accept.
 */

#include "ipc/ipc.h"
#include "arch/x86_64/cpu.h"
#include "lib/errno.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/file.h"
#include "proc/process.h"

#define PENDING_MAX 16

struct listener {
    char name[OS_SERVICE_NAME_MAX];
    struct file *pending[PENDING_MAX];   /* server ends of channels not yet accepted */
    int pending_count;
    struct listener *next;
};

static struct listener *services;

static struct listener *find(const char *name) {
    for (struct listener *l = services; l; l = l->next) {
        if (strcmp(l->name, name) == 0) {
            return l;
        }
    }
    return NULL;
}

static bool valid_name(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len >= OS_SERVICE_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

int service_register(const char *name, struct file **out) {
    if (!valid_name(name)) {
        return -EINVAL;
    }
    struct listener *l = kzalloc(sizeof(*l));
    struct file *f = file_new(FILE_LISTENER);
    if (!l || !f) {
        kfree(l);
        kfree(f);
        return -ENOMEM;
    }
    strlcpy(l->name, name, sizeof(l->name));
    f->object = l;

    uint64_t irq = irq_save();
    if (find(name)) {
        irq_restore(irq);
        kfree(l);
        kfree(f);
        return -EEXIST;
    }
    l->next = services;
    services = l;
    irq_restore(irq);
    *out = f;
    return 0;
}

int service_connect(const char *name, struct file **out) {
    struct file *client, *server;
    int err = channel_pair(&client, &server);
    if (err) {
        return err;
    }
    uint64_t irq = irq_save();
    struct listener *l = find(name);
    if (!l) {
        err = -ECONNREFUSED;
    } else if (l->pending_count == PENDING_MAX) {
        err = -EAGAIN;   /* the server isn't accepting fast enough */
    } else {
        l->pending[l->pending_count++] = server;
        ipc_notify();
    }
    irq_restore(irq);
    if (err) {
        file_put(client);
        file_put(server);
        return err;
    }
    *out = client;
    return 0;
}

int service_accept(struct file *f, struct file **out) {
    struct listener *l = f->object;
    uint64_t irq = irq_save();
    while (l->pending_count == 0) {
        if (process_current_killed()) {
            irq_restore(irq);
            return -EINTR;
        }
        ipc_wait(0);
    }
    *out = l->pending[0];
    l->pending_count--;
    memmove(l->pending, l->pending + 1, sizeof(l->pending[0]) * (size_t)l->pending_count);
    irq_restore(irq);
    return 0;
}

void service_close(struct listener *l) {
    uint64_t irq = irq_save();
    for (struct listener **link = &services; *link; link = &(*link)->next) {
        if (*link == l) {
            *link = l->next;
            break;
        }
    }
    irq_restore(irq);
    /* Clients still waiting to be accepted see their channel close. */
    for (int i = 0; i < l->pending_count; i++) {
        file_put(l->pending[i]);
    }
    kfree(l);
}

uint16_t service_poll(struct file *f) {
    struct listener *l = f->object;
    return l->pending_count ? OS_POLL_IN : 0;
}
