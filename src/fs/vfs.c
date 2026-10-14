#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "drivers/rtc.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/sync.h"

/* One big lock around the whole tree: simple, and plenty for a RAM filesystem. */
static struct mutex vfs_lock = MUTEX_INIT;
static struct vnode *root;

void vfs_init(void) {
    root = ramfs_create_root();
    if (!root) {
        panic("vfs: cannot create the root directory");
    }
    root->refcount = 1;   /* the root is never freed */
}

struct vnode *vfs_root(void) {
    return root;
}

/* --- references ------------------------------------------------------------ */

static void maybe_free(struct vnode *n) {
    if (n->unlinked && n->refcount == 0) {
        n->ops->release(n);
        kfree(n);
    }
}

void vfs_get(struct vnode *n) {
    mutex_lock(&vfs_lock);
    n->refcount++;
    mutex_unlock(&vfs_lock);
}

void vfs_put(struct vnode *n) {
    mutex_lock(&vfs_lock);
    if (n->refcount == 0) {
        panic("vfs_put: '%s' has no references", n->name);
    }
    n->refcount--;
    maybe_free(n);
    mutex_unlock(&vfs_lock);
}

/* --- path resolution (call with vfs_lock held) ----------------------------- */

static int resolve_locked(struct vnode *cwd, const char *path, struct vnode **out) {
    struct vnode *node = (path[0] == '/' || !cwd) ? root : cwd;
    char component[VFS_NAME_MAX];

    for (const char *p = path;;) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        size_t len = 0;
        while (p[len] && p[len] != '/') {
            len++;
        }
        if (len >= VFS_NAME_MAX) {
            return -ENAMETOOLONG;
        }
        memcpy(component, p, len);
        component[len] = '\0';
        p += len;

        if (node->type != VNODE_DIR) {
            return -ENOTDIR;
        }
        if (strcmp(component, ".") == 0) {
            continue;
        }
        if (strcmp(component, "..") == 0) {
            if (node->parent) {
                node = node->parent;
            }
            continue;
        }
        int err = node->ops->lookup(node, component, &node);
        if (err) {
            return err;
        }
        while (node->mounted) {
            node = node->mounted;
        }
    }
    *out = node;
    return 0;
}

/* Split "a/b/c" into the vnode for "a/b" and the name "c". */
static int resolve_parent_locked(struct vnode *cwd, const char *path,
                                 struct vnode **parent, char name[VFS_NAME_MAX]) {
    char buf[VFS_PATH_MAX];
    if (strlcpy(buf, path, sizeof(buf)) >= sizeof(buf)) {
        return -ENAMETOOLONG;
    }
    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/') {
        buf[--len] = '\0';
    }

    char *slash = strrchr(buf, '/');
    const char *dir, *base;
    if (!slash) {
        dir = ".";
        base = buf;
    } else if (slash == buf) {
        dir = "/";
        base = buf + 1;
    } else {
        *slash = '\0';
        dir = buf;
        base = slash + 1;
    }
    if (!*base || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        return -EINVAL;
    }
    if (strlen(base) >= VFS_NAME_MAX) {
        return -ENAMETOOLONG;
    }

    int err = resolve_locked(cwd, dir, parent);
    if (err) {
        return err;
    }
    if ((*parent)->type != VNODE_DIR) {
        return -ENOTDIR;
    }
    strlcpy(name, base, VFS_NAME_MAX);
    return 0;
}

/* --- public API ------------------------------------------------------------- */

int vfs_resolve(struct vnode *cwd, const char *path, struct vnode **out) {
    mutex_lock(&vfs_lock);
    int err = resolve_locked(cwd, path, out);
    if (!err) {
        (*out)->refcount++;
    }
    mutex_unlock(&vfs_lock);
    return err;
}

