#pragma once

/*
 * userfs: filesystems served by a program (see SYS_USERFS in <viralos.h>). NTFS and ext4 mounted
 * read-write are run this way, by ports of the Linux tools' libraries, instead of by the kernel's
 * read-only drivers.
 */

#include <viralos.h>

#include "block/block.h"

struct mount;
struct process;

/* The program that serves this kernel driver's filesystem read-write ("/usr/bin/ntfsd"), or NULL. */
const char *userfs_server_for(const char *driver_name);
/* Start `server` for `d` and wait until it has mounted it (at `path`, NULL for the usual place). */
int userfs_start(struct block_device *d, const char *server, const char *path);
/* SYS_USERFS: the calling program serves the filesystem described by `in`. Returns the channel's fd. */
int64_t userfs_register(struct process *p, const struct os_userfs *in, int32_t *buffer_fd);
/* (The flusher, now and then) If the program serving `m` has exited, unmount it. */
void userfs_check(struct mount *m);
