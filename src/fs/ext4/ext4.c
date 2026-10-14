/*
 * ext2/ext3/ext4, read-write, on lwext4 (fs/ext4/lwext4, by Grzegorz Kostka and others: extents,
 * hashed folders, metadata checksums and the journal). This file connects it to diskfs:
 *
 *   the disk      an ext4_blockdev whose reads and writes go to the kernel's block cache (bio_*);
 *                 on a writable filesystem lwext4's own cache holds changes back (write-back, much
 *                 faster) until lw_sync, which diskfs calls with every sync
 *   names         lwext4 works with paths under a mount point of its own ("/vos3/"): a node's
 *                 path is its names up to this filesystem's root
 *   files         an lwext4 file handle per open file node, opened when it's first read or written
 *
 * A filesystem mounts read-only (it's often another system's disk) and becomes writable when the
 * mount options say "rw" or it's the installed system's own / (make_writable): then a journal that
 * needs replaying is replayed, and changes go through the journal if there is one.
 * Filesystems lwext4 won't take (features it doesn't know) go to the read-only driver (ext4_ro.c).
 */

#include <ext4.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_super.h>

#include "fs/filesystems.h"
#include "fs/diskfs.h"
#include "block/block.h"
#include "drivers/rtc.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/sync.h"

#define SLOTS        CONFIG_EXT4_MOUNTPOINTS_COUNT
#define PATH_MAX_LW  1024

struct lw {
    struct ext4_blockdev_iface iface;
    struct ext4_blockdev bdev;
    struct block_device *dev;
    int slot;
    char name[16];               /* lwext4's name for the device: "vos3" */
    char mp[16];                 /* its mount point: "/vos3/" */
    bool writable, journal;
    uint8_t *sector;             /* lwext4's one-sector buffer */
};

/* A file or folder: an lwext4 handle for files, once opened. */
struct node {
    ext4_file file;
    bool open, open_rw;
    bool symlink;
};

static struct lw *slots[SLOTS];
static struct mutex slots_lock = MUTEX_INIT;   /* lwext4's device and mount tables are shared */

/* --- the disk ------------------------------------------------------------------------------------ */

static int bd_open(struct ext4_blockdev *bdev) {
    (void)bdev;
    return EOK;
}

static int bd_close(struct ext4_blockdev *bdev) {
    (void)bdev;
    return EOK;
}

static int bd_read(struct ext4_blockdev *bdev, void *buf, uint64_t blk, uint32_t count) {
    struct lw *l = bdev->bdif->p_user;
    uint32_t bs = bdev->bdif->ph_bsize;
    return bio_read(l->dev, blk * bs, buf, (size_t)count * bs) ? EIO : EOK;
}

static int bd_write(struct ext4_blockdev *bdev, const void *buf, uint64_t blk, uint32_t count) {
    struct lw *l = bdev->bdif->p_user;
    uint32_t bs = bdev->bdif->ph_bsize;
    return bio_write(l->dev, blk * bs, buf, (size_t)count * bs) ? EIO : EOK;
}

/* --- paths --------------------------------------------------------------------------------------- */

/* `n`'s path for lwext4: its mount point, then the names down from this filesystem's root. */
static void path_of(struct dnode *n, char *out, size_t size) {
    struct lw *l = n->m->fs;
    const char *names[64];
    int depth = 0;
    for (struct dnode *p = n; p && p != n->m->root && depth < 64; p = (struct dnode *)p->v.parent) {
        names[depth++] = p->v.name;
    }
    strlcpy(out, l->mp, size);
    for (int i = depth - 1; i >= 0; i--) {
        size_t len = strlen(out);
        ksnprintf(out + len, size - len, "%s%s", names[i], i ? "/" : "");
    }
}

static void child_path(struct dnode *dir, const char *name, char *out, size_t size) {
    path_of(dir, out, size);
    size_t len = strlen(out);
    ksnprintf(out + len, size - len, "%s%s", out[len - 1] == '/' ? "" : "/", name);
}

static int err_of(int r) {
    return r == EOK ? 0 : -(r < 0 ? -r : r);
}

/* --- files and folders -------------------------------------------------------------------------- */

static struct node *node_new(void) {
    return kzalloc(sizeof(struct node));
}

