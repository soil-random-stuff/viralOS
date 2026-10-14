/*
 * userfs: a filesystem whose driver is a program. To the rest of the kernel it's a diskfs mount like
 * any other (the tree of dnodes, the mount lock, the flusher); each driver operation becomes a request
 * on a message channel to the program, with file data and folder listings in a shared buffer. The
 * mount lock keeps one request in flight per filesystem.
 *
 * If the program goes away, its filesystem is unmounted (everything still open fails with EIO).
 */

#include "fs/userfs.h"
#include "arch/x86_64/cpu.h"
#include "drivers/timer.h"
#include "fs/diskfs.h"
#include "fs/mount.h"
#include "ipc/ipc.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/file.h"
#include "proc/process.h"
#include "sched/sched.h"
#include <paths.h>

#define START_TIMEOUT_MS 30000

struct userfs {
    struct file *channel;        /* the kernel's end */
    struct file *buffer;         /* shared with the program */
    uint32_t seq;
    bool broken;                 /* the program is gone */
};

static const struct fs_driver userfs_driver;
static const struct diskfs_ops userfs_ops;

static uint64_t node_of(struct dnode *n) {
    return (uint64_t)(uintptr_t)n->priv;
}

/* --- talking to the program ----------------------------------------------------------------------- */

static void unmount_thread(void *arg) {
    struct mount *m = arg;
    mount_unmount_forced(m->path);
}

/* The program is gone: everything fails from now on, and the filesystem comes off the tree. */
static void lost(struct mount *m) {
    struct userfs *u = m->fs;
    if (u->broken) {
        return;
    }
    u->broken = true;
    kprintf("[WARN] Disk: the program serving %s (%s) is gone; unmounting it\n", m->path, m->fs_name);
    thread_create("userfs-gone", unmount_thread, m);
}

void userfs_check(struct mount *m) {
    struct userfs *u = m->fs;
    if (u && !u->broken && (channel_poll(u->channel) & OS_POLL_HUP)) {
        lost(m);
    }
}

/* Send a request and wait for its answer. */
static int call(struct mount *m, struct os_userfs_request *req, struct os_userfs_reply *rep) {
    struct userfs *u = m->fs;
    if (u->broken) {
        return -EIO;
    }
    req->seq = ++u->seq;
    if (channel_send(u->channel, req, sizeof(*req), NULL, true) < 0) {
        lost(m);
        return -EIO;
    }
    for (;;) {
        struct file *attached = NULL;
        int64_t n = channel_recv(u->channel, rep, sizeof(*rep), &attached);
        if (attached) {
            file_put(attached);
        }
        if (n == -EINTR) {
            return -EINTR;   /* (its answer is skipped when the next request reads) */
        }
        if (n != (int64_t)sizeof(*rep)) {
            lost(m);
            return -EIO;
        }
        if (rep->seq == req->seq) {
            return rep->result < 0 ? (int)rep->result : 0;
        }
    }
}

/* --- diskfs operations ----------------------------------------------------------------------------- */

static int u_list(struct dnode *dir) {
    struct mount *m = dir->m;
    struct userfs *u = m->fs;
    uint64_t offset = 0;
    for (;;) {
        struct os_userfs_request req = { .op = OS_UFS_LIST, .node = node_of(dir), .offset = offset };
        struct os_userfs_reply rep;
        int err = call(m, &req, &rep);
        if (err) {
            return err;
        }
        uint64_t at = 0;
        for (int64_t i = 0; i < rep.result; i++) {
            struct os_userfs_entry e;
            char name[VFS_NAME_MAX];
            if (shm_copy(u->buffer, at, &e, sizeof(e), false) != 0 || e.name_len >= sizeof(name) ||
                shm_copy(u->buffer, at + sizeof(e), name, e.name_len, false) != 0) {
                return -EIO;
            }
            name[e.name_len] = '\0';
            at += OS_USERFS_ENTRY_SIZE(e.name_len);
            if (!e.node || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
                continue;
            }
            struct dnode *n = diskfs_new(m, name, e.type == OS_DIRECTORY ? VNODE_DIR : VNODE_FILE, e.size,
                                         (void *)(uintptr_t)e.node);
            if (!n) {
                return -ENOMEM;
            }
            n->v.mtime = n->v.ctime = e.mtime > 0 ? (uint64_t)e.mtime : 0;
            diskfs_add(dir, n);
        }
        if (!rep.more || rep.result <= 0) {
            return 0;
        }
        offset += (uint64_t)rep.result;
    }
}

static int64_t u_read(struct dnode *f, uint64_t offset, void *buf, size_t len) {
    struct mount *m = f->m;
    struct userfs *u = m->fs;
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done < OS_USERFS_BUFFER ? len - done : OS_USERFS_BUFFER;
        struct os_userfs_request req = { .op = OS_UFS_READ, .node = node_of(f), .offset = offset + done,
                                         .length = chunk };
        struct os_userfs_reply rep;
        int err = call(m, &req, &rep);
        if (err) {
            return done ? (int64_t)done : err;
        }
        size_t got = rep.result > (int64_t)chunk ? chunk : (size_t)rep.result;
        if (got && shm_copy(u->buffer, 0, (uint8_t *)buf + done, got, false) != 0) {
            return -EIO;
        }
        done += got;
        if (got < chunk) {
            break;   /* the end of the file */
        }
    }
    return (int64_t)done;
}

