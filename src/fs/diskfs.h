#pragma once

/*
 * diskfs: what the disk filesystems (FAT, exFAT, ext4, NTFS) have in common,
 * so each driver only deals with its on-disk format.
 *
 * Every file and folder a program has looked at is a dnode in memory, under
 * its parent. A folder's entries are read from the disk the first time
 * someone looks inside (ops->list), then kept: lookups and listings are
 * answered from memory, and creating/deleting/renaming updates both the disk
 * (through the driver) and the in-memory list. The mount's lock (m->sb) is
 * held around every call into the driver, so drivers needn't lock anything.
 *
 * After creating, deleting or renaming something the block cache is synced
 * right away; file contents within half a second (the flusher in mount.c),
 * so a drive pulled out a moment after a change has it.
 */

#include "fs/mount.h"
#include "fs/vfs.h"

struct dnode {
    struct vnode v;              /* first: the VFS's vnode is the dnode */
    struct mount *m;
    struct dnode *children;      /* folders: the entries, once listed */
    struct dnode *next;          /* the next entry in the parent */
    bool listed;
    void *priv;                  /* the driver's data (where it is on disk) */
};

struct diskfs_ops {
    /* Read the folder's entries from disk: diskfs_add() each one. */
    int (*list)(struct dnode *dir);
    int64_t (*read)(struct dnode *f, uint64_t offset, void *buf, size_t len);
    /* The rest may be NULL on a read-only filesystem. */
    int64_t (*write)(struct dnode *f, uint64_t offset, const void *buf, size_t len);
    int (*truncate)(struct dnode *f, uint64_t size);
    /* Make the entry `node` (name and type already set) in `dir` on disk; set node->priv. */
    int (*create)(struct dnode *dir, struct dnode *node);
    /* Remove `child` (an empty folder or a file) from `dir` on disk and free its space. */
    int (*unlink)(struct dnode *dir, struct dnode *child);
    /* Move `child` from `dir` to `new_dir` as `new_name` (free there) on disk. */
    int (*rename)(struct dnode *dir, struct dnode *child, struct dnode *new_dir, const char *new_name);
    /* Free node->priv (the dnode itself is freed by diskfs). */
    void (*release)(struct dnode *n);
    /* Store new permission bits on disk; NULL: the filesystem has none (only kept in memory). */
    int (*set_mode)(struct dnode *n, uint32_t mode);
};

/* A new node for `m` (not in any folder yet). Folders get 0755, files 0644 (no write bits if read-only). */
struct dnode *diskfs_new(struct mount *m, const char *name, enum vnode_type type, uint64_t size, void *priv);
/* Put `child` into `dir`'s list (from ops->list). */
void diskfs_add(struct dnode *dir, struct dnode *child);
/* Compare names the way this filesystem does (ASCII case folded if it's case-insensitive). */
bool diskfs_same_name(struct mount *m, const char *a, const char *b);
/* Free a whole unreferenced tree (unmount); false if something still uses part of it. */
bool diskfs_free_tree(struct dnode *root);

/* --- helpers for drivers ------------------------------------------------------------------------------- */

/* UTF-16LE <-> UTF-8 (FAT long names, exFAT, NTFS). Returns the output length; stops at a NUL. */
size_t utf16_to_utf8(const uint16_t *in, size_t count, char *out, size_t size);
/* Returns the number of UTF-16 units written, or -1 if it didn't fit / the input is bad. */
int utf8_to_utf16(const char *in, uint16_t *out, size_t max);
/* Seconds since 1970 <-> a broken-down local time, for the FAT/exFAT timestamp formats. */
struct dtime {
    int year, month, day, hour, minute, second;
};
void dtime_from_unix(uint64_t t, struct dtime *out);
uint64_t dtime_to_unix(const struct dtime *t);

static inline uint16_t get16(const void *p) {
    const uint8_t *b = p;
    return (uint16_t)(b[0] | b[1] << 8);
}
static inline uint32_t get32(const void *p) {
    const uint8_t *b = p;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static inline uint64_t get64(const void *p) {
    return get32(p) | (uint64_t)get32((const uint8_t *)p + 4) << 32;
}
static inline void put16(void *p, uint16_t v) {
    uint8_t *b = p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}
static inline void put32(void *p, uint32_t v) {
    uint8_t *b = p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}
static inline void put64(void *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32((uint8_t *)p + 4, (uint32_t)(v >> 32));
}
