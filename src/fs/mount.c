#include "fs/mount.h"
#include "block/block.h"
#include "fs/diskfs.h"
#include "fs/filesystems.h"
#include "fs/userfs.h"
#include "proc/process.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "sched/sync.h"
#include <fstab.h>
#include <paths.h>

#define DRIVES _PATH_MEDIA
#define CONFIG _PATH_FSTAB

/* Which drivers get a look, in order (the ones with a distinctive signature first). */
/* (ext4_ro_driver after ext4_driver: it takes the ext4 filesystems lwext4 won't.) */
static const struct fs_driver *const drivers[] = { &ntfs_driver, &exfat_driver, &ext4_driver, &ext4_ro_driver,
                                                   &fat_driver };

static struct mutex lock = MUTEX_INIT;
static struct mount *mounts;

/* The installed system's root filesystem (see mount_set_root). */
static char root_uuid[40];
static struct mount *root_mount;

static bool same_uuid(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a;
        char y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y) {
            return false;
        }
    }
    return *a == *b;
}

void mount_set_root(const char *uuid) {
    strlcpy(root_uuid, uuid, sizeof(root_uuid));
}

bool mount_wait_root(uint64_t ms) {
    for (uint64_t waited = 0; !root_mount && waited < ms; waited += 50) {
        sched_sleep_ms(50);
    }
    return root_mount != NULL;
}

struct mount *mount_root_fs(void) {
    return root_mount;
}

/* The filesystem the system was installed on: it becomes /. */
static int mount_as_root(struct mount *m) {
    if (m->read_only && m->driver->make_writable && !m->dev->read_only && !m->dev->disk->read_only) {
        int err = m->driver->make_writable(m);
        if (err) {
            kprintf("[WARN] Root filesystem: %s stays read-only: %s\n", m->dev->name, strerror(err));
        }
    }
    m->is_root = true;
    strlcpy(m->path, "/", sizeof(m->path));
    vfs_set_root(&m->root->v);
    mutex_lock(&lock);
    m->next = mounts;
    mounts = m;
    mutex_unlock(&lock);
    root_mount = m;
    uint64_t total = 0, free = 0;
    m->driver->statfs(m, &total, &free);
    kprintf("[ OK ] Root filesystem: %s (%s%s, %lu MB, %lu MB free)\n", m->dev->name, m->fs_name,
            m->read_only ? ", read-only" : "", total / 1000000, free / 1000000);
    return 0;
}

/* Between operations (the tree is held), write out what file writes left in the cache. */
static void flusher(void *arg) {
    (void)arg;
    for (;;) {
        sched_sleep_ms(500);
        for (int i = 0;; i++) {
            struct mount *m = mount_at(i);
            if (!m) {
                break;
            }
            if (m->server_pid) {
                userfs_check(m);   /* its program may have gone */
            }
            if (m->dirty) {
                mutex_lock(&m->sb.lock);
                if (m->dirty && !m->dead) {
                    m->dirty = false;
                    if (m->driver->sync) {
                        m->driver->sync(m);
                    }
                    bio_sync(m->dev);
                }
                mutex_unlock(&m->sb.lock);
            }
        }
    }
}

void mount_init(void) {
    thread_create("flusher", flusher, NULL);
}

bool mount_busy(struct block_device *d) {
    bool busy = false;
    mutex_lock(&lock);
    for (struct mount *m = mounts; m && !busy; m = m->next) {
        busy = m->dev == d || (d->disk == d && m->dev->disk == d);
    }
    mutex_unlock(&lock);
    return busy;
}

bool mount_overlaps(struct block_device *disk, uint64_t offset, uint64_t len) {
    bool hit = false;
    mutex_lock(&lock);
    for (struct mount *m = mounts; m && !hit; m = m->next) {
        if (m->dev->disk != disk) {
            continue;
        }
        uint64_t from = m->dev->start * m->dev->sector_size, to = from + m->dev->sectors * m->dev->sector_size;
        hit = offset < to && offset + len > from;
    }
    mutex_unlock(&lock);
    return hit;
}

bool mount_write_allowed(struct block_device *d, uint64_t offset, uint64_t len) {
    struct process *me = process_current();
    bool allowed = true;
    mutex_lock(&lock);
    for (struct mount *m = mounts; m && allowed; m = m->next) {
        if (m->server_pid && me && m->server_pid == me->pid) {
            continue;   /* its own filesystem */
        }
        if (d->disk == d) {
            uint64_t from = m->dev->start * m->dev->sector_size, to = from + m->dev->sectors * m->dev->sector_size;
            allowed = m->dev->disk != d || !(offset < to && offset + len > from);
        } else {
            allowed = m->dev != d;
        }
    }
    mutex_unlock(&lock);
    return allowed;
}

