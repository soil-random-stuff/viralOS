#pragma once

/*
 * /dev: one file per disk and partition ("usb0", "usb0p1"), for the
 * disk tools (mkfs, partitioning, the Disks app). Reading and writing goes
 * through the block cache, so it stays in step with the mounted
 * filesystems; writing to something mounted (or a disk with a mounted
 * partition) fails with EBUSY.
 */

#include <stdbool.h>

struct block_device;
struct vnode;

void devfs_init(void);
/* Give a newly registered device its file (block.c calls this). */
void devfs_add(struct block_device *d);
/* The device behind a /dev file, or NULL for any other vnode. */
struct block_device *devfs_device(struct vnode *n);
