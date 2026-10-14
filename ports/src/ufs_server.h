/*
 * The program side of userfs (SYS_USERFS in <viralos.h>), shared by the read-write filesystem
 * servers (ext4d, ntfsd): they open their filesystem, fill in a struct os_userfs, and hand their
 * operations to ufs_serve(), which mounts it and answers the kernel's requests until it's unmounted.
 *
 * These are Linux (musl) programs: the system calls here go through OS_LINUX_NATIVE_BASE.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "viralos.h"

struct ufs_ops {
    /* Fill `buf` (size bytes) with os_userfs_entry records for the folder's entries from number
       `start` on; *count = how many, *more = 1 if some didn't fit. 0 or -errno. */
    int (*list)(uint64_t dir, uint64_t start, uint8_t *buf, size_t size, int64_t *count, int *more);
    int64_t (*read)(uint64_t node, uint64_t offset, void *buf, size_t len);
    int64_t (*write)(uint64_t node, uint64_t offset, const void *buf, size_t len);
    int (*truncate)(uint64_t node, uint64_t size);
    int (*create)(uint64_t dir, const char *name, int is_dir, uint64_t *node);
    int (*unlink)(uint64_t dir, uint64_t node, const char *name, int is_dir);
    int (*rename)(uint64_t dir, uint64_t node, const char *name, uint64_t new_dir, const char *new_name,
                  int is_dir);
    int (*statfs)(uint64_t *total, uint64_t *free);
    int (*sync)(void);
    /* Unmounting: write everything out and mark the filesystem clean, but close nothing (the
       program exits right after; see ufs_serve). */
    int (*destroy)(void);
};

/* While answering a request, a server must not open or close files: the kernel may be holding the
   lock that needs (it waits for the answer with folder operations under way). The device it opened
   before ufs_serve, reads, writes and fsync are fine. */

/* Add one entry to a LIST buffer at *used; 0 if it doesn't fit. */
int ufs_add_entry(uint8_t *buf, size_t size, size_t *used, uint64_t node, uint64_t bytes, int64_t mtime,
                  int is_dir, const char *name, size_t name_len);

/* Mount the filesystem `desc` describes and serve it; returns when it's been unmounted (0) or on error
   (-errno, before it was mounted). */
int ufs_serve(struct os_userfs *desc, const struct ufs_ops *ops);