struct mount *mount_of(struct block_device *d) {
    mutex_lock(&lock);
    struct mount *m = mounts;
    while (m && m->dev != d) {
        m = m->next;
    }
    mutex_unlock(&lock);
    return m;
}

/*
 * /etc/fstab, written by the Disks app (see <fstab.h>): where the filesystem with this UUID goes,
 * "noauto" (don't mount it when it's plugged in), "rw" (NTFS or ext4 read-write, served by a
 * program, see fs/userfs.c).
 */
static bool config_for(const char *uuid, char *path, size_t size, bool *noauto, bool *rw) {
    if (!uuid[0]) {
        return false;
    }
    struct vnode *f;
    if (vfs_resolve(NULL, CONFIG, &f) != 0) {
        return false;
    }
    static char buf[8192];
    static struct fstab_entry e;
    static struct mutex buf_lock = MUTEX_INIT;
    mutex_lock(&buf_lock);
    int64_t n = vfs_read(f, 0, buf, sizeof(buf) - 1);
    vfs_put(f);
    bool found = false;
    buf[n > 0 ? n : 0] = '\0';
    for (char *line = buf; line && *line && !found;) {
        char *next = line;
        while (*next && *next != '\n') {
            next++;
        }
        if (*next) {
            *next++ = '\0';
        } else {
            next = NULL;
        }
        if (fstab_parse(line, &e) && strcmp(e.uuid, uuid) == 0) {
            strlcpy(path, e.path, size);
            *noauto = e.noauto;
            *rw = e.rw;
            found = true;
        }
        line = next;
    }
    mutex_unlock(&buf_lock);
    return found;
}

/* Make the folder at `path` (and the ones above it) if it isn't there. True if it was made. */
static bool make_dirs(const char *path) {
    struct vnode *n;
    if (vfs_resolve(NULL, path, &n) == 0) {
        vfs_put(n);
        return false;
    }
    char part[VFS_PATH_MAX];
    strlcpy(part, path, sizeof(part));
    for (char *c = part + 1; *c; c++) {
        if (*c == '/') {
            *c = '\0';
            vfs_create(NULL, part, VNODE_DIR, 0755, NULL);
            *c = '/';
        }
    }
    return vfs_create(NULL, part, VNODE_DIR, 0755, NULL) == 0;
}

struct mount *mount_at(int n) {
    mutex_lock(&lock);
    struct mount *m = mounts;
    while (m && n-- > 0) {
        m = m->next;
    }
    mutex_unlock(&lock);
    return m;
}

/* A folder name for the drive: its label, else "<model> <size>"; unique under /media. */
static void pick_name(struct mount *m, char *out, size_t size) {
    char base[VFS_NAME_MAX];
    if (m->label[0]) {
        strlcpy(base, m->label, sizeof(base));
    } else {
        uint64_t bytes = m->dev->sectors * m->dev->sector_size;
        uint64_t gb10 = bytes * 10 / 1000000000;
        if (gb10 >= 10) {
            ksnprintf(base, sizeof(base), "%lu GB Drive", (gb10 + 5) / 10);
        } else {
            ksnprintf(base, sizeof(base), "%lu MB Drive", bytes / 1000000);
        }
    }
    /* No slashes, no leading/trailing spaces, not "." or "..". */
    for (char *c = base; *c; c++) {
        if (*c == '/' || (unsigned char)*c < 0x20) {
            *c = '_';
        }
    }
    size_t len = strlen(base);
    while (len && base[len - 1] == ' ') {
        base[--len] = '\0';
    }
    char *start = base;
    while (*start == ' ') {
        start++;
    }
    if (!*start || strcmp(start, ".") == 0 || strcmp(start, "..") == 0) {
        start = "Drive";
    }
    for (int n = 1; n < 100; n++) {
        char path[VFS_PATH_MAX];
        if (n == 1) {
            ksnprintf(out, size, "%s", start);
        } else {
            ksnprintf(out, size, "%s (%d)", start, n);
        }
        ksnprintf(path, sizeof(path), DRIVES "/%s", out);
        struct vnode *existing;
        if (vfs_resolve(NULL, path, &existing) != 0) {
            return;
        }
        vfs_put(existing);
    }
}

/* Put a mounted filesystem into the tree at `path` (NULL: from the config, else /media/<label>).
   On failure the caller undoes the driver's mount. */
