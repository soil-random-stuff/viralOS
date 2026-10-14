#include <stdbool.h>

#include "proc/file.h"
#include "proc/tty.h"
#include "arch/x86_64/cpu.h"
#include "fs/vfs.h"
#include "ipc/ipc.h"
#include "lib/errno.h"
#include "mm/heap.h"
#include "net/net.h"

static struct file *file_alloc(enum file_kind kind) {
    struct file *f = kzalloc(sizeof(*f));
    if (f) {
        f->kind = kind;
        f->refs = 1;
        f->flags = O_RDWR;
    }
    return f;
}

struct file *file_new(enum file_kind kind) {
    return file_alloc(kind);
}

struct file *file_open_console(void) {
    struct file *f = file_alloc(FILE_CONSOLE);
    if (f) {
        f->flags = O_RDWR;
    }
    return f;
}

struct file *file_open(struct vnode *cwd, const char *path, int flags, int *err) {
    int access = flags & O_ACCMODE;
    bool writing = access == O_WRONLY || access == O_RDWR;

    struct vnode *n;
    *err = vfs_resolve(cwd, path, &n);
    if (*err == -ENOENT && (flags & O_CREAT)) {
        *err = vfs_create(cwd, path, VNODE_FILE, 0644, &n);
    }
    if (*err) {
        return NULL;
    }
    if (n->type == VNODE_DIR && writing) {
        vfs_put(n);
        *err = -EISDIR;
        return NULL;
    }
    if (writing && (flags & O_TRUNC) && n->type == VNODE_FILE) {
        *err = vfs_truncate(n, 0);
        if (*err) {
            vfs_put(n);
            return NULL;
        }
    }

    struct file *f = file_alloc(FILE_VNODE);
    if (!f) {
        vfs_put(n);
        *err = -ENOMEM;
        return NULL;
    }
    f->vnode = n;
    f->flags = flags;
    return f;
}

void file_get(struct file *f) {
    uint64_t irq = irq_save();
    f->refs++;
    irq_restore(irq);
}

void file_put(struct file *f) {
    uint64_t irq = irq_save();
    bool last = --f->refs == 0;
    irq_restore(irq);
    if (last) {
        switch (f->kind) {
        case FILE_VNODE:    vfs_put(f->vnode); break;
        case FILE_CHANNEL:  channel_close(f->object, f->end); break;
        case FILE_LISTENER: service_close(f->object); break;
        case FILE_SHM:      shm_put(f->object); break;
        case FILE_PTY:        pty_close(f->object, false); break;
        case FILE_PTY_MASTER: pty_close(f->object, true); break;
        case FILE_SOCKET:   net_socket_close(f); break;
        case FILE_CONSOLE:  break;
        }
        kfree(f);
    }
}

int64_t file_read(struct file *f, void *buf, size_t len) {
    if ((f->flags & O_ACCMODE) == O_WRONLY) {
        return -EBADF;
    }
    switch (f->kind) {
    case FILE_VNODE:      break;
    case FILE_CONSOLE:
    case FILE_PTY:        return tty_read(tty_of(f), buf, len);
    case FILE_PTY_MASTER: return pty_master_read(f->object, buf, len);
    default:              return -EINVAL;   /* messages go through msg_recv */
    }
    int64_t n = vfs_read(f->vnode, f->offset, buf, len);
    if (n > 0) {
        f->offset += (uint64_t)n;
    }
    return n;
}

int64_t file_write(struct file *f, const void *buf, size_t len) {
    if ((f->flags & O_ACCMODE) == O_RDONLY) {
        return -EBADF;
    }
    switch (f->kind) {
    case FILE_VNODE:      break;
    case FILE_CONSOLE:
    case FILE_PTY:        return tty_write(tty_of(f), buf, len);
    case FILE_PTY_MASTER: return pty_master_write(f->object, buf, len);
    default:              return -EINVAL;   /* messages go through msg_send */
    }
    if (f->flags & O_APPEND) {
        f->offset = f->vnode->size;
    }
    int64_t n = vfs_write(f->vnode, f->offset, buf, len);
    if (n > 0) {
        f->offset += (uint64_t)n;
    }
    return n;
}
