#pragma once

#include "fs/mount.h"

/* The disk filesystems (fs/<name>/), tried in this order by mount_auto. */
extern const struct fs_driver ntfs_driver;    /* NTFS: read-only */
extern const struct fs_driver exfat_driver;   /* exFAT: read-write */
extern const struct fs_driver ext4_driver;    /* ext2/3/4: read-write (lwext4); read-only unless asked */
extern const struct fs_driver ext4_ro_driver; /* ext2/3/4: read-only, for what lwext4 won't take */
extern const struct fs_driver fat_driver;     /* FAT12/16/32 with long names: read-write */
