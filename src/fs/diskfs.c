#include "fs/diskfs.h"
#include "block/block.h"
#include "drivers/rtc.h"
#include "lib/errno.h"
#include "lib/string.h"
#include "mm/heap.h"

static const struct vnode_ops diskfs_vnode_ops;

static struct dnode *dn(struct vnode *v) {
    return (struct dnode *)v;
}

struct dnode *diskfs_new(struct mount *m, const char *name, enum vnode_type type, uint64_t size, void *priv) {
    struct dnode *n = kzalloc(sizeof(*n));
    if (!n) {
        return NULL;
    }
    strlcpy(n->v.name, name, sizeof(n->v.name));
    n->v.type = type;
    n->v.size = type == VNODE_FILE ? size : 0;
    n->v.mode = type == VNODE_DIR ? (m->read_only ? 0555 : 0755) : (m->read_only ? 0444 : 0644);
    n->v.ops = &diskfs_vnode_ops;
    n->v.sb = &m->sb;
    n->m = m;
    n->priv = priv;
    return n;
}

void diskfs_add(struct dnode *dir, struct dnode *child) {
    /* Appended, so listings come in the disk's order; programs sort for themselves. */
    struct dnode **link = &dir->children;
    while (*link) {
        link = &(*link)->next;
    }
    child->next = NULL;
    *link = child;
    child->v.parent = &dir->v;
}

static void remove_child(struct dnode *dir, struct dnode *child) {
    for (struct dnode **link = &dir->children; *link; link = &(*link)->next) {
        if (*link == child) {
            *link = child->next;
            child->next = NULL;
            return;
        }
    }
}

static char fold(char c) {
    return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

bool diskfs_same_name(struct mount *m, const char *a, const char *b) {
    if (!m->case_insensitive) {
        return strcmp(a, b) == 0;
    }
    while (*a && fold(*a) == fold(*b)) {
        a++;
        b++;
    }
    return fold(*a) == fold(*b);
}

static int ensure_listed(struct dnode *dir) {
    if (dir->listed) {
        return 0;
    }
    int err = dir->m->ops->list(dir);
    if (err) {
        /* Keep what was read (better than nothing), but try again next time. */
        return dir->children ? 0 : err;
    }
    dir->listed = true;
    return 0;
}

static int writable(struct dnode *n) {
    if (n->m->dead) {
        return -EIO;
    }
    return n->m->read_only ? -EROFS : 0;
}

/* After a change: everything on disk, in order. */
static int sync(struct mount *m, int err) {
    int e = bio_sync(m->dev);
    return err ? err : e;
}

/* --- vnode operations -------------------------------------------------------------------------------- */

static int64_t d_read(struct vnode *v, uint64_t offset, void *buf, size_t len) {
    struct dnode *n = dn(v);
    if (n->m->dead) {
        return -EIO;
    }
    if (offset >= v->size) {
        return 0;
    }
    if (len > v->size - offset) {
        len = v->size - offset;
    }
    return n->m->ops->read(n, offset, buf, len);
}

/* File contents: the flusher (mount.c) syncs within half a second, so a stream of small writes
   becomes a few big ones. */
static int64_t d_write(struct vnode *v, uint64_t offset, const void *buf, size_t len) {
    struct dnode *n = dn(v);
    int err = writable(n);
    if (err) {
        return err;
    }
    n->m->dirty = true;
    return n->m->ops->write(n, offset, buf, len);
}

static int d_truncate(struct vnode *v, uint64_t size) {
    struct dnode *n = dn(v);
    int err = writable(n);
    if (err) {
        return err;
    }
    n->m->dirty = true;
    return n->m->ops->truncate(n, size);
}

/* Folder operations come with the tree lock held; they take the mount's lock for the driver.
   (File reads and writes get the mount's lock from the VFS.) */
static void lock(struct mount *m) {
    mutex_lock(&m->sb.lock);
}

static void unlock(struct mount *m) {
    mutex_unlock(&m->sb.lock);
}

static int d_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    struct dnode *d = dn(dir);
    if (d->m->dead) {
        return -EIO;
    }
    lock(d->m);
    int err = ensure_listed(d);
    if (!err) {
        err = -ENOENT;
        for (struct dnode *c = d->children; c; c = c->next) {
            if (diskfs_same_name(d->m, c->v.name, name)) {
                *out = &c->v;
                err = 0;
                break;
            }
        }
    }
    unlock(d->m);
    return err;
}