static int place(struct mount *m, const char *path) {
    char configured[VFS_PATH_MAX];
    bool noauto = false, rw = false;
    bool have_config = config_for(m->uuid, configured, sizeof(configured), &noauto, &rw);
    if (path && path[0]) {
        strlcpy(m->path, path, sizeof(m->path));
    } else if (have_config && configured[0] == '/') {
        strlcpy(m->path, configured, sizeof(m->path));
    } else {
        make_dirs(DRIVES);
        char name[VFS_NAME_MAX];
        pick_name(m, name, sizeof(name));
        ksnprintf(m->path, sizeof(m->path), DRIVES "/%s", name);
    }
    m->created_dir = make_dirs(m->path);
    int err = vfs_resolve(NULL, m->path, &m->mountpoint);
    if (!err) {
        err = vfs_attach(m->path, &m->root->v);
    }
    if (err) {
        kprintf("[WARN] Disk: can't mount %s at %s: %s\n", m->dev->name, m->path, strerror(err));
        if (m->mountpoint) {
            vfs_put(m->mountpoint);
        }
        if (m->created_dir) {
            vfs_unlink(NULL, m->path);
        }
        return err;
    }
    vfs_put(m->mountpoint);   /* vfs_attach holds the mount point now */

    mutex_lock(&lock);
    struct mount **link = &mounts;
    while (*link) {
        link = &(*link)->next;
    }
    *link = m;
    mutex_unlock(&lock);

    if (m->server_pid) {
        /* (Not its size: the program can't answer until it's back from mounting.) */
        kprintf("[ OK ] Disk: %s (%s, read-write by pid %lu) mounted at %s\n", m->dev->name, m->fs_name,
                m->server_pid, m->path);
        return 0;
    }
    uint64_t total = 0, free = 0;
    m->driver->statfs(m, &total, &free);
    kprintf("[ OK ] Disk: %s (%s%s) mounted at %s: %lu MB, %lu MB free\n", m->dev->name, m->fs_name,
            m->read_only ? ", read-only" : "", m->path, total / 1000000, free / 1000000);
    return 0;
}

int mount_attach(struct mount *m, const char *path) {
    if (mount_busy(m->dev)) {
        return -EBUSY;
    }
    return place(m, path);
}

/* Mount `d` at `path` (NULL: from the config, else /media/<label>). `automatic`: it was just plugged in.
   `allow_rw`: if the config says read-write, hand NTFS/ext4 to their program. */
static int do_mount(struct block_device *d, const char *path, bool automatic, bool allow_rw) {
    if (mount_busy(d)) {
        return -EBUSY;
    }
    struct mount *m = kzalloc(sizeof(*m));
    if (!m) {
        return -ENOMEM;
    }
    m->sb.lock = (struct mutex)MUTEX_INIT;
    m->dev = d;
    int err = -EINVAL;
    for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]) && err; i++) {
        m->driver = drivers[i];
        err = drivers[i]->mount(m);
        if (err && err != -EINVAL) {
            kprintf("[WARN] Disk: %s looks like %s, but mounting it failed: %s\n", d->name, drivers[i]->name,
                    strerror(err));
        }
    }
    if (err) {
        if (err == -EINVAL) {
            kprintf("       Disk: %s: no filesystem I know\n", d->name);
        }
        kfree(m);
        return err;
    }
    if (d->read_only || d->disk->read_only) {
        m->read_only = true;
    }
    if (root_uuid[0] && !root_mount && same_uuid(m->uuid, root_uuid)) {
        return mount_as_root(m);
    }

    char configured[VFS_PATH_MAX];
    bool noauto = false, rw = false;
    bool have_config = config_for(m->uuid, configured, sizeof(configured), &noauto, &rw);
    if (automatic && have_config && noauto) {
        kprintf("       Disk: %s (%s) isn't mounted automatically (Disks settings)\n", d->name, m->fs_name);
        m->dead = true;
        m->driver->unmount(m);
        kfree(m);
        return 0;
    }
    if (allow_rw && have_config && rw && m->read_only && m->driver->make_writable && !d->read_only &&
        !d->disk->read_only) {
        int werr = m->driver->make_writable(m);
        if (werr) {
            kprintf("[WARN] Disk: %s: can't write to it (%s); mounting it read-only\n", d->name, strerror(werr));
        }
    }
    const char *server = m->read_only ? userfs_server_for(m->driver->name) : NULL;
    if (allow_rw && have_config && rw && server && !d->read_only && !d->disk->read_only) {
        /* Read-write: the kernel's driver lets go, and the program takes over the device. */
        m->dead = true;
        m->driver->unmount(m);
        kfree(m);
        err = userfs_start(d, server, path);
        if (!err) {
            return 0;
        }
        kprintf("[WARN] Disk: %s: %s didn't start (%s); mounting it read-only\n", d->name, server, strerror(err));
        return do_mount(d, path, automatic, false);
    }
    err = place(m, path);
    if (err) {
        m->dead = true;
        m->driver->unmount(m);
        kfree(m);
    }
    return err;
}

void mount_auto(struct block_device *d) {
    do_mount(d, NULL, true, true);
}

