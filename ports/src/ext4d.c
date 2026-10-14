/*
 * ext4d: ext2/3/4 read-write, for viralOS's userfs (see ufs_server.h). The kernel starts it as
 * "ext4d <device> <mount path or "">" when a filesystem's mount options say "rw".
 *
 * The filesystem work is e2fsprogs' libext2fs; what to do for each operation follows
 * e2fsprogs/misc/fuse2fs.c (op_create, op_mkdir, __op_unlink, __op_rmdir, op_rename,
 * fuse2fs_truncate, op_read, op_write, op_statfs, fuse2fs_mount, op_destroy), minus the FUSE parts.
 * Like fuse2fs it doesn't write the journal: the filesystem is marked "not clean" while it's mounted,
 * so a crash makes e2fsck check it, and a journal that needs recovery is refused (e2fsck first).
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ext2fs/ext2_fs.h"
#include "ext2fs/ext2fs.h"
#include "et/com_err.h"
#include "uuid/uuid.h"

#include "ufs_server.h"

static ext2_filsys fs;
static time_t last_flush;
static int changed;               /* metadata changed since the last full flush */

#define FLUSH_EVERY 5             /* seconds between full flushes (bitmaps, group descriptors) */

/* fuse2fs __translate_error, the errno part. */
static int translate(errcode_t err) {
    switch (err) {
    case 0:                          return 0;
    case EXT2_ET_NO_MEMORY:          return -ENOMEM;
    case EXT2_ET_INVALID_ARGUMENT:
    case EXT2_ET_LLSEEK_FAILED:      return -EINVAL;
    case EXT2_ET_NO_DIRECTORY:       return -ENOTDIR;
    case EXT2_ET_FILE_NOT_FOUND:     return -ENOENT;
    case EXT2_ET_DIR_NO_SPACE:
    case EXT2_ET_TOOSMALL:
    case EXT2_ET_BLOCK_ALLOC_FAIL:
    case EXT2_ET_INODE_ALLOC_FAIL:
    case EXT2_ET_EA_NO_SPACE:        return -ENOSPC;
    case EXT2_ET_FILE_TOO_BIG:       return -EFBIG;
    case EXT2_ET_FILE_EXISTS:        return -EEXIST;
    case EXT2_ET_RO_FILSYS:          return -EROFS;
    case EXT2_ET_UNIMPLEMENTED:      return -EOPNOTSUPP;
    default:
        if (err < 256) {
            return -(int)err;
        }
        fprintf(stderr, "ext4d: %s\n", error_message(err));
        return -EIO;
    }
}

static errcode_t read_inode(ext2_ino_t ino, struct ext2_inode_large *inode) {
    memset(inode, 0, sizeof(*inode));
    return ext2fs_read_inode_full(fs, ino, EXT2_INODE(inode), sizeof(*inode));
}

static errcode_t write_inode(ext2_ino_t ino, struct ext2_inode_large *inode) {
    return ext2fs_write_inode_full(fs, ino, EXT2_INODE(inode), sizeof(*inode));
}

static uint64_t inode_size(const struct ext2_inode_large *inode) {
    return inode->i_size | (uint64_t)inode->i_size_high << 32;
}

/* fuse2fs init_times / update_mtime / update_ctime (seconds only). The low 32 bits in the field, the
   epoch bits in its _extra (EXT4_INODE_SET_XTIME), when the inode is big enough to have one.
   (libext2fs's ext2fs_inode_xtime_set keeps only 28 bits of the seconds.) */
#define HAS_EXTRA(inode, field) \
    (EXT2_GOOD_OLD_INODE_SIZE + (inode)->i_extra_isize >= offsetof(struct ext2_inode_large, field) + 4)