static int d_readdir(struct vnode *dir, size_t index, struct vnode **out) {
    struct dnode *d = dn(dir);
    if (d->m->dead) {
        return -EIO;
    }
    lock(d->m);
    int err = ensure_listed(d);
    if (!err) {
        err = -ENOENT;
        for (struct dnode *c = d->children; c; c = c->next) {
            if (index-- == 0) {
                *out = &c->v;
                err = 0;
                break;
            }
        }
    }
    unlock(d->m);
    return err;
}

static int create_locked(struct dnode *d, const char *name, enum vnode_type type, struct vnode **out) {
    int err = writable(d);
    if (!err) {
        err = ensure_listed(d);
    }
    if (err) {
        return err;
    }
    struct dnode *n = diskfs_new(d->m, name, type, 0, NULL);
    if (!n) {
        return -ENOMEM;
    }
    n->v.mtime = n->v.ctime = rtc_time();
    n->listed = true;   /* a new folder is empty */
    err = sync(d->m, d->m->ops->create(d, n));
    if (err) {
        if (n->priv) {
            d->m->ops->release(n);
        }
        kfree(n);
        return err;
    }
    diskfs_add(d, n);
    *out = &n->v;
    return 0;
}

static int d_create(struct vnode *dir, const char *name, enum vnode_type type, uint32_t mode, struct vnode **out) {
    (void)mode;
    struct dnode *d = dn(dir);
    lock(d->m);
    int err = create_locked(d, name, type, out);
    unlock(d->m);
    return err;
}

static int d_unlink(struct vnode *dir, struct vnode *child) {
    struct dnode *d = dn(dir), *c = dn(child);
    lock(d->m);
    int err = writable(d);
    if (!err && child->type == VNODE_DIR) {
        err = ensure_listed(c);
        if (!err && c->children) {
            err = -ENOTEMPTY;
        }
    }
    if (!err) {
        err = sync(d->m, d->m->ops->unlink(d, c));
    }
    if (!err) {
        remove_child(d, c);
    }
    unlock(d->m);
    return err;
}

static int d_rename(struct vnode *dir, struct vnode *child, struct vnode *new_dir, const char *new_name) {
    struct dnode *d = dn(dir), *c = dn(child), *nd = dn(new_dir);
    lock(d->m);
    int err = writable(d);
    if (!err) {
        err = ensure_listed(nd);
    }
    if (!err) {
        err = sync(d->m, d->m->ops->rename(d, c, nd, new_name));
    }
    if (!err) {
        remove_child(d, c);
        strlcpy(c->v.name, new_name, sizeof(c->v.name));
        diskfs_add(nd, c);
    }
    unlock(d->m);
    return err;
}

static void d_release(struct vnode *v) {
    struct dnode *n = dn(v);
    if (n->priv && n->m->ops->release) {
        n->m->ops->release(n);
    }
    n->priv = NULL;
}

static const struct vnode_ops diskfs_vnode_ops = {
    .read = d_read,
    .write = d_write,
    .truncate = d_truncate,
    .lookup = d_lookup,
    .readdir = d_readdir,
    .create = d_create,
    .unlink = d_unlink,
    .rename = d_rename,
    .release = d_release,
};

/* (Call after vfs_detach, which drops the mount's own reference on the root.) */
static bool tree_in_use(struct dnode *n) {
    if (n->v.refcount) {
        return true;
    }
    for (struct dnode *c = n->children; c; c = c->next) {
        if (tree_in_use(c)) {
            return true;
        }
    }
    return false;
}

static void free_tree(struct dnode *n) {
    struct dnode *c = n->children;
    while (c) {
        struct dnode *next = c->next;
        free_tree(c);
        c = next;
    }
    d_release(&n->v);
    kfree(n);
}

bool diskfs_free_tree(struct dnode *root) {
    if (tree_in_use(root)) {
        return false;
    }
    free_tree(root);
    return true;
}