static int64_t u_write(struct dnode *f, uint64_t offset, const void *buf, size_t len) {
    struct mount *m = f->m;
    struct userfs *u = m->fs;
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done < OS_USERFS_BUFFER ? len - done : OS_USERFS_BUFFER;
        if (shm_copy(u->buffer, 0, (void *)((const uint8_t *)buf + done), chunk, true) != 0) {
            return -EIO;
        }
        struct os_userfs_request req = { .op = OS_UFS_WRITE, .node = node_of(f), .offset = offset + done,
                                         .length = chunk };
        struct os_userfs_reply rep;
        int err = call(m, &req, &rep);
        if (err) {
            return done ? (int64_t)done : err;
        }
        size_t wrote = rep.result > (int64_t)chunk ? chunk : (size_t)rep.result;
        done += wrote;
        if (offset + done > f->v.size) {
            f->v.size = offset + done;
        }
        if (wrote < chunk) {
            break;   /* full */
        }
    }
    if (len && !done) {
        return -ENOSPC;
    }
    return (int64_t)done;
}

static int u_truncate(struct dnode *f, uint64_t size) {
    struct os_userfs_request req = { .op = OS_UFS_TRUNCATE, .node = node_of(f), .length = size };
    struct os_userfs_reply rep;
    int err = call(f->m, &req, &rep);
    if (!err) {
        f->v.size = size;
    }
    return err;
}

static int u_create(struct dnode *dir, struct dnode *node) {
    struct os_userfs_request req = { .op = OS_UFS_CREATE, .dir = node_of(dir),
                                     .type = node->v.type == VNODE_DIR ? OS_DIRECTORY : OS_FILE };
    strlcpy(req.name, node->v.name, sizeof(req.name));
    struct os_userfs_reply rep;
    int err = call(dir->m, &req, &rep);
    if (!err) {
        node->priv = (void *)(uintptr_t)rep.node;
    }
    return err;
}

static int u_unlink(struct dnode *dir, struct dnode *child) {
    struct os_userfs_request req = { .op = OS_UFS_UNLINK, .dir = node_of(dir), .node = node_of(child),
                                     .type = child->v.type == VNODE_DIR ? OS_DIRECTORY : OS_FILE };
    strlcpy(req.name, child->v.name, sizeof(req.name));
    struct os_userfs_reply rep;
    return call(dir->m, &req, &rep);
}

static int u_rename(struct dnode *dir, struct dnode *child, struct dnode *new_dir, const char *new_name) {
    struct os_userfs_request req = { .op = OS_UFS_RENAME, .dir = node_of(dir), .node = node_of(child),
                                     .new_dir = node_of(new_dir),
                                     .type = child->v.type == VNODE_DIR ? OS_DIRECTORY : OS_FILE };
    strlcpy(req.name, child->v.name, sizeof(req.name));
    strlcpy(req.new_name, new_name, sizeof(req.new_name));
    struct os_userfs_reply rep;
    return call(dir->m, &req, &rep);
}

static const struct diskfs_ops userfs_ops = {
    .list = u_list,
    .read = u_read,
    .write = u_write,
    .truncate = u_truncate,
    .create = u_create,
    .unlink = u_unlink,
    .rename = u_rename,
    .release = NULL,   /* a node is just a number */
};

/* --- the driver ------------------------------------------------------------------------------------- */

static int u_mount(struct mount *m) {
    (void)m;
    return -EINVAL;   /* never found by probing: the program brings it */
}

static void u_unmount(struct mount *m) {
    struct userfs *u = m->fs;
    if (!u) {
        return;
    }
    if (!u->broken && !m->dead) {
        /* Let it write everything out; it exits after answering. */
        struct os_userfs_request req = { .op = OS_UFS_DESTROY };
        struct os_userfs_reply rep;
        int err = call(m, &req, &rep);
        if (err) {
            kprintf("[WARN] Disk: %s: the program couldn't finish writing (%s)\n", m->path, strerror(err));
        }
    }
    u->broken = true;
    file_put(u->channel);   /* the program sees the channel close, if it hasn't gone already */
    file_put(u->buffer);
    m->fs = NULL;
    kfree(u);
}

static int u_statfs(struct mount *m, uint64_t *total, uint64_t *free) {
    struct os_userfs_request req = { .op = OS_UFS_STATFS };
    struct os_userfs_reply rep;
    int err = call(m, &req, &rep);
    if (!err) {
        *total = rep.total;
        *free = rep.free;
    }
    return err;
}