int mount_device(struct block_device *d, const char *path, bool writable) {
    int err = do_mount(d, path, false, true);
    if (err || !writable) {
        return err;
    }
    /* Asked for read-write (the installer): if it came up read-only and can be writable, it is. */
    struct mount *m = mount_of(d);
    if (m && m->read_only && m->driver->make_writable && !d->read_only && !d->disk->read_only) {
        err = m->driver->make_writable(m);
    }
    return err;
}

bool mount_wants_root(void) {
    return root_uuid[0] && !root_mount;
}

void mount_try_root(struct block_device *d) {
    if (!mount_wants_root() || mount_busy(d)) {
        return;
    }
    struct mount *m = kzalloc(sizeof(*m));
    if (!m) {
        return;
    }
    m->sb.lock = (struct mutex)MUTEX_INIT;
    m->dev = d;
    int err = -EINVAL;
    for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]) && err; i++) {
        m->driver = drivers[i];
        err = drivers[i]->mount(m);
    }
    if (!err && same_uuid(m->uuid, root_uuid)) {
        m->read_only = m->read_only || d->read_only || d->disk->read_only;
        mount_as_root(m);
        return;
    }
    if (!err) {
        m->dead = true;   /* (not it: left alone, as before) */
        m->driver->unmount(m);
    }
    kfree(m);
}

/* Take a mount off the tree and free it (sync first unless the disk is gone). */
static void unmount(struct mount *m) {
    mutex_lock(&lock);
    for (struct mount **link = &mounts; *link; link = &(*link)->next) {
        if (*link == m) {
            *link = m->next;
            break;
        }
    }
    mutex_unlock(&lock);

    struct vnode *mp = m->mountpoint;
    bool gone = m->dev->gone || m->dev->disk->gone;
    if (!m->is_root) {
        vfs_detach(mp);
    }
    if (!m->is_root && m->created_dir) {
        vfs_unlink(NULL, m->path);
    }

    /* Nothing runs on this filesystem while both locks are held; from now on everything fails. */
    vfs_lock_tree();
    mutex_lock(&m->sb.lock);
    m->dead = gone;
    m->driver->unmount(m);   /* writes the "cleanly unmounted" marks, unless the disk is gone */
    m->dead = true;
    if (!gone) {
        bio_sync(m->dev);
    }
    bool freed = diskfs_free_tree(m->root);
    mutex_unlock(&m->sb.lock);
    vfs_unlock_tree();
    kprintf("       Disk: %s unmounted from %s%s\n", m->dev->name, m->path, gone ? " (unplugged)" : "");
    /* The struct itself stays (the flusher may be looking at it; it's small). If a program still
       holds a file or folder from it, the tree stays too: those fail with EIO until restart. */
    (void)freed;
}

void mount_disk_gone(struct block_device *disk) {
    for (;;) {
        struct mount *found = NULL;
        mutex_lock(&lock);
        for (struct mount *m = mounts; m; m = m->next) {
            if (m->dev->disk == disk) {
                found = m;
                break;
            }
        }
        mutex_unlock(&lock);
        if (!found) {
            return;
        }
        unmount(found);
    }
}

void mount_unmount_all(void) {
    for (;;) {
        mutex_lock(&lock);
        struct mount *m = mounts;
        mutex_unlock(&lock);
        if (!m) {
            return;
        }
        unmount(m);
    }
}

static int unmount_path(const char *path, bool force) {
    struct mount *found = NULL;
    mutex_lock(&lock);
    for (struct mount *m = mounts; m; m = m->next) {
        if (strcmp(m->path, path) == 0) {
            found = m;
        }
    }
    mutex_unlock(&lock);
    if (!found) {
        return -ENOENT;
    }
    if (found->is_root) {
        return -EBUSY;   /* (the system runs from it) */
    }
    if (!force && process_files_on(&found->sb)) {
        return -EBUSY;   /* a program has a file open on it (ejecting or unplugging still goes ahead) */
    }
    unmount(found);
    return 0;
}

int mount_unmount(const char *path) {
    return unmount_path(path, false);
}

int mount_unmount_forced(const char *path) {
    return unmount_path(path, true);
}

int mount_eject(const char *path) {
    struct block_device *disk = NULL;
    mutex_lock(&lock);
    for (struct mount *m = mounts; m; m = m->next) {
        if (strcmp(m->path, path) == 0) {
            disk = m->dev->disk;
        }
    }
    if (root_mount && disk == root_mount->dev->disk) {
        disk = NULL;   /* (the system's own disk: not ejected) */
    }
    mutex_unlock(&lock);
    if (!disk) {
        return -ENOENT;
    }
    mount_disk_gone(disk);
    bio_forget(disk);
    return 0;
}