int vfs_create(struct vnode *cwd, const char *path, enum vnode_type type, uint32_t mode,
               struct vnode **out) {
    mutex_lock(&vfs_lock);
    struct vnode *parent, *existing, *node = NULL;
    char name[VFS_NAME_MAX];
    int err = resolve_parent_locked(cwd, path, &parent, name);
    if (!err) {
        err = parent->ops->lookup(parent, name, &existing) == 0 ? -EEXIST : 0;
    }
    if (!err) {
        err = parent->ops->create(parent, name, type, mode, &node);
    }
    if (!err) {
        node->mtime = node->ctime = rtc_time();
        parent->mtime = node->mtime;
    }
    if (!err && out) {
        node->refcount++;
        *out = node;
    }
    mutex_unlock(&vfs_lock);
    return err;
}

int vfs_rename(struct vnode *cwd, const char *from, const char *to) {
    mutex_lock(&vfs_lock);
    struct vnode *old_dir, *new_dir, *node, *existing;
    char old_name[VFS_NAME_MAX], new_name[VFS_NAME_MAX];
    int err = resolve_parent_locked(cwd, from, &old_dir, old_name);
    if (!err) {
        err = old_dir->ops->lookup(old_dir, old_name, &node);
    }
    if (!err) {
        err = resolve_parent_locked(cwd, to, &new_dir, new_name);
    }
    if (!err && new_dir->ops->lookup(new_dir, new_name, &existing) == 0) {
        err = existing == node ? 0 : -EEXIST;
        if (!err && (new_dir != old_dir || strcmp(node->name, new_name) == 0 || !node->ops->rename)) {
            mutex_unlock(&vfs_lock);
            return 0;   /* renamed to itself */
        }
        if (!err) {
            /* Only the case changes, on a drive where "a" finds "A": by way of a free name, as two
               ordinary renames. */
            char temp[VFS_NAME_MAX];
            struct vnode *taken;
            int n = 0;
            do {
                ksnprintf(temp, sizeof(temp), ".rename-%d", n++);
            } while (n < 100 && old_dir->ops->lookup(old_dir, temp, &taken) == 0);
            err = node->ops->rename(old_dir, node, old_dir, temp);
            if (!err) {
                err = node->ops->rename(old_dir, node, old_dir, new_name);
                if (err) {
                    node->ops->rename(old_dir, node, old_dir, old_name);   /* put it back */
                }
            }
            if (!err) {
                old_dir->mtime = rtc_time();
            }
            mutex_unlock(&vfs_lock);
            return err;
        }
    }
    /* A folder can't move into itself or anything inside it. */
    for (struct vnode *p = new_dir; !err && p; p = p->parent) {
        if (p == node) {
            err = -EINVAL;
        }
    }
    if (!err && node->mounted) {
        err = -EBUSY;
    }
    if (!err && (old_dir->sb != new_dir->sb || old_dir->ops != new_dir->ops)) {
        err = -EXDEV;
    }
    if (!err && !node->ops->rename) {
        err = -EINVAL;
    }
    if (!err) {
        err = node->ops->rename(old_dir, node, new_dir, new_name);
    }
    if (!err) {
        old_dir->mtime = new_dir->mtime = rtc_time();
    }
    mutex_unlock(&vfs_lock);
    return err;
}

int vfs_unlink(struct vnode *cwd, const char *path) {
    mutex_lock(&vfs_lock);
    struct vnode *parent, *node;
    char name[VFS_NAME_MAX];
    int err = resolve_parent_locked(cwd, path, &parent, name);
    if (!err) {
        err = parent->ops->lookup(parent, name, &node);
    }
    if (!err && node->mounted) {
        err = -EBUSY;
    }
    if (!err) {
        err = parent->ops->unlink(parent, node);
    }
    if (!err) {
        parent->mtime = rtc_time();
        node->unlinked = true;
        node->parent = NULL;
        maybe_free(node);
    }
    mutex_unlock(&vfs_lock);
    return err;
}

/* File contents: a mounted filesystem's own lock, else the tree lock. */
static struct mutex *data_lock(struct vnode *n) {
    return n->sb ? &n->sb->lock : &vfs_lock;
}