/* The file's handle, opened (read-write if the filesystem is) when first needed. */
static int handle(struct dnode *n, bool write, ext4_file **out) {
    struct node *x = n->priv;
    struct lw *l = n->m->fs;
    if (!x) {
        x = n->priv = node_new();
        if (!x) {
            return -ENOMEM;
        }
    }
    if (x->open && (x->open_rw || !write)) {
        *out = &x->file;
        return 0;
    }
    if (x->open) {
        ext4_fclose(&x->file);
        x->open = false;
    }
    static char path[PATH_MAX_LW];
    path_of(n, path, sizeof(path));
    int r = ext4_fopen2(&x->file, path, l->writable ? O_RDWR : O_RDONLY);
    if (r != EOK) {
        return err_of(r);
    }
    x->open = true;
    x->open_rw = l->writable;
    *out = &x->file;
    return 0;
}

static void set_from_inode(struct dnode *n, struct lw *l, uint32_t ino) {
    struct ext4_inode_ref ref;
    struct ext4_fs *fs = l->bdev.fs;
    if (!fs || ext4_fs_get_inode_ref(fs, ino, &ref) != EOK) {
        return;
    }
    uint32_t mode = ext4_inode_get_mode(&fs->sb, ref.inode);
    n->v.mode = (mode & 07777) & (l->writable ? 07777 : 07555);
    n->v.mtime = ext4_inode_get_modif_time(ref.inode);
    n->v.ctime = ext4_inode_get_change_inode_time(ref.inode);
    if (n->v.type == VNODE_FILE) {
        n->v.size = ext4_inode_get_size(&fs->sb, ref.inode);
    }
    ext4_fs_put_inode_ref(&ref);
}

static int lw_list(struct dnode *dir) {
    struct lw *l = dir->m->fs;
    static char path[PATH_MAX_LW];
    path_of(dir, path, sizeof(path));
    ext4_dir d;
    int r = ext4_dir_open(&d, path);
    if (r != EOK) {
        return err_of(r);
    }
    const ext4_direntry *de;
    while ((de = ext4_dir_entry_next(&d)) != NULL) {
        char name[256];
        memcpy(name, de->name, de->name_length);
        name[de->name_length] = '\0';
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || !de->inode) {
            continue;
        }
        bool dir_entry = de->inode_type == EXT4_DE_DIR;
        bool symlink = de->inode_type == EXT4_DE_SYMLINK;
        if (!dir_entry && !symlink && de->inode_type != EXT4_DE_REG_FILE) {
            continue;   /* devices, pipes, sockets: not shown */
        }
        struct node *x = node_new();
        struct dnode *n = diskfs_new(dir->m, name, dir_entry ? VNODE_DIR : VNODE_FILE, 0, x);
        if (!n) {
            kfree(x);
            break;
        }
        if (x) {
            x->symlink = symlink;
        }
        set_from_inode(n, l, de->inode);
        diskfs_add(dir, n);
    }
    ext4_dir_close(&d);
    return 0;
}

static int64_t lw_read(struct dnode *f, uint64_t offset, void *buf, size_t len) {
    struct node *x = f->priv;
    if (x && x->symlink) {
        /* (A symbolic link shows as a small file holding where it points.) */
        static char path[PATH_MAX_LW], target[PATH_MAX_LW];
        size_t n = 0;
        path_of(f, path, sizeof(path));
        int r = ext4_readlink(path, target, sizeof(target), &n);
        if (r != EOK) {
            return err_of(r);
        }
        if (offset >= n) {
            return 0;
        }
        len = len < n - offset ? len : n - offset;
        memcpy(buf, target + offset, len);
        return (int64_t)len;
    }
    ext4_file *h;
    int err = handle(f, false, &h);
    if (err) {
        return err;
    }
    size_t got = 0;
    int r = ext4_fseek(h, (int64_t)offset, SEEK_SET);
    if (r == EOK) {
        r = ext4_fread(h, buf, len, &got);
    }
    return r == EOK ? (int64_t)got : err_of(r);
}

static int64_t lw_write(struct dnode *f, uint64_t offset, const void *buf, size_t len) {
    ext4_file *h;
    int err = handle(f, true, &h);
    if (err) {
        return err;
    }
    size_t done = 0;
    int r = ext4_fseek(h, (int64_t)offset, SEEK_SET);
    if (r == EOK) {
        r = ext4_fwrite(h, buf, len, &done);
    }
    if (offset + done > f->v.size) {
        f->v.size = offset + done;
    }
    f->v.mtime = rtc_time();
    return r == EOK ? (int64_t)done : err_of(r);
}

static int lw_truncate(struct dnode *f, uint64_t size) {
    ext4_file *h;
    int err = handle(f, true, &h);
    if (err) {
        return err;
    }
    int r = ext4_ftruncate(h, size);
    if (r == EOK) {
        f->v.size = size;
        f->v.mtime = rtc_time();
    }
    return err_of(r);
}

