#include "fs/devfs.h"
#include "block/block.h"
#include "fs/mount.h"
#include "fs/vfs.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include <paths.h>

static const struct vnode_ops dir_ops, file_ops, null_ops, log_ops;
static struct vnode root, null_node;   /* /dev/null: reads nothing, swallows writes */
static struct vnode log_node;          /* /dev/log: what's written goes to the kernel's log */
static struct vfs_sb sb = { MUTEX_INIT };   /* device I/O doesn't take the tree lock */

static bool visible(struct block_device *d) {
    return !d->gone && !d->disk->gone && d->node.ops == &file_ops;
}

static int d_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    (void)dir;
    if (strcmp(name, "null") == 0) {
        *out = &null_node;
        return 0;
    }
    if (strcmp(name, "log") == 0) {
        *out = &log_node;
        return 0;
    }
    for (int i = 0;; i++) {
        struct block_device *d = block_at(i);
        if (!d) {
            return -ENOENT;
        }
        if (visible(d) && strcmp(d->name, name) == 0) {
            *out = &d->node;
            return 0;
        }
    }
}

static int d_readdir(struct vnode *dir, size_t index, struct vnode **out) {
    (void)dir;
    if (index-- == 0) {
        *out = &null_node;
        return 0;
    }
    if (index-- == 0) {
        *out = &log_node;
        return 0;
    }
    for (int i = 0;; i++) {
        struct block_device *d = block_at(i);
        if (!d) {
            return -ENOENT;
        }
        if (visible(d) && index-- == 0) {
            *out = &d->node;
            return 0;
        }
    }
}

static int no_create(struct vnode *dir, const char *name, enum vnode_type type, uint32_t mode, struct vnode **out) {
    (void)dir, (void)name, (void)type, (void)mode, (void)out;
    return -EPERM;
}

static int no_unlink(struct vnode *dir, struct vnode *child) {
    (void)dir, (void)child;
    return -EPERM;
}

static void no_release(struct vnode *n) {
    (void)n;
}

static int64_t f_read(struct vnode *n, uint64_t offset, void *buf, size_t len) {
    struct block_device *d = n->fs_data;
    uint64_t size = d->sectors * d->sector_size;
    if (offset >= size) {
        return 0;
    }
    if (len > size - offset) {
        len = (size_t)(size - offset);
    }
    int err = bio_read(d, offset, buf, len);
    return err ? err : (int64_t)len;
}

static int64_t f_write(struct vnode *n, uint64_t offset, const void *buf, size_t len) {
    struct block_device *d = n->fs_data;
    /* A mounted partition can't be written behind its filesystem's back; on a whole disk only the
       parts that are mounted are off limits (so the partition table can change around them). */
    if (!mount_write_allowed(d, offset, len)) {
        return -EBUSY;
    }
    uint64_t size = d->sectors * d->sector_size;
    if (offset >= size) {
        return len ? -ENOSPC : 0;
    }
    if (len > size - offset) {
        len = (size_t)(size - offset);
    }
    int err = bio_write(d, offset, buf, len);
    return err ? err : (int64_t)len;
}

static int64_t null_read(struct vnode *n, uint64_t offset, void *buf, size_t len) {
    (void)n, (void)offset, (void)buf, (void)len;
    return 0;
}

static int64_t null_write(struct vnode *n, uint64_t offset, const void *buf, size_t len) {
    (void)n, (void)offset, (void)buf;
    return (int64_t)len;
}

static int f_truncate(struct vnode *n, uint64_t size) {
    (void)n, (void)size;
    return 0;   /* (O_TRUNC on a device does nothing, as on Linux) */
}

static const struct vnode_ops dir_ops = {
    .lookup = d_lookup,
    .readdir = d_readdir,
    .create = no_create,
    .unlink = no_unlink,
    .release = no_release,
};

static const struct vnode_ops file_ops = {
    .read = f_read,
    .write = f_write,
    .truncate = f_truncate,
    .release = no_release,
};

static int64_t log_write(struct vnode *n, uint64_t offset, const void *buf, size_t len) {
    (void)n, (void)offset;
    char chunk[257];
    for (size_t done = 0; done < len;) {
        size_t k = len - done < 256 ? len - done : 256;
        memcpy(chunk, (const char *)buf + done, k);
        chunk[k] = '\0';
        kprintf("%s", chunk);
        done += k;
    }
    return (int64_t)len;
}

static const struct vnode_ops log_ops = {
    .read = null_read,
    .write = log_write,
    .truncate = f_truncate,
    .release = no_release,
};

static const struct vnode_ops null_ops = {
    .read = null_read,
    .write = null_write,
    .truncate = f_truncate,
    .release = no_release,
};

void devfs_add(struct block_device *d) {
    struct vnode *n = &d->node;
    memset(n, 0, sizeof(*n));
    strlcpy(n->name, d->name, sizeof(n->name));
    n->type = VNODE_FILE;
    n->mode = d->read_only ? 0440 : 0660;
    n->size = d->sectors * d->sector_size;
    n->parent = &root;
    n->ops = &file_ops;
    n->fs_data = d;
    n->sb = &sb;
    n->refcount = 1;   /* never freed (the device struct lives on) */
}

struct block_device *devfs_device(struct vnode *n) {
    return n && n->ops == &file_ops ? n->fs_data : NULL;
}

void devfs_init(void) {
    root.type = VNODE_DIR;
    root.mode = 0755;
    root.ops = &dir_ops;
    root.sb = &sb;
    root.refcount = 1;
    strlcpy(null_node.name, "null", sizeof(null_node.name));
    null_node.type = VNODE_FILE;
    null_node.mode = 0666;
    null_node.parent = &root;
    null_node.ops = &null_ops;
    null_node.sb = &sb;
    null_node.refcount = 1;
    log_node = null_node;
    strlcpy(log_node.name, "log", sizeof(log_node.name));
    log_node.mode = 0222;
    log_node.ops = &log_ops;
    devfs_reattach();
}

void devfs_reattach(void) {
    vfs_create(NULL, _PATH_DEV, VNODE_DIR, 0755, NULL);
    int err = vfs_attach(_PATH_DEV, &root);
    if (err) {
        kprintf("[WARN] Can't set up /dev: %s\n", strerror(err));
    }
}