#define SET_XTIME(inode, field, t)                                                  \
    do {                                                                            \
        (inode)->field = (__u32)(t);                                                \
        if (HAS_EXTRA(inode, field##_extra)) {                                      \
            (inode)->field##_extra = (__u32)(((int64_t)(t) - (__s32)(t)) >> 32) & EXT4_EPOCH_MASK; \
        }                                                                           \
    } while (0)

static void set_times(struct ext2_inode_large *inode, int a, int c, int m) {
    time_t now = time(NULL);
    if (a) {
        SET_XTIME(inode, i_atime, now);
    }
    if (c) {
        SET_XTIME(inode, i_ctime, now);
    }
    if (m) {
        SET_XTIME(inode, i_mtime, now);
    }
    if (a && c && m && HAS_EXTRA(inode, i_crtime_extra)) {
        SET_XTIME(inode, i_crtime, now);   /* a new inode: created now */
    }
}

static int touch(ext2_ino_t ino, int mtime) {
    struct ext2_inode_large inode;
    errcode_t err = read_inode(ino, &inode);
    if (!err) {
        set_times(&inode, 0, 1, mtime);
        err = write_inode(ino, &inode);
    }
    return translate(err);
}

/* fuse2fs fs_can_allocate: keep the reserved blocks, as libext2fs can corrupt a completely full fs. */
static int can_allocate(blk64_t num) {
    blk64_t reserved = ext2fs_r_blocks_count(fs->super);
    if (!reserved) {
        reserved = ext2fs_blocks_count(fs->super) / 10;
    }
    return num <= ext2fs_blocks_count(fs->super) && ext2fs_free_blocks_count(fs->super) > reserved + num;
}

static void set_extra_isize(struct ext2_inode_large *inode) {
    size_t extra = sizeof(struct ext2_inode_large) - EXT2_GOOD_OLD_INODE_SIZE;
    if (ext2fs_has_feature_extra_isize(fs->super)) {
        if (fs->super->s_want_extra_isize > extra) {
            extra = fs->super->s_want_extra_isize;
        }
        if (fs->super->s_min_extra_isize > extra) {
            extra = fs->super->s_min_extra_isize;
        }
    }
    inode->i_extra_isize = (__u16)extra;
}

/* --- operations --------------------------------------------------------------------------------------- */

struct list_state {
    uint64_t start, index;
    uint8_t *buf;
    size_t size, used;
    int64_t count;
    int more;
};

static int list_entry(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent, int offset, int blocksize,
                      char *block, void *data) {
    (void)dir, (void)entry, (void)offset, (void)blocksize, (void)block;
    struct list_state *s = data;
    int len = ext2fs_dirent_name_len(dirent);
    if (!dirent->inode || (len == 1 && dirent->name[0] == '.') ||
        (len == 2 && dirent->name[0] == '.' && dirent->name[1] == '.')) {
        return 0;
    }
    if (s->index++ < s->start) {
        return 0;
    }
    struct ext2_inode_large inode;
    if (read_inode(dirent->inode, &inode)) {
        return 0;
    }
    int is_dir = LINUX_S_ISDIR(inode.i_mode);
    if (!is_dir && !LINUX_S_ISREG(inode.i_mode)) {
        return 0;   /* links, devices, pipes: not something the OS can open */
    }
    if (!ufs_add_entry(s->buf, s->size, &s->used, dirent->inode, is_dir ? 0 : inode_size(&inode),
                       ext2fs_inode_xtime_get(&inode, i_mtime), is_dir, dirent->name, (size_t)len)) {
        s->more = 1;
        return DIRENT_ABORT;
    }
    s->count++;
    return 0;
}

static int e_list(uint64_t dir, uint64_t start, uint8_t *buf, size_t size, int64_t *count, int *more) {
    struct list_state s = { .start = start, .buf = buf, .size = size };
    errcode_t err = ext2fs_dir_iterate2(fs, (ext2_ino_t)dir, 0, NULL, list_entry, &s);
    *count = s.count;
    *more = s.more;
    return translate(err);
}

static int64_t e_read(uint64_t node, uint64_t offset, void *buf, size_t len) {
    ext2_file_t f;
    errcode_t err = ext2fs_file_open(fs, (ext2_ino_t)node, 0, &f);
    if (err) {
        return translate(err);
    }
    size_t done = 0;
    err = ext2fs_file_llseek(f, offset, EXT2_SEEK_SET, NULL);
    while (!err && done < len) {
        unsigned int got = 0;
        err = ext2fs_file_read(f, (char *)buf + done, (unsigned int)(len - done), &got);
        if (!got) {
            break;
        }
        done += got;
    }
    ext2fs_file_close(f);
    return done ? (int64_t)done : translate(err);
}

static int64_t e_write(uint64_t node, uint64_t offset, const void *buf, size_t len) {
    if (!can_allocate((len + fs->blocksize - 1) / fs->blocksize)) {
        return -ENOSPC;
    }
    ext2_file_t f;
    errcode_t err = ext2fs_file_open(fs, (ext2_ino_t)node, EXT2_FILE_WRITE, &f);
    if (err) {
        return translate(err);
    }
    unsigned int got = 0;
    err = ext2fs_file_llseek(f, offset, EXT2_SEEK_SET, NULL);
    if (!err) {
        err = ext2fs_file_write(f, buf, (unsigned int)len, &got);
    }
    if (!err) {
        err = ext2fs_file_flush(f);
    }
    errcode_t cerr = ext2fs_file_close(f);
    if (!err) {
        err = cerr;
    }
    if (err) {
        return got ? (int64_t)got : translate(err);
    }
    changed = 1;
    touch((ext2_ino_t)node, 1);
    return got;
}

static int e_truncate(uint64_t node, uint64_t size) {
    ext2_file_t f;
    errcode_t err = ext2fs_file_open(fs, (ext2_ino_t)node, EXT2_FILE_WRITE, &f);
    if (err) {
        return translate(err);
    }
    err = ext2fs_file_set_size2(f, size);
    errcode_t cerr = ext2fs_file_close(f);
    if (err || cerr) {
        return translate(err ? err : cerr);
    }
    changed = 1;
    return touch((ext2_ino_t)node, 1);
}

static int e_create(uint64_t dir, const char *name, int is_dir, uint64_t *node) {
    ext2_ino_t parent = (ext2_ino_t)dir, child;
    if (!can_allocate(1)) {
        return -ENOSPC;
    }
    errcode_t err;
    struct ext2_inode_large inode;
    if (is_dir) {
        /* op_mkdir */
        err = ext2fs_mkdir2(fs, parent, 0, 0, EXT2FS_LINK_EXPAND, name, &child);
        if (err) {
            return translate(err);
        }
        err = read_inode(child, &inode);
        if (err) {
            return translate(err);
        }
        set_extra_isize(&inode);
        inode.i_mode = LINUX_S_IFDIR | 0755;
        inode.i_generation = (__u32)rand();
        set_times(&inode, 1, 1, 1);
        err = write_inode(child, &inode);
        if (!err && !(inode.i_flags & EXT4_INLINE_DATA_FL) && ext2fs_has_feature_metadata_csum(fs->super)) {
            /* The directory block's checksum covers i_generation: write it again. */
            char *block;
            blk64_t blk;
            err = ext2fs_new_dir_block(fs, child, parent, &block);
            if (!err) {
                err = ext2fs_bmap2(fs, child, EXT2_INODE(&inode), NULL, 0, 0, NULL, &blk);
                if (!err) {
                    err = ext2fs_write_dir_block4(fs, blk, block, 0, child);
                }
                ext2fs_free_mem(&block);
            }
        }
    } else {
        /* op_create */
        err = ext2fs_new_inode(fs, parent, LINUX_S_IFREG | 0644, 0, &child);
        if (err) {
            return translate(err);
        }
        err = ext2fs_link(fs, parent, name, child, EXT2_FT_REG_FILE | EXT2FS_LINK_EXPAND);
        if (err) {
            return translate(err);
        }
        memset(&inode, 0, sizeof(inode));
        inode.i_mode = LINUX_S_IFREG | 0644;
        inode.i_links_count = 1;
        set_extra_isize(&inode);
        if (ext2fs_has_feature_extents(fs->super)) {
            ext2_extent_handle_t handle;
            inode.i_flags &= ~EXT4_EXTENTS_FL;
            err = ext2fs_extent_open2(fs, child, EXT2_INODE(&inode), &handle);
            if (err) {
                return translate(err);
            }
            ext2fs_extent_free(handle);
        }
        err = ext2fs_write_new_inode(fs, child, EXT2_INODE(&inode));
        if (!err) {
            inode.i_generation = (__u32)rand();
            set_times(&inode, 1, 1, 1);
            err = write_inode(child, &inode);
        }
        if (!err) {
            ext2fs_inode_alloc_stats2(fs, child, 1, 0);
        }
    }
    if (err) {
        return translate(err);
    }
    changed = 1;
    touch(parent, 1);
    *node = child;
    return 0;
}

/* fuse2fs remove_inode: drop a link; free the inode and its blocks when none are left. */
static int remove_inode(ext2_ino_t ino) {
    struct ext2_inode_large inode;
    errcode_t err = read_inode(ino, &inode);
    if (err) {
        return translate(err);
    }
    if (LINUX_S_ISDIR(inode.i_mode)) {
        if (!ext2fs_dir_link_empty(EXT2_INODE(&inode))) {
            return -EUCLEAN;
        }
        inode.i_links_count = 0;
        ext2fs_set_dtime(fs, EXT2_INODE(&inode));
    } else {
        if (inode.i_links_count == 0) {
            return -EUCLEAN;
        }
        inode.i_links_count--;
        if (!inode.i_links_count) {
            ext2fs_set_dtime(fs, EXT2_INODE(&inode));
        }
    }
    set_times(&inode, 0, 1, 0);
    if (inode.i_links_count) {
        return translate(write_inode(ino, &inode));
    }
    err = ext2fs_free_ext_attr(fs, ino, &inode);
    if (!err && ext2fs_inode_has_valid_blocks2(fs, EXT2_INODE(&inode))) {
        err = ext2fs_punch(fs, ino, EXT2_INODE(&inode), NULL, 0, ~0ULL);
    }
    if (err) {
        return translate(err);
    }
    ext2fs_inode_alloc_stats2(fs, ino, -1, LINUX_S_ISDIR(inode.i_mode));
    return translate(write_inode(ino, &inode));
}

struct empty_check {
    int empty;
};

static int empty_entry(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent, int offset, int blocksize,
                       char *block, void *data) {
    (void)dir, (void)entry, (void)offset, (void)blocksize, (void)block;
    struct empty_check *c = data;
    int len = ext2fs_dirent_name_len(dirent);
    if (!dirent->inode || (len == 1 && dirent->name[0] == '.') ||
        (len == 2 && dirent->name[0] == '.' && dirent->name[1] == '.')) {
        return 0;
    }
    c->empty = 0;
    return DIRENT_ABORT;
}

static int e_unlink(uint64_t dir, uint64_t node, const char *name, int is_dir) {
    ext2_ino_t parent = (ext2_ino_t)dir, ino = (ext2_ino_t)node;
    if (is_dir) {
        /* __op_rmdir */
        struct empty_check c = { 1 };
        errcode_t err = ext2fs_dir_iterate2(fs, ino, 0, NULL, empty_entry, &c);
        if (err) {
            return translate(err);
        }
        if (!c.empty) {
            return -ENOTEMPTY;
        }
    }
    errcode_t err = ext2fs_unlink(fs, parent, name, 0, 0);
    if (err) {
        return translate(err);
    }
    int ret = remove_inode(ino);
    if (ret) {
        return ret;
    }
    if (is_dir) {
        /* The parent loses the subfolder's ".." link. */
        struct ext2_inode_large inode;
        err = read_inode(parent, &inode);
        if (!err) {
            ext2fs_dec_nlink(EXT2_INODE(&inode));
            set_times(&inode, 0, 1, 1);
            err = write_inode(parent, &inode);
        }
        if (err) {
            return translate(err);
        }
    } else {
        touch(parent, 1);
    }
    changed = 1;
    return 0;
}

struct dotdot {
    ext2_ino_t new_dotdot;
};

static int update_dotdot(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent, int offset, int blocksize,
                         char *block, void *data) {
    (void)dir, (void)entry, (void)offset, (void)blocksize, (void)block;
    struct dotdot *d = data;
    if (ext2fs_dirent_name_len(dirent) == 2 && dirent->name[0] == '.' && dirent->name[1] == '.') {
        dirent->inode = d->new_dotdot;
        return DIRENT_CHANGED | DIRENT_ABORT;
    }
    return 0;
}

/* op_rename, for a destination name that's free. */
static int e_rename(uint64_t dir, uint64_t node, const char *name, uint64_t new_dir, const char *new_name,
                    int is_dir) {
    ext2_ino_t from_dir = (ext2_ino_t)dir, to_dir = (ext2_ino_t)new_dir, ino = (ext2_ino_t)node;
    if (!can_allocate(5)) {
        return -ENOSPC;
    }
    struct ext2_inode_large inode;
    errcode_t err;
    if (is_dir && from_dir != to_dir) {
        err = read_inode(to_dir, &inode);
        if (err) {
            return translate(err);
        }
        if (ext2fs_dir_link_max(fs, &inode)) {
            return -EMLINK;
        }
    }
    err = ext2fs_link(fs, to_dir, new_name, ino, (is_dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE) | EXT2FS_LINK_EXPAND);
    if (err) {
        return translate(err);
    }
    if (is_dir && from_dir != to_dir) {
        struct dotdot d = { to_dir };
        err = ext2fs_dir_iterate2(fs, ino, 0, NULL, update_dotdot, &d);
        if (!err) {
            err = read_inode(from_dir, &inode);
        }
        if (!err) {
            ext2fs_dec_nlink(EXT2_INODE(&inode));
            err = write_inode(from_dir, &inode);
        }
        if (!err) {
            err = read_inode(to_dir, &inode);
        }
        if (!err) {
            ext2fs_inc_nlink(fs, EXT2_INODE(&inode));
            err = write_inode(to_dir, &inode);
        }
        if (err) {
            return translate(err);
        }
    }
    touch(ino, 0);
    touch(to_dir, 1);
    err = ext2fs_unlink(fs, from_dir, name, 0, 0);
    if (err) {
        return translate(err);
    }
    if (from_dir != to_dir) {
        touch(from_dir, 1);
    }
    changed = 1;
    return 0;
}

static int e_statfs(uint64_t *total, uint64_t *free) {
    blk64_t reserved = ext2fs_r_blocks_count(fs->super);
    blk64_t avail = ext2fs_free_blocks_count(fs->super);
    avail = avail > reserved ? avail - reserved : 0;
    *total = ext2fs_blocks_count(fs->super) * fs->blocksize;
    *free = avail * fs->blocksize;
    return 0;
}

/* Data and inodes go out at once (io_channel_flush); the bitmaps and group descriptors every few
   seconds while things change, and at the end (writing them is a lot on a big disk). */
static int e_sync(void) {
    errcode_t err = 0;
    if (changed && time(NULL) - last_flush >= FLUSH_EVERY) {
        err = ext2fs_flush2(fs, 0);
        last_flush = time(NULL);
        changed = 0;
    } else {
        err = io_channel_flush(fs->io);
    }
    return translate(err);
}

/* op_destroy: mark it clean again and write everything (ext2fs_close would write nothing more). */
static int e_destroy(void) {
    if (!fs) {
        return 0;
    }
    fs->super->s_state |= EXT2_VALID_FS;
    if (fs->super->s_error_count) {
        fs->super->s_state |= EXT2_ERROR_FS;
    }
    ext2fs_mark_super_dirty(fs);
    ext2fs_set_gdt_csum(fs);
    errcode_t err = ext2fs_flush2(fs, 0);
    if (err) {
        fprintf(stderr, "ext4d: while writing everything out: %s\n", error_message(err));
    }
    return translate(err);
}

static const struct ufs_ops ops = {
    .list = e_list, .read = e_read, .write = e_write, .truncate = e_truncate, .create = e_create,
    .unlink = e_unlink, .rename = e_rename, .statfs = e_statfs, .sync = e_sync, .destroy = e_destroy,
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: ext4d <device> [mount path]\n");
        return 2;
    }
    char devpath[64];
    snprintf(devpath, sizeof(devpath), "/dev/%s", argv[1]);
    add_error_table(&et_ext2_error_table);
    srand((unsigned int)time(NULL));

    errcode_t err = ext2fs_open2(devpath, NULL, EXT2_FLAG_64BITS | EXT2_FLAG_RW, 0, 0, unix_io_manager, &fs);
    if (err) {
        fprintf(stderr, "ext4d: %s: %s\n", devpath, error_message(err));
        return 1;
    }
    /* fuse2fs_mount */
    if (ext2fs_has_feature_journal_needs_recovery(fs->super)) {
        fprintf(stderr, "ext4d: %s: the journal needs recovering: check the filesystem first\n", devpath);
        ext2fs_close_free(&fs);
        return 1;
    }
    err = ext2fs_read_bitmaps(fs);
    if (err) {
        fprintf(stderr, "ext4d: %s: bitmaps: %s\n", devpath, error_message(err));
        ext2fs_close_free(&fs);
        return 1;
    }
    /* Not clean while it's mounted: a crash makes e2fsck look at it. */
    fs->super->s_mnt_count++;
    ext2fs_set_tstamp(fs->super, s_mtime, time(NULL));
    fs->super->s_state &= ~EXT2_VALID_FS;
    ext2fs_mark_super_dirty(fs);
    err = ext2fs_flush2(fs, 0);
    if (err) {
        fprintf(stderr, "ext4d: %s: %s\n", devpath, error_message(err));
        ext2fs_close_free(&fs);
        return 1;
    }
    last_flush = time(NULL);

    static struct os_userfs desc;
    snprintf(desc.device, sizeof(desc.device), "%s", argv[1]);
    snprintf(desc.path, sizeof(desc.path), "%s", argc > 2 ? argv[2] : "");
    snprintf(desc.fs_name, sizeof(desc.fs_name), "%s",
             ext2fs_has_feature_extents(fs->super) || ext2fs_has_feature_64bit(fs->super) ? "ext4"
             : ext2fs_has_feature_journal(fs->super) ? "ext3" : "ext2");
    memcpy(desc.label, fs->super->s_volume_name, sizeof(fs->super->s_volume_name));
    desc.label[sizeof(fs->super->s_volume_name)] = 0;
    uuid_unparse(fs->super->s_uuid, desc.uuid);
    desc.root = EXT2_ROOT_INO;
    int ret = ufs_serve(&desc, &ops);
    if (ret < 0) {
        e_destroy();
        return 1;
    }
    return 0;
}