static int lw_create(struct dnode *dir, struct dnode *node) {
    static char path[PATH_MAX_LW];
    child_path(dir, node->v.name, path, sizeof(path));
    struct node *x = node_new();
    if (!x) {
        return -ENOMEM;
    }
    int r;
    if (node->v.type == VNODE_DIR) {
        r = ext4_dir_mk(path);
    } else {
        r = ext4_fopen2(&x->file, path, O_RDWR | O_CREAT | O_TRUNC);
        x->open = x->open_rw = r == EOK;
    }
    if (r != EOK) {
        kfree(x);
        return err_of(r);
    }
    node->priv = x;
    uint64_t now = rtc_time();
    ext4_mtime_set(path, (uint32_t)now);
    ext4_ctime_set(path, (uint32_t)now);
    node->v.mtime = node->v.ctime = now;
    /* (lwext4 makes them 0777/0666: the usual 0755/0644, as a umask of 022 would.) */
    node->v.mode = node->v.type == VNODE_DIR ? 0755 : 0644;
    ext4_mode_set(path, node->v.mode);
    return 0;
}

static void close_node(struct dnode *n) {
    struct node *x = n->priv;
    if (x && x->open) {
        ext4_fclose(&x->file);
        x->open = false;
    }
}

static int lw_unlink(struct dnode *dir, struct dnode *child) {
    static char path[PATH_MAX_LW];
    child_path(dir, child->v.name, path, sizeof(path));
    close_node(child);
    int r = child->v.type == VNODE_DIR ? ext4_dir_rm(path) : ext4_fremove(path);
    return err_of(r);
}

static int lw_rename(struct dnode *dir, struct dnode *child, struct dnode *new_dir, const char *new_name) {
    static char from[PATH_MAX_LW], to[PATH_MAX_LW];
    child_path(dir, child->v.name, from, sizeof(from));
    child_path(new_dir, new_name, to, sizeof(to));
    close_node(child);   /* (reopened by its new path when needed) */
    int r = child->v.type == VNODE_DIR ? ext4_dir_mv(from, to) : ext4_frename(from, to);
    return err_of(r);
}

static int lw_set_mode(struct dnode *n, uint32_t mode) {
    static char path[PATH_MAX_LW];
    path_of(n, path, sizeof(path));
    return err_of(ext4_mode_set(path, mode));
}

static void lw_release(struct dnode *n) {
    close_node(n);
    kfree(n->priv);
    n->priv = NULL;
}

static const struct diskfs_ops lw_ops = {
    .list = lw_list,
    .read = lw_read,
    .write = lw_write,
    .truncate = lw_truncate,
    .create = lw_create,
    .unlink = lw_unlink,
    .rename = lw_rename,
    .release = lw_release,
    .set_mode = lw_set_mode,
};

/* --- mounting ----------------------------------------------------------------------------------- */

static void lw_free(struct lw *l) {
    mutex_lock(&slots_lock);
    if (l->slot >= 0) {
        slots[l->slot] = NULL;
    }
    mutex_unlock(&slots_lock);
    kfree(l->sector);
    kfree(l);
}

