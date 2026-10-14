#pragma once

#include <stdint.h>

struct vnode;

/* In-memory filesystem: every file's contents live in a heap buffer. */
struct vnode *ramfs_create_root(void);

struct ramfs_stats {
    uint64_t files;
    uint64_t directories;
    uint64_t bytes;       /* file contents */
};
void ramfs_get_stats(struct ramfs_stats *out);
