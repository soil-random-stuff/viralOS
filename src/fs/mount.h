#pragma once

/*
 * Mounted drives. The disks thread calls mount_auto() for every partition
 * it finds: each filesystem driver gets a look, the first that recognizes it
 * mounts it at /media/<label>. mount_disk_gone() unmounts what was on a
 * disk that was pulled out; mount_eject() does it cleanly on request.
 */

#include <stdbool.h>
#include <stdint.h>

#include "fs/vfs.h"

struct block_device;
struct mount;
struct dnode;

/* What a disk filesystem implements (see fs/diskfs.h for the node operations). */
struct fs_driver {
    const char *name;   /* "fat", "exfat", "ext4", "ntfs" */
    /* Recognize and mount m->dev: fill m->fs_name, m->label, m->fs, m->ops, m->root (diskfs_new_root),
       set m->read_only / m->case_insensitive as needed. Return 0, or a negative errno (-EINVAL: not mine). */
    int (*mount)(struct mount *m);
    /* Write back whatever is pending (unless m->dead) and free the driver's state. */
    void (*unmount)(struct mount *m);
    /* Size and free space, in bytes. */
    int (*statfs)(struct mount *m, uint64_t *total, uint64_t *free);
    /* Write out what the driver itself holds (before the block cache is synced); may be NULL. */
    int (*sync)(struct mount *m);
    /* Mounted read-only to begin with (another system's disk), it may switch to read-write when the
       mount options say so or it's the installed system's own /. 0 or -errno; may be NULL. */
    int (*make_writable)(struct mount *m);
};

struct process;

struct mount {
    struct vfs_sb sb;           /* its lock (every dnode's vnode.sb points here) */
    struct block_device *dev;
    const struct fs_driver *driver;
    const struct diskfs_ops *ops;
    char fs_name[16];           /* shown to the user: "FAT32", "exFAT", "ext4", "NTFS" */
    char label[VFS_NAME_MAX];   /* the volume label (UTF-8), may be empty */
    char path[VFS_PATH_MAX];    /* "/media/<name>" */
    char uuid[40];              /* the filesystem's serial/UUID as blkid shows it, may be empty */
    bool created_dir;           /* the mount point was made for it (and goes when it's unmounted) */
    struct dnode *root;
    struct vnode *mountpoint;
    bool read_only;
    bool case_insensitive;      /* FAT, exFAT, NTFS: "Readme.TXT" finds "README.txt" */
    bool dead;                  /* unplugged or ejected: every operation fails */
    bool is_root;               /* mounted as / (the installed system): never unmounted by hand */
    bool dirty;                 /* file data written since the last sync */
    bool journaled;             /* changes go through a journal: no full sync after each one (the flusher does it) */
    uint64_t server_pid;        /* userfs: the program serving it (it may write the device); 0: none */
    void *fs;                   /* driver-private */
    struct mount *next;
};

/* Start the flusher thread: syncs drives with written file data every half second. */
void mount_init(void);
void mount_auto(struct block_device *d);
/* Mount a device (by hand: the Disks app) at `path`, or NULL for where it would go by itself. */
int mount_device(struct block_device *d, const char *path, bool writable);
/* Unmount the one filesystem mounted at `path`: -EBUSY if a program has a file open on it.
   mount_unmount_forced: even then (its files fail with EIO from then on). */
int mount_unmount(const char *path);
/* The installed system: the filesystem with this UUID ("root=UUID=..." on the kernel command
   line) becomes / when its disk shows up; set before the disks are looked at. mount_wait_root:
   false if it hasn't shown up after `ms`. */
void mount_set_root(const char *uuid);
bool mount_wait_root(uint64_t ms);
struct mount *mount_root_fs(void);   /* NULL: / is the RAM filesystem */
/* Still looking for the root? Then a partition that's otherwise left alone (an EFI System Partition)
   gets mount_try_root: mounted as / if it's the one, else not at all. */
bool mount_wants_root(void);
void mount_try_root(struct block_device *d);
int mount_unmount_forced(const char *path);
/* Before power off: unmount everything cleanly. */
void mount_unmount_all(void);
/* May the current process write [offset, offset+len) of `d` (bytes)? Not if a filesystem there is mounted,
   unless the process is the one serving it. */
bool mount_write_allowed(struct block_device *d, uint64_t offset, uint64_t len);
/* userfs: put a filesystem that's ready (m->driver, ops, root, names set) into the tree at `path`
   (NULL/"": the configured place or /media/<label>). */
int mount_attach(struct mount *m, const char *path);
/* Is the device (or, for a whole disk, anything on it) mounted? */
bool mount_busy(struct block_device *d);
/* Does [offset, offset+len) of the disk (bytes) touch a mounted filesystem on it? */
bool mount_overlaps(struct block_device *disk, uint64_t offset, uint64_t len);
/* The mount of exactly this device, or NULL. */
struct mount *mount_of(struct block_device *d);
void mount_disk_gone(struct block_device *disk);
/* Unmount everything on the disk that holds the drive mounted at `path` (sync first). */
int mount_eject(const char *path);
/* The n-th live mount; NULL past the end. */
struct mount *mount_at(int n);