static int lw_mount(struct mount *m) {
    /* Not ext2/3/4: someone else's (quietly). */
    uint8_t magic[2];
    if (bio_read(m->dev, 1024 + 56, magic, 2) || magic[0] != 0x53 || magic[1] != 0xEF) {
        return -EINVAL;
    }
    struct lw *l = kzalloc(sizeof(*l));
    if (!l) {
        return -ENOMEM;
    }
    l->slot = -1;
    mutex_lock(&slots_lock);
    for (int i = 0; i < SLOTS && l->slot < 0; i++) {
        if (!slots[i]) {
            slots[i] = l;
            l->slot = i;
        }
    }
    mutex_unlock(&slots_lock);
    l->dev = m->dev;
    l->sector = kmalloc(m->dev->sector_size);
    if (l->slot < 0 || !l->sector) {
        lw_free(l);
        return -ENOMEM;
    }
    ksnprintf(l->name, sizeof(l->name), "vos%d", l->slot);
    ksnprintf(l->mp, sizeof(l->mp), "/vos%d/", l->slot);
    l->iface.open = bd_open;
    l->iface.bread = bd_read;
    l->iface.bwrite = bd_write;
    l->iface.close = bd_close;
    l->iface.ph_bsize = m->dev->sector_size;
    l->iface.ph_bcnt = m->dev->sectors;
    l->iface.ph_bbuf = l->sector;
    l->iface.p_user = l;
    l->bdev.bdif = &l->iface;
    l->bdev.part_offset = 0;
    l->bdev.part_size = m->dev->sectors * m->dev->sector_size;

    mutex_lock(&slots_lock);
    int r = ext4_device_register(&l->bdev, l->name);
    if (r == EOK) {
        r = ext4_mount(l->name, l->mp, true);
        if (r != EOK) {
            ext4_device_unregister(l->name);
        }
    }
    mutex_unlock(&slots_lock);
    if (r != EOK) {
        kprintf("       Disk: %s: ext4 features lwext4 doesn't handle (%d); trying the read-only driver\n",
                m->dev->name, r);
        lw_free(l);
        return -EINVAL;   /* (the next driver, ext4_ro, gets a go) */
    }

    struct ext4_sblock *sb = &l->bdev.fs->sb;
    const uint8_t *u = sb->uuid;
    ksnprintf(m->uuid, sizeof(m->uuid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0],
              u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    char label[17];
    memcpy(label, sb->volume_name, 16);
    label[16] = '\0';
    for (int i = 0; label[i]; i++) {
        if ((unsigned char)label[i] < 0x20) {
            label[i] = '_';
        }
    }
    strlcpy(m->label, label, sizeof(m->label));
    uint32_t incompat = ext4_get32(sb, features_incompatible);
    l->journal = ext4_get32(sb, features_compatible) & EXT4_FCOM_HAS_JOURNAL;
    strlcpy(m->fs_name, incompat & EXT4_FINCOM_EXTENTS ? "ext4" : l->journal ? "ext3" : "ext2", sizeof(m->fs_name));
    m->fs = l;
    m->ops = &lw_ops;
    m->read_only = true;
    m->root = diskfs_new(m, "", VNODE_DIR, 0, NULL);
    if (!m->root) {
        mutex_lock(&slots_lock);
        ext4_umount(l->mp);
        ext4_device_unregister(l->name);
        mutex_unlock(&slots_lock);
        lw_free(l);
        m->fs = NULL;
        return -ENOMEM;
    }
    set_from_inode(m->root, l, EXT4_INODE_ROOT_INDEX);
    return 0;
}

static int lw_make_writable(struct mount *m) {
    struct lw *l = m->fs;
    if (l->writable) {
        return 0;
    }
    mutex_lock(&slots_lock);
    int r = ext4_umount(l->mp);
    if (r == EOK) {
        r = ext4_mount(l->name, l->mp, false);
    }
    if (r == EOK && l->journal) {
        r = ext4_recover(l->mp);   /* (a journal left with changes in it: they go in first) */
        if (r == EOK || r == ENOTSUP) {
            r = ext4_journal_start(l->mp);
        }
    }
    if (r != EOK) {
        ext4_umount(l->mp);
        int back = ext4_mount(l->name, l->mp, true);   /* read-only again, as it was */
        (void)back;
    }
    mutex_unlock(&slots_lock);
    if (r != EOK) {
        return err_of(r);
    }
    ext4_cache_write_back(l->mp, true);   /* (flushed by lw_sync: after every change, and twice a second) */
    l->writable = true;
    m->read_only = false;
    m->journaled = l->journal;
    /* The nodes made so far were read-only: let them be written. */
    m->root->v.mode |= 0200;
    return 0;
}

static void lw_unmount(struct mount *m) {
    struct lw *l = m->fs;
    if (!l) {
        return;
    }
    mutex_lock(&slots_lock);
    if (l->writable) {
        ext4_cache_write_back(l->mp, false);   /* (writes out what's cached) */
    }
    if (l->writable && l->journal) {
        ext4_journal_stop(l->mp);
    }
    ext4_umount(l->mp);
    ext4_device_unregister(l->name);
    mutex_unlock(&slots_lock);
    lw_free(l);
    m->fs = NULL;
}

static int lw_statfs(struct mount *m, uint64_t *total, uint64_t *free) {
    struct lw *l = m->fs;
    struct ext4_mount_stats s;
    if (ext4_mount_point_stats(l->mp, &s) != EOK) {
        return -EIO;
    }
    *total = s.blocks_count * s.block_size;
    *free = s.free_blocks_count * s.block_size;
    return 0;
}

static int lw_sync(struct mount *m) {
    struct lw *l = m->fs;
    return l->writable ? err_of(ext4_cache_flush(l->mp)) : 0;
}

const struct fs_driver ext4_driver = {
    .name = "ext4",
    .mount = lw_mount,
    .unmount = lw_unmount,
    .statfs = lw_statfs,
    .sync = lw_sync,
    .make_writable = lw_make_writable,
};
