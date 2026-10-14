/*
 * ntfsd: NTFS read-write, for viralOS's userfs (see ufs_server.h). The kernel starts it as
 * "ntfsd <device> <mount path or "">" when a filesystem's mount options say "rw".
 *
 * The filesystem work is the ntfs-3g library as kept in ntfsprogs-plus (libntfs); each operation
 * does what ntfs-3g's FUSE driver (src/ntfs-3g.c: ntfs_fuse_readdir, _read, _write, _truncate,
 * _create, _rm, _rename, _statfs) does with it, minus FUSE. Mounting refuses a hibernated Windows
 * or a $LogFile left by Fast Startup (the kernel then mounts it read-only), and resets a $LogFile
 * that's only unclean, as ntfs-3g does by default.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "types.h"
#include "attrib.h"
#include "dir.h"
#include "inode.h"
#include "logging.h"
#include "ntfstime.h"
#include "unistr.h"
#include "volume.h"

#include "ufs_server.h"

static ntfs_volume *vol;
static int keep_fd = -1;   /* a second descriptor for the device: see n_destroy */

static int fail(int fallback) {
    return errno ? -errno : -fallback;
}

static ntfs_inode *open_node(uint64_t node) {
    return ntfs_inode_open(vol, (MFT_REF)node);
}

static uint64_t node_of(ntfs_inode *ni) {
    return MK_MREF(ni->mft_no, le16_to_cpu(ni->mrec->sequence_number));
}

/* A name as NTFS keeps it (UTF-16); free() the result. -1 if it can't be one. */
static int to_ucs(const char *name, ntfschar **out) {
    *out = NULL;
    int len = ntfs_mbstoucs(name, out);
    if (len <= 0 || len > NTFS_MAX_NAME_LEN) {
        free(*out);
        *out = NULL;
        errno = len <= 0 ? EINVAL : ENAMETOOLONG;
        return -1;
    }
    return len;
}

/* --- listing (ntfs_fuse_readdir / ntfs_fuse_filler) ------------------------------------------------ */

struct list_state {
    uint64_t start, index;
    uint8_t *buf;
    size_t size, used;
    int64_t count;
    int more;
    MFT_REF dir;
};

static int filldir(void *data, const ntfschar *name, const int name_len, const int name_type, const s64 pos,
                   const MFT_REF mref, const unsigned dt_type) {
    (void)pos;
    struct list_state *s = data;
    if (name_type == FILE_NAME_DOS) {
        return 0;   /* the 8.3 alias of a name listed anyway */
    }
    if (MREF(mref) < FILE_first_user && MREF(s->dir) == FILE_root) {
        return 0;   /* $MFT, $Bitmap and the other metadata files */
    }
    if (dt_type != NTFS_DT_DIR && dt_type != NTFS_DT_REG && dt_type != NTFS_DT_UNKNOWN) {
        return 0;   /* symlinks (reparse points other than folders) */
    }
    char *utf8 = NULL;
    int len = ntfs_ucstombs(name, name_len, &utf8, 0);
    if (len <= 0) {
        return 0;
    }
    if ((len == 1 && utf8[0] == '.') || (len == 2 && utf8[0] == '.' && utf8[1] == '.')) {
        free(utf8);
        return 0;
    }
    if (s->index++ < s->start) {
        free(utf8);
        return 0;
    }
    /* Size and modification time (ntfs_fuse_getattr). */
    uint64_t size = 0;
    int64_t mtime = 0;
    int is_dir = dt_type == NTFS_DT_DIR;
    ntfs_inode *ni = ntfs_inode_open(vol, mref);
    if (ni) {
        is_dir = (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0;
        mtime = ntfs2timespec(ni->last_data_change_time).tv_sec;
        if (!is_dir) {
            ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
            if (na) {
                size = (uint64_t)na->data_size;
                ntfs_attr_close(na);
            }
        }
        ntfs_inode_close(ni);
    }
    int fits = ufs_add_entry(s->buf, s->size, &s->used, mref, size, mtime, is_dir, utf8, (size_t)len);
    free(utf8);
    if (!fits) {
        s->more = 1;
        return -1;   /* stop */
    }
    s->count++;
    return 0;
}

static int n_list(uint64_t dir, uint64_t start, uint8_t *buf, size_t size, int64_t *count, int *more) {
    ntfs_inode *ni = open_node(dir);
    if (!ni) {
        return fail(ENOENT);
    }
    struct list_state s = { .start = start, .buf = buf, .size = size, .dir = dir };
    s64 pos = 0;
    errno = 0;
    int err = ntfs_readdir(ni, &pos, &s, filldir);
    if (err && !s.more) {
        err = fail(EIO);
    } else {
        err = 0;
    }
    ntfs_inode_close(ni);
    *count = s.count;
    *more = s.more;
    return err;
}

/* --- file data (ntfs_fuse_read, _write, _truncate) --------------------------------------------------- */

static int64_t n_read(uint64_t node, uint64_t offset, void *buf, size_t len) {
    ntfs_inode *ni = open_node(node);
    if (!ni) {
        return fail(ENOENT);
    }
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
        int err = fail(EIO);
        ntfs_inode_close(ni);
        return err;
    }
    int64_t done = 0;
    if ((s64)offset < na->data_size) {
        if ((s64)(offset + len) > na->data_size) {
            len = (size_t)(na->data_size - (s64)offset);
        }
        while ((size_t)done < len) {
            s64 n = ntfs_attr_pread(na, (s64)offset + done, (s64)len - done, (char *)buf + done);
            if (n <= 0) {
                if (!done) {
                    done = fail(EIO);
                }
                break;
            }
            done += n;
        }
    }
    ntfs_attr_close(na);
    ntfs_inode_close(ni);
    return done;
}

