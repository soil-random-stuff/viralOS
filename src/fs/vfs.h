#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sched/sync.h"

/*
 * Virtual File System: one directory tree made of vnodes. Each vnode is
 * backed by a filesystem driver through its `ops`: ramfs for the root, the
 * disk filesystems (fs/diskfs.c) for drives mounted under /media.
 *
 * Mounting: a directory's `mounted` points at another filesystem's root,
 * and path lookups step through. That root takes the mount point's name and
 * parent, so ".." and vfs_path work across the boundary.
 *
 * Lifetime: being in the tree doesn't keep a vnode alive, references do.
 * Functions that hand out a vnode take a reference the caller must drop
 * with vfs_put(). An unlinked vnode is freed when its last reference goes.
 *
 * All functions return 0 / a byte count on success or a negative errno.
 * Paths are absolute, or relative to `cwd` (NULL = the root).
 */

#define VFS_NAME_MAX 256   /* = OS_NAME_MAX: 255 bytes of UTF-8 + NUL */
#define VFS_PATH_MAX 1024  /* = OS_PATH_MAX */

enum vnode_type { VNODE_FILE, VNODE_DIR };

struct vnode;

/*
 * A mounted filesystem's own lock. Reading and writing its files takes only
 * this, not the tree lock, so a slow USB drive doesn't hold up everything
 * else. Folder operations take the tree lock, then (in the filesystem) this.
 */
struct vfs_sb {
    struct mutex lock;
};

struct vnode_ops {
    int64_t (*read)(struct vnode *n, uint64_t offset, void *buf, size_t len);
    int64_t (*write)(struct vnode *n, uint64_t offset, const void *buf, size_t len);
    int (*truncate)(struct vnode *n, uint64_t size);
    int (*lookup)(struct vnode *dir, const char *name, struct vnode **out);
    int (*readdir)(struct vnode *dir, size_t index, struct vnode **out);
    int (*create)(struct vnode *dir, const char *name, enum vnode_type type,
                  uint32_t mode, struct vnode **out);
    int (*unlink)(struct vnode *dir, struct vnode *child);
    /* Move `child` from `dir` into `new_dir` under `new_name` (which is free). */
    int (*rename)(struct vnode *dir, struct vnode *child, struct vnode *new_dir, const char *new_name);
    void (*release)(struct vnode *n);   /* free driver data; vnode is freed after */
};

struct vnode {
    char name[VFS_NAME_MAX];
    enum vnode_type type;
    uint32_t mode;           /* permission bits, e.g. 0644 */
    uint64_t size;           /* bytes, for files */
    uint64_t mtime;          /* seconds since 1970, 0 if unknown */
    uint64_t ctime;          /* created, same units */
    struct vnode *parent;    /* NULL for the root and for unlinked nodes */
    uint32_t refcount;
    bool unlinked;
    const struct vnode_ops *ops;
    void *fs_data;           /* driver-private */
    struct vfs_sb *sb;       /* which filesystem it's on (NULL: the root ramfs); renames can't cross */
    struct vnode *mounted;   /* a mount point: the root of what's mounted here */
};

void vfs_init(void);
struct vnode *vfs_root(void);

void vfs_get(struct vnode *n);
void vfs_put(struct vnode *n);

int vfs_resolve(struct vnode *cwd, const char *path, struct vnode **out);
int vfs_create(struct vnode *cwd, const char *path, enum vnode_type type, uint32_t mode,
               struct vnode **out);   /* `out` may be NULL */
int vfs_rename(struct vnode *cwd, const char *from, const char *to);
int vfs_unlink(struct vnode *cwd, const char *path);

int64_t vfs_read(struct vnode *n, uint64_t offset, void *buf, size_t len);
int64_t vfs_write(struct vnode *n, uint64_t offset, const void *buf, size_t len);
int vfs_truncate(struct vnode *n, uint64_t size);
int vfs_readdir(struct vnode *dir, size_t index, struct vnode **out);

/* Absolute path of `n` into buf. */
void vfs_path(struct vnode *n, char *buf, size_t size);

/* Mount `fs_root` on the directory at `path` (which must exist and be empty). */
int vfs_attach(const char *path, struct vnode *fs_root);
/* Undo it (lookups no longer reach the filesystem). */
void vfs_detach(struct vnode *mountpoint);
/* Hold the tree still (no operation runs meanwhile), e.g. to tear a filesystem down. */
void vfs_lock_tree(void);
void vfs_unlock_tree(void);
