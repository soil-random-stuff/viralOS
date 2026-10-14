#pragma once

/*
 * Block devices: disks (USB sticks for now) and the partitions on them.
 *
 *   block.c      the registry, partitions (MBR with extended partitions, GPT),
 *                and the "disks" thread that mounts what it finds
 *   cache.c      a byte-addressed cache over the sectors, which is what the
 *                filesystems use: bio_read / bio_write / bio_sync
 *
 * A disk driver fills in a struct block_device and calls block_add_disk().
 * When the hardware goes away it calls block_disk_gone(): from then on every
 * request fails with -EIO and the disks thread unmounts what was on it.
 * Device structs are never freed (a filesystem may still point at one).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fs/vfs.h"

struct block_device {
    char name[16];               /* "usb0", "usb0p1" */
    char model[48];              /* "SanDisk Cruzer Blade" */
    char serial[40];             /* may be empty */
    char table[8];               /* disks: "gpt", "mbr" or "" (none / not read yet) */
    uint8_t type_guid[16];       /* GPT partitions: the partition type */
    uint8_t part_guid[16];       /* GPT partitions: its own GUID */
    char part_name[72];          /* GPT partitions: its name (UTF-8) */
    uint32_t sector_size;        /* 512 or 4096 */
    uint64_t sectors;
    bool read_only;
    bool removable;
    bool external;               /* plugged in from outside (USB), not built in */
    bool gone;                   /* unplugged: everything fails */
    int partition;               /* 0: the whole disk, 1..: a partition (number as on the disk) */
    struct block_device *disk;   /* the whole disk (itself, for a disk) */
    uint64_t start;              /* partitions: first sector on the disk */
    uint8_t part_type;           /* MBR type byte, 0 for GPT */

    /* Disk drivers fill these; `lba` and `count` are in sectors, buffers are ordinary kernel memory. */
    int (*read)(struct block_device *d, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct block_device *d, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct block_device *d);   /* may be NULL */
    void *priv;

    struct vnode node;           /* its file in /dev (fs/devfs.c) */
    struct block_device *next;   /* registry */
};

void block_init(void);
/* Before power off: unmount everything, write the caches out, tell the disks to save theirs. */
void block_shutdown(void);
/* A disk appeared: look for partitions and filesystems on it (in the disks thread). */
void block_add_disk(struct block_device *disk);
/* It's gone (unplugged): fail all I/O, unmount its filesystems. */
void block_disk_gone(struct block_device *disk);
/* Read the disk's partition table again (after it was rewritten): its old partitions go, the new
   ones are registered and mounted. -EBUSY if something on it is mounted. */
int block_rescan(struct block_device *disk);
/* The n-th registered device (disks and partitions, gone ones included); NULL past the end. */
struct block_device *block_at(int n);
/* A device that's still there, by name ("usb0p1"); NULL if none. */
struct block_device *block_find(const char *name);

/* Sector I/O on a disk or partition (bounds-checked, partition-relative). Bypasses the cache. */
int block_read(struct block_device *d, uint64_t lba, uint32_t count, void *buf);
int block_write(struct block_device *d, uint64_t lba, uint32_t count, const void *buf);

/* --- cache.c ---------------------------------------------------------------------------------------- */

/* Read/write `len` bytes at byte `offset` of a disk or partition, through the cache. Small writes
   stay in the cache (dirty) until bio_sync; big aligned ones go straight to the disk. */
int bio_read(struct block_device *d, uint64_t offset, void *buf, size_t len);
int bio_write(struct block_device *d, uint64_t offset, const void *buf, size_t len);
/* Write every dirty cached piece of this device's disk, then flush the disk's own cache. */
int bio_sync(struct block_device *d);
/* Forget everything cached for this disk (after it's gone, or unmounted). Dirty data is dropped. */
void bio_forget(struct block_device *disk);
void bio_stats(uint64_t *cached_bytes, uint64_t *dirty_bytes);