static int64_t n_write(uint64_t node, uint64_t offset, const void *buf, size_t len) {
    ntfs_inode *ni = open_node(node);
    if (!ni) {
        return fail(ENOENT);
    }
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
        int err = fail(EIO);
        ntfs_inode_close(ni);
        return err;
    }
    int64_t done = 0;
    while ((size_t)done < len) {
        errno = 0;
        s64 n = ntfs_attr_pwrite(na, (s64)offset + done, (s64)len - done, (const char *)buf + done);
        if (n <= 0) {
            if (!done) {
                done = fail(ENOSPC);
            }
            break;
        }
        done += n;
    }
    if (done > 0) {
        ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
    }
    ntfs_attr_close(na);
    if (ntfs_inode_close(ni) && done >= 0) {
        done = fail(EIO);
    }
    return done;
}

static int n_truncate(uint64_t node, uint64_t size) {
    ntfs_inode *ni = open_node(node);
    if (!ni) {
        return fail(ENOENT);
    }
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    int err = 0;
    if (!na) {
        err = fail(EIO);
    } else {
        errno = 0;
        if (ntfs_attr_truncate(na, (s64)size)) {
            err = fail(EIO);
        } else {
            ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
        }
        ntfs_attr_close(na);
    }
    if (ntfs_inode_close(ni) && !err) {
        err = fail(EIO);
    }
    return err;
}

/* --- names (ntfs_fuse_create, ntfs_fuse_rm, ntfs_fuse_rename) ---------------------------------------- */

static int n_create(uint64_t dir, const char *name, int is_dir, uint64_t *node) {
    ntfschar *uname;
    int ulen = to_ucs(name, &uname);
    if (ulen < 0) {
        return fail(EINVAL);
    }
    ntfs_inode *dir_ni = open_node(dir);
    if (!dir_ni) {
        free(uname);
        return fail(ENOENT);
    }
    errno = 0;
    ntfs_inode *ni = ntfs_create(dir_ni, const_cpu_to_le32(0), uname, (u8)ulen, is_dir ? S_IFDIR : S_IFREG);
    int err = 0;
    if (!ni) {
        err = fail(EIO);
    } else {
        *node = node_of(ni);
        if (ntfs_inode_close_in_dir(ni, dir_ni)) {
            err = fail(EIO);
        }
        ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
    }
    if (ntfs_inode_close(dir_ni) && !err) {
        err = fail(EIO);
    }
    free(uname);
    return err;
}

/* Take the name `name` off `node` in `dir` (the node goes when it was its last name). */
static int remove_name(uint64_t dir, uint64_t node, const char *name) {
    ntfschar *uname;
    int ulen = to_ucs(name, &uname);
    if (ulen < 0) {
        return fail(EINVAL);
    }
    ntfs_inode *dir_ni = open_node(dir);
    ntfs_inode *ni = dir_ni ? open_node(node) : NULL;
    int err = 0;
    if (!dir_ni || !ni) {
        err = fail(ENOENT);
        if (dir_ni) {
            ntfs_inode_close(dir_ni);
        }
        free(uname);
        return err;
    }
    errno = 0;
    /* (ntfs_delete always closes both ni and dir_ni, and updates the folder's times) */
    if (ntfs_delete(vol, NULL, ni, dir_ni, uname, (u8)ulen)) {
        err = fail(EIO);
    }
    free(uname);
    return err;
}

static int n_unlink(uint64_t dir, uint64_t node, const char *name, int is_dir) {
    (void)is_dir;   /* (the kernel checked that a folder is empty; ntfs_delete checks too) */
    return remove_name(dir, node, name);
}

/* Give `node` the name `name` in `dir` too (ntfs_fuse_link). */
static int add_name(uint64_t node, uint64_t dir, const char *name) {
    ntfschar *uname;
    int ulen = to_ucs(name, &uname);
    if (ulen < 0) {
        return fail(EINVAL);
    }
    ntfs_inode *ni = open_node(node);
    ntfs_inode *dir_ni = ni ? open_node(dir) : NULL;
    int err = 0;
    if (!ni || !dir_ni) {
        err = fail(ENOENT);
    } else {
        errno = 0;
        if (ntfs_link(ni, dir_ni, uname, (u8)ulen)) {
            err = fail(EIO);
        } else {
            ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
            ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
        }
    }
    if (dir_ni && ntfs_inode_close(dir_ni) && !err) {
        err = fail(EIO);
    }
    if (ni && ntfs_inode_close(ni) && !err) {
        err = fail(EIO);
    }
    free(uname);
    return err;
}