int64_t vfs_read(struct vnode *n, uint64_t offset, void *buf, size_t len) {
    if (n->type == VNODE_DIR) {
        return -EISDIR;
    }
    struct mutex *lock = data_lock(n);
    mutex_lock(lock);
    int64_t r = n->ops->read(n, offset, buf, len);
    mutex_unlock(lock);
    return r;
}

int64_t vfs_write(struct vnode *n, uint64_t offset, const void *buf, size_t len) {
    if (n->type == VNODE_DIR) {
        return -EISDIR;
    }
    struct mutex *lock = data_lock(n);
    mutex_lock(lock);
    int64_t r = n->ops->write(n, offset, buf, len);
    if (r > 0) {
        n->mtime = rtc_time();
    }
    mutex_unlock(lock);
    return r;
}

int vfs_truncate(struct vnode *n, uint64_t size) {
    if (n->type == VNODE_DIR) {
        return -EISDIR;
    }
    struct mutex *lock = data_lock(n);
    mutex_lock(lock);
    int r = n->ops->truncate(n, size);
    if (r == 0) {
        n->mtime = rtc_time();
    }
    mutex_unlock(lock);
    return r;
}

int vfs_readdir(struct vnode *dir, size_t index, struct vnode **out) {
    if (dir->type != VNODE_DIR) {
        return -ENOTDIR;
    }
    mutex_lock(&vfs_lock);
    int err = dir->ops->readdir(dir, index, out);
    if (!err) {
        (*out)->refcount++;
    }
    mutex_unlock(&vfs_lock);
    return err;
}

void vfs_path(struct vnode *n, char *buf, size_t size) {
    mutex_lock(&vfs_lock);
    if (n->unlinked) {
        strlcpy(buf, "(deleted)", size);
    } else if (n == root || !n->parent) {
        strlcpy(buf, "/", size);
    } else {
        /* How long the path is, then fill it in from the end. */
        size_t total = 0;
        for (struct vnode *p = n; p && p != root; p = p->parent) {
            total += 1 + strlen(p->name);
        }
        if (total + 1 > size) {
            strlcpy(buf, "(path too long)", size);
        } else {
            size_t end = total;
            buf[end] = '\0';
            for (struct vnode *p = n; p && p != root; p = p->parent) {
                size_t len = strlen(p->name);
                end -= len;
                memcpy(buf + end, p->name, len);
                buf[--end] = '/';
            }
        }
    }
    mutex_unlock(&vfs_lock);
}

/* --- mounts ---------------------------------------------------------------------------------------------- */

int vfs_attach(const char *path, struct vnode *fs_root) {
    mutex_lock(&vfs_lock);
    struct vnode *dir, *child;
    int err = resolve_locked(NULL, path, &dir);
    if (!err && (dir->type != VNODE_DIR || dir->mounted)) {
        err = -EBUSY;
    }
    if (!err && dir->ops->readdir(dir, 0, &child) == 0) {
        err = -ENOTEMPTY;
    }
    if (!err) {
        dir->mounted = fs_root;
        fs_root->parent = dir->parent;
        strlcpy(fs_root->name, dir->name, sizeof(fs_root->name));
        fs_root->refcount++;   /* the mount's own reference */
        dir->refcount++;
    }
    mutex_unlock(&vfs_lock);
    return err;
}

void vfs_detach(struct vnode *mountpoint) {
    mutex_lock(&vfs_lock);
    struct vnode *fs_root = mountpoint->mounted;
    if (fs_root) {
        mountpoint->mounted = NULL;
        fs_root->refcount--;
        mountpoint->refcount--;
        maybe_free(mountpoint);
    }
    mutex_unlock(&vfs_lock);
}

void vfs_lock_tree(void) {
    mutex_lock(&vfs_lock);
}

void vfs_unlock_tree(void) {
    mutex_unlock(&vfs_lock);
}