static int u_sync(struct mount *m) {
    struct os_userfs_request req = { .op = OS_UFS_SYNC };
    struct os_userfs_reply rep;
    return call(m, &req, &rep);
}

static const struct fs_driver userfs_driver = {
    .name = "userfs",
    .mount = u_mount,
    .unmount = u_unmount,
    .statfs = u_statfs,
    .sync = u_sync,
};

/* --- starting and registering ------------------------------------------------------------------------ */

const char *userfs_server_for(const char *driver_name) {
    if (strcmp(driver_name, "NTFS") == 0 || strcmp(driver_name, "ntfs") == 0) {
        return _PATH_BIN "/ntfsd";
    }
    if (strcmp(driver_name, "ext4") == 0) {
        return _PATH_BIN "/ext4d";
    }
    return NULL;
}

int userfs_start(struct block_device *d, const char *server, const char *path) {
    const char *name = server;
    for (const char *c = server; *c; c++) {
        if (*c == '/') {
            name = c + 1;
        }
    }
    char *argv[3] = { (char *)name, d->name, (char *)(path ? path : "") };
    struct spawn_options opts = { .detached = true };
    int err;
    /* What it says goes to the kernel's log (not a file: writing one could wait for a lock held while
       the kernel waits for it). */
    struct file *log = file_open(NULL, _PATH_DEV "/log", O_WRONLY, &err);
    opts.files[1] = opts.files[2] = log;
    struct process *p = process_spawn(server, 3, argv, &opts, &err);
    if (log) {
        file_put(log);
    }
    if (!p) {
        return err;
    }
    uint64_t pid = opts.pid;
    kprintf("       Disk: %s: started %s (pid %lu) to mount it read-write\n", d->name, server, pid);
    /* Wait for it to put the filesystem in the tree, or to give up. */
    for (uint64_t end = uptime_ms() + START_TIMEOUT_MS; uptime_ms() < end;) {
        if (mount_of(d)) {
            return 0;
        }
        uint64_t irq = irq_save();
        struct process *q = process_find(pid);
        bool alive = q && !q->exited;
        irq_restore(irq);
        if (!alive) {
            return mount_of(d) ? 0 : -EIO;
        }
        sched_sleep_ms(20);
    }
    return -EIO;   /* it never said */
}

int64_t userfs_register(struct process *p, const struct os_userfs *in, int32_t *buffer_fd) {
    struct block_device *d = block_find(in->device);
    if (!d) {
        return -ENODEV;
    }
    if (mount_busy(d)) {
        return -EBUSY;
    }
    struct userfs *u = kzalloc(sizeof(*u));
    struct mount *m = kzalloc(sizeof(*m));
    struct file *mine = NULL, *theirs = NULL, *buffer = NULL;
    int err = !u || !m ? -ENOMEM : channel_pair(&mine, &theirs);
    if (!err) {
        err = shm_create(OS_USERFS_BUFFER, &buffer);
    }
    if (err) {
        goto fail;
    }
    /* Descriptors for the program: its end of the channel, and the buffer. */
    int chan_fd = -1, buf_fd = -1;
    for (int fd = 0; fd < PROCESS_MAX_FILES && (chan_fd < 0 || buf_fd < 0); fd++) {
        if (!p->files[fd]) {
            if (chan_fd < 0) {
                chan_fd = fd;
            } else {
                buf_fd = fd;
            }
        }
    }
    if (buf_fd < 0) {
        err = -EMFILE;
        goto fail;
    }

    m->sb.lock = (struct mutex)MUTEX_INIT;
    m->dev = d;
    m->driver = &userfs_driver;
    m->ops = &userfs_ops;
    m->fs = u;
    m->server_pid = p->pid;
    m->case_insensitive = in->flags & OS_USERFS_CASE_INSENSITIVE;
    strlcpy(m->fs_name, in->fs_name, sizeof(m->fs_name));
    strlcpy(m->label, in->label, sizeof(m->label));
    strlcpy(m->uuid, in->uuid, sizeof(m->uuid));
    u->channel = mine;
    u->buffer = buffer;
    m->root = diskfs_new(m, "", VNODE_DIR, 0, (void *)(uintptr_t)in->root);
    if (!m->root || !in->root) {
        err = m->root ? -EINVAL : -ENOMEM;
        kfree(m->root);
        goto fail;
    }
    /* Installed before the filesystem appears: requests may come at once (they wait in the channel). */
    p->files[chan_fd] = theirs;
    file_get(buffer);
    p->files[buf_fd] = buffer;
    err = mount_attach(m, in->path[0] ? in->path : NULL);
    if (err) {
        p->files[chan_fd] = NULL;
        p->files[buf_fd] = NULL;
        file_put(buffer);
        kfree(m->root);
        goto fail;
    }
    *buffer_fd = buf_fd;
    return chan_fd;

fail:
    if (mine) {
        file_put(mine);
    }
    if (theirs) {
        file_put(theirs);
    }
    if (buffer) {
        file_put(buffer);
    }
    kfree(u);
    kfree(m);
    return err;
}