static int same_ignoring_case(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a;
        char y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y) {
            return 0;
        }
    }
    return *a == *b;
}

/* ntfs_fuse_rename: link under the new name, then remove the old one. A change of case only, in the
   same folder, goes through a temporary name (the two names would be the same to NTFS). */
static int n_rename(uint64_t dir, uint64_t node, const char *name, uint64_t new_dir, const char *new_name,
                    int is_dir) {
    (void)is_dir;
    if (dir == new_dir && same_ignoring_case(name, new_name)) {
        char temp[300];
        snprintf(temp, sizeof(temp), "%s.ntfsd-%d", name, getpid());
        int err = add_name(node, dir, temp);
        if (!err) {
            err = remove_name(dir, node, name);
        }
        if (!err) {
            err = add_name(node, dir, new_name);
        }
        if (!err) {
            err = remove_name(dir, node, temp);
        }
        return err;
    }
    int err = add_name(node, new_dir, new_name);
    if (err) {
        return err;
    }
    err = remove_name(dir, node, name);
    if (err) {
        remove_name(new_dir, node, new_name);   /* undo */
    }
    return err;
}

/* --- the volume --------------------------------------------------------------------------------------- */

static int n_statfs(uint64_t *total, uint64_t *free) {
    if (vol->free_clusters < 0) {
        ntfs_volume_get_free_space(vol);
    }
    *total = (uint64_t)vol->nr_clusters * vol->cluster_size;
    *free = vol->free_clusters > 0 ? (uint64_t)vol->free_clusters * vol->cluster_size : 0;
    return 0;
}

static int n_sync(void) {
    /* MFT records are written when their inode is closed, data when it's written: what's left is the
       device's own cache. */
    if (vol->dev->d_ops->sync && vol->dev->d_ops->sync(vol->dev)) {
        return fail(EIO);
    }
    return 0;
}

/* ntfs_umount writes what's left (the bitmaps, the volume's flags) and closes the device. Its close
   must not be the device's last descriptor: closing that waits for the kernel, which is waiting for
   this answer (see ufs_server.h). keep_fd is a duplicate kept for that. */
static int n_destroy(void) {
    if (!vol) {
        return 0;
    }
    int err = ntfs_umount(vol, FALSE) ? fail(EIO) : 0;
    vol = NULL;
    return err;
}

static const struct ufs_ops ops = {
    .list = n_list, .read = n_read, .write = n_write, .truncate = n_truncate, .create = n_create,
    .unlink = n_unlink, .rename = n_rename, .statfs = n_statfs, .sync = n_sync, .destroy = n_destroy,
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: ntfsd <device> [mount path]\n");
        return 2;
    }
    char devpath[64];
    snprintf(devpath, sizeof(devpath), "/dev/%s", argv[1]);
    ntfs_log_set_handler(ntfs_log_handler_stderr);
    ntfs_log_set_levels(NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR | NTFS_LOG_LEVEL_CRITICAL |
                        NTFS_LOG_LEVEL_WARNING);

    /* The serial number from the boot sector: the UUID the kernel and the Disks app know it by. */
    unsigned char boot[512];
    int fd = open(devpath, O_RDONLY);
    if (fd < 0 || pread(fd, boot, sizeof(boot), 0) != (ssize_t)sizeof(boot)) {
        fprintf(stderr, "ntfsd: can't read %s\n", devpath);
        return 1;
    }
    close(fd);

    vol = ntfs_mount(devpath, NTFS_MNT_RECOVER);
    if (!vol) {
        fprintf(stderr, "ntfsd: %s: can't mount it read-write (%s)\n", devpath, strerror(errno));
        return 1;
    }
    if (NVolReadOnly(vol)) {
        fprintf(stderr, "ntfsd: %s: it can only be read now\n", devpath);
        ntfs_umount(vol, FALSE);
        return 1;
    }
    keep_fd = dup(*(int *)vol->dev->d_private);
    ntfs_volume_get_free_space(vol);

    static struct os_userfs desc;
    snprintf(desc.device, sizeof(desc.device), "%s", argv[1]);
    snprintf(desc.path, sizeof(desc.path), "%s", argc > 2 ? argv[2] : "");
    snprintf(desc.fs_name, sizeof(desc.fs_name), "NTFS");
    snprintf(desc.label, sizeof(desc.label), "%s", vol->vol_name ? vol->vol_name : "");
    unsigned int lo = boot[72] | boot[73] << 8 | boot[74] << 16 | (unsigned int)boot[75] << 24;
    unsigned int hi = boot[76] | boot[77] << 8 | boot[78] << 16 | (unsigned int)boot[79] << 24;
    snprintf(desc.uuid, sizeof(desc.uuid), "%08X%08X", hi, lo);
    desc.root = FILE_root;
    desc.flags = OS_USERFS_CASE_INSENSITIVE;
    int ret = ufs_serve(&desc, &ops);
    if (ret < 0) {
        n_destroy();
        return 1;
    }
    return 0;
}