/* --- UTF-16 and UTF-8 ---------------------------------------------------------------------------------- */

size_t utf16_to_utf8(const uint16_t *in, size_t count, char *out, size_t size) {
    size_t o = 0;
    for (size_t i = 0; i < count && in[i]; i++) {
        uint32_t c = in[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < count && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c < 0xE000) {
            c = 0xFFFD;   /* a lone surrogate */
        }
        char tmp[4];
        int n;
        if (c < 0x80) {
            tmp[0] = (char)c;
            n = 1;
        } else if (c < 0x800) {
            tmp[0] = (char)(0xC0 | c >> 6);
            tmp[1] = (char)(0x80 | (c & 0x3F));
            n = 2;
        } else if (c < 0x10000) {
            tmp[0] = (char)(0xE0 | c >> 12);
            tmp[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            tmp[2] = (char)(0x80 | (c & 0x3F));
            n = 3;
        } else {
            tmp[0] = (char)(0xF0 | c >> 18);
            tmp[1] = (char)(0x80 | ((c >> 12) & 0x3F));
            tmp[2] = (char)(0x80 | ((c >> 6) & 0x3F));
            tmp[3] = (char)(0x80 | (c & 0x3F));
            n = 4;
        }
        if (o + (size_t)n + 1 > size) {
            break;
        }
        memcpy(out + o, tmp, (size_t)n);
        o += (size_t)n;
    }
    if (size) {
        out[o] = '\0';
    }
    return o;
}

int utf8_to_utf16(const char *in, uint16_t *out, size_t max) {
    const uint8_t *s = (const uint8_t *)in;
    size_t o = 0;
    while (*s) {
        uint32_t c;
        if (*s < 0x80) {
            c = *s++;
        } else if ((*s & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            c = (uint32_t)(*s & 0x1F) << 6 | (s[1] & 0x3F);
            s += 2;
        } else if ((*s & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            c = (uint32_t)(*s & 0x0F) << 12 | (uint32_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3F);
            s += 3;
        } else if ((*s & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
            c = (uint32_t)(*s & 0x07) << 18 | (uint32_t)(s[1] & 0x3F) << 12 | (uint32_t)(s[2] & 0x3F) << 6 |
                (s[3] & 0x3F);
            s += 4;
        } else {
            return -1;
        }
        if (c >= 0x10000) {
            if (o + 2 > max) {
                return -1;
            }
            c -= 0x10000;
            out[o++] = (uint16_t)(0xD800 + (c >> 10));
            out[o++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else {
            if (o + 1 > max) {
                return -1;
            }
            out[o++] = (uint16_t)c;
        }
    }
    return (int)o;
}

/* --- dates --------------------------------------------------------------------------------------------- */

static bool leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static const int month_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

void dtime_from_unix(uint64_t t, struct dtime *out) {
    uint64_t days = t / 86400, secs = t % 86400;
    out->hour = (int)(secs / 3600);
    out->minute = (int)(secs / 60 % 60);
    out->second = (int)(secs % 60);
    int y = 1970;
    for (;;) {
        uint64_t len = leap(y) ? 366 : 365;
        if (days < len) {
            break;
        }
        days -= len;
        y++;
    }
    int m = 0;
    for (;; m++) {
        uint64_t len = (uint64_t)month_days[m] + (m == 1 && leap(y));
        if (days < len) {
            break;
        }
        days -= len;
    }
    out->year = y;
    out->month = m + 1;
    out->day = (int)days + 1;
}

uint64_t dtime_to_unix(const struct dtime *t) {
    if (t->year < 1970 || t->month < 1 || t->month > 12 || t->day < 1) {
        return 0;
    }
    uint64_t days = 0;
    for (int y = 1970; y < t->year; y++) {
        days += leap(y) ? 366 : 365;
    }
    for (int m = 0; m < t->month - 1; m++) {
        days += (uint64_t)month_days[m] + (m == 1 && leap(t->year));
    }
    days += (uint64_t)t->day - 1;
    return days * 86400 + (uint64_t)t->hour * 3600 + (uint64_t)t->minute * 60 + (uint64_t)t->second;
}
