/*
 * ramfs: directories keep their children in a name-sorted linked list,
 * files keep their bytes in a heap buffer that grows as they're written.
 * Called by the VFS with its lock held.
 */

#include <stdbool.h>

#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "lib/errno.h"
#include "lib/string.h"
#include "mm/heap.h"

struct ramfs_node {
    uint8_t *data;           /* files */
    uint64_t capacity;
    struct vnode *children;  /* directories: first child */
    struct vnode *next;      /* next sibling in the parent's list */
};

static struct ramfs_stats stats;

static const struct vnode_ops ramfs_ops;
static void insert_sorted(struct vnode *dir, struct vnode *n);

static struct ramfs_node *rn(struct vnode *n) {
    return n->fs_data;
}

static struct vnode *alloc_node(const char *name, enum vnode_type type, uint32_t mode) {
    struct vnode *n = kzalloc(sizeof(*n));
    struct ramfs_node *r = kzalloc(sizeof(*r));
    if (!n || !r) {
        kfree(n);
        kfree(r);
        return NULL;
    }
    strlcpy(n->name, name, sizeof(n->name));
    n->type = type;
    n->mode = mode;
    n->ops = &ramfs_ops;
    n->fs_data = r;
    if (type == VNODE_DIR) {
        stats.directories++;
    } else {
        stats.files++;
    }
    return n;
}

struct vnode *ramfs_create_root(void) {
    return alloc_node("", VNODE_DIR, 0755);
}

void ramfs_get_stats(struct ramfs_stats *out) {
    *out = stats;
}

/* --- files ------------------------------------------------------------------- */

static int64_t ramfs_read(struct vnode *n, uint64_t offset, void *buf, size_t len) {
    if (offset >= n->size) {
        return 0;
    }
    if (len > n->size - offset) {
        len = n->size - offset;
    }
    memcpy(buf, rn(n)->data + offset, len);
    return (int64_t)len;
}

/* Make room for `size` bytes, zero-filling anything new. */
static int reserve(struct vnode *n, uint64_t size) {
    struct ramfs_node *r = rn(n);
    if (size > r->capacity) {
        uint64_t cap = r->capacity ? r->capacity * 2 : 64;
        while (cap < size) {
            cap *= 2;
        }
        uint8_t *data = krealloc(r->data, cap);
        if (!data) {
            return -ENOMEM;
        }
        r->data = data;
        r->capacity = cap;
    }
    if (size > n->size) {
        memset(r->data + n->size, 0, size - n->size);
    }
    return 0;
}

static int64_t ramfs_write(struct vnode *n, uint64_t offset, const void *buf, size_t len) {
    uint64_t end = offset + len;
    int err = reserve(n, end);
    if (err) {
        return err;
    }
    memcpy(rn(n)->data + offset, buf, len);
    if (end > n->size) {
        stats.bytes += end - n->size;
        n->size = end;
    }
    return (int64_t)len;
}

static int ramfs_truncate(struct vnode *n, uint64_t size) {
    int err = reserve(n, size);
    if (err) {
        return err;
    }
    stats.bytes = stats.bytes - n->size + size;
    n->size = size;
    return 0;
}

/* --- directories --------------------------------------------------------------- */

static int ramfs_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    for (struct vnode *c = rn(dir)->children; c; c = rn(c)->next) {
        if (strcmp(c->name, name) == 0) {
            *out = c;
            return 0;
        }
    }
    return -ENOENT;
}

static int ramfs_readdir(struct vnode *dir, size_t index, struct vnode **out) {
    for (struct vnode *c = rn(dir)->children; c; c = rn(c)->next) {
        if (index-- == 0) {
            *out = c;
            return 0;
        }
    }
    return -ENOENT;
}

static int ramfs_create(struct vnode *dir, const char *name, enum vnode_type type,
                        uint32_t mode, struct vnode **out) {
    struct vnode *n = alloc_node(name, type, mode);
    if (!n) {
        return -ENOMEM;
    }
    n->parent = dir;

    insert_sorted(dir, n);   /* sorted, so `ls` comes out in order */

    *out = n;
    return 0;
}

static int ramfs_unlink(struct vnode *dir, struct vnode *child) {
    if (child->type == VNODE_DIR && rn(child)->children) {
        return -ENOTEMPTY;
    }
    for (struct vnode **link = &rn(dir)->children; *link; link = &rn(*link)->next) {
        if (*link == child) {
            *link = rn(child)->next;
            return 0;
        }
    }
    return -ENOENT;
}

static void insert_sorted(struct vnode *dir, struct vnode *n) {
    struct vnode **link = &rn(dir)->children;
    while (*link && strcmp((*link)->name, n->name) < 0) {
        link = &rn(*link)->next;
    }
    rn(n)->next = *link;
    *link = n;
}

static int ramfs_rename(struct vnode *dir, struct vnode *child, struct vnode *new_dir,
                        const char *new_name) {
    for (struct vnode **link = &rn(dir)->children; *link; link = &rn(*link)->next) {
        if (*link == child) {
            *link = rn(child)->next;
            strlcpy(child->name, new_name, sizeof(child->name));
            child->parent = new_dir;
            insert_sorted(new_dir, child);
            return 0;
        }
    }
    return -ENOENT;
}

static void ramfs_release(struct vnode *n) {
    if (n->type == VNODE_DIR) {
        stats.directories--;
    } else {
        stats.files--;
        stats.bytes -= n->size;
    }
    kfree(rn(n)->data);
    kfree(rn(n));
}

static const struct vnode_ops ramfs_ops = {
    .read     = ramfs_read,
    .write    = ramfs_write,
    .truncate = ramfs_truncate,
    .lookup   = ramfs_lookup,
    .readdir  = ramfs_readdir,
    .create   = ramfs_create,
    .unlink   = ramfs_unlink,
    .rename   = ramfs_rename,
    .release  = ramfs_release,
};
