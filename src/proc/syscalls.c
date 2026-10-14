/*
 * System call implementations. Arguments come from the saved user
 * registers in the frame (see <viralos.h> for the convention); every
 * pointer is user memory and only touched through usercopy.
 */

#include <stddef.h>

#include "proc/file.h"
#include "proc/linux.h"
#include "proc/process.h"
#include "proc/tty.h"
#include "proc/usercopy.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/idt.h"
#include "drivers/display.h"
#include "drivers/hda.h"
#include "drivers/input.h"
#include "drivers/rtc.h"
#include "acpi/power.h"
#include "drivers/timer.h"
#include "fs/vfs.h"
#include "arch/x86_64/smp.h"
#include "fs/mount.h"
#include "fs/userfs.h"
#include "fs/devfs.h"
#include "block/block.h"
#include "ipc/ipc.h"
#include "lib/errno.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include "net/net.h"

#define CHUNK 512   /* bounce buffer size for read/write */

static int64_t install(struct process *p, struct file *f);

static struct file *get_file(struct process *p, uint64_t fd) {
    return fd < PROCESS_MAX_FILES ? p->files[fd] : NULL;
}

static int64_t sys_write(struct process *p, uint64_t fd, uint64_t ubuf, uint64_t len) {
    struct file *f = get_file(p, fd);
    if (!f) {
        return -EBADF;
    }
    if (f->kind == FILE_SOCKET) {
        return net_socket_write(p, f, ubuf, len);
    }
    char buf[CHUNK];
    uint64_t done = 0;
    while (done < len) {
        size_t n = len - done < CHUNK ? len - done : CHUNK;
        if (!copy_from_user(p->space, buf, ubuf + done, n)) {
            return done ? (int64_t)done : -EFAULT;
        }
        int64_t w = file_write(f, buf, n);
        if (w < 0) {
            return done ? (int64_t)done : w;
        }
        done += (uint64_t)w;
    }
    return (int64_t)done;
}

static int64_t sys_read(struct process *p, uint64_t fd, uint64_t ubuf, uint64_t len) {
    struct file *f = get_file(p, fd);
    if (!f) {
        return -EBADF;
    }
    if (f->kind == FILE_SOCKET) {
        return net_socket_read(p, f, ubuf, len);
    }
    /* One chunk per call: for the console that's one line, for files the
       program just calls read() again. */
    char buf[CHUNK];
    int64_t n = file_read(f, buf, len < CHUNK ? len : CHUNK);
    if (n > 0 && !copy_to_user(p->space, ubuf, buf, (size_t)n)) {
        return -EFAULT;
    }
    return n;
}

static int64_t sys_open(struct process *p, uint64_t upath, uint64_t flags) {
    char path[VFS_PATH_MAX];
    int64_t len = copy_string_from_user(p->space, path, upath, sizeof(path));
    if (len < 0) {
        return len;
    }
    int fd = 0;
    while (fd < PROCESS_MAX_FILES && p->files[fd]) {
        fd++;
    }
    if (fd == PROCESS_MAX_FILES) {
        return -EMFILE;
    }
    int err;
    struct file *f = file_open(p->cwd, path, (int)flags, &err);
    if (!f) {
        return err;
    }
    p->files[fd] = f;
    return fd;
}

static int64_t sys_close(struct process *p, uint64_t fd) {
    struct file *f = get_file(p, fd);
    if (!f) {
        return -EBADF;
    }
    p->files[fd] = NULL;
    file_put(f);
    return 0;
}

static void fill_info(struct os_file_info *info, const struct vnode *n) {
    memset(info, 0, sizeof(*info));
    strlcpy(info->name, n->name, sizeof(info->name));
    info->type = n->type == VNODE_DIR ? OS_DIRECTORY : OS_FILE;
    info->mode = n->mode;
    info->size = n->size;
    info->mtime = n->mtime;
    info->ctime = n->ctime;
}

static int64_t sys_stat(struct process *p, uint64_t upath, uint64_t uinfo) {
    char path[VFS_PATH_MAX];
    int64_t len = copy_string_from_user(p->space, path, upath, sizeof(path));
    if (len < 0) {
        return len;
    }
    struct vnode *n;
    int err = vfs_resolve(p->cwd, path, &n);
    if (err) {
        return err;
    }
    struct os_file_info info;
    fill_info(&info, n);
    vfs_put(n);
    return copy_to_user(p->space, uinfo, &info, sizeof(info)) ? 0 : -EFAULT;
}

static int64_t sys_readdir(struct process *p, uint64_t fd, uint64_t index, uint64_t uinfo) {
    struct file *f = get_file(p, fd);
    if (!f) {
        return -EBADF;
    }
    if (f->kind != FILE_VNODE) {
        return -ENOTDIR;
    }
    struct vnode *child;
    int err = vfs_readdir(f->vnode, index, &child);
    if (err) {
        return err;
    }
    struct os_file_info info;
    fill_info(&info, child);
    vfs_put(child);
    return copy_to_user(p->space, uinfo, &info, sizeof(info)) ? 0 : -EFAULT;
}

static int64_t sys_getcwd(struct process *p, uint64_t ubuf, uint64_t size) {
    char path[VFS_PATH_MAX];
    vfs_path(p->cwd, path, sizeof(path));
    size_t len = strlen(path);
    if (size < len + 1) {
        return -EINVAL;
    }
    return copy_to_user(p->space, ubuf, path, len + 1) ? (int64_t)len : -EFAULT;
}

static int64_t copy_path(struct process *p, uint64_t upath, char path[VFS_PATH_MAX]) {
    int64_t len = copy_string_from_user(p->space, path, upath, VFS_PATH_MAX);
    return len < 0 ? len : 0;
}

/*
 * spawn(): start a child process. The kernel copies the path and every
 * argument string out of the caller's memory before touching anything.
 */
static int64_t sys_spawn(struct process *p, uint64_t uargs) {
    struct os_spawn args;
    if (!copy_from_user(p->space, &args, uargs, sizeof(args))) {
        return -EFAULT;
    }
    char path[VFS_PATH_MAX];
    int64_t err = copy_path(p, (uint64_t)args.path, path);
    if (err) {
        return err;
    }

    /* Arguments: up to PROCESS_MAX_ARGS strings, 4 KiB in total. */
    enum { ARG_SPACE = 4096 };
    char *strings = kmalloc(ARG_SPACE);
    if (!strings) {
        return -ENOMEM;
    }
    char *argv[PROCESS_MAX_ARGS];
    int argc = 0;
    size_t used = 0;
    for (;; argc++) {
        uint64_t uarg;
        if (!copy_from_user(p->space, &uarg, (uint64_t)args.argv + (uint64_t)argc * 8, 8)) {
            err = -EFAULT;
            break;
        }
        if (uarg == 0) {
            break;
        }
        if (argc == PROCESS_MAX_ARGS) {
            err = -EINVAL;
            break;
        }
        int64_t len = copy_string_from_user(p->space, strings + used, uarg, ARG_SPACE - used);
        if (len < 0) {
            err = len == -ENAMETOOLONG ? -EINVAL : len;
            break;
        }
        argv[argc] = strings + used;
        used += (size_t)len + 1;
    }
    if (!err && argc == 0) {
        err = -EINVAL;   /* argv[0] is required */
    }

    struct spawn_options opts = { .cwd = p->cwd, .parent = p };
    if (args.flags & OS_SPAWN_DETACHED) {
        opts.parent = NULL;
        opts.detached = true;
    }
    for (int i = 0; i < 3 && !err; i++) {
        int32_t fd = args.fds[i] == OS_SPAWN_INHERIT ? i : args.fds[i];
        opts.files[i] = get_file(p, (uint64_t)fd);
        if (!opts.files[i] && args.fds[i] != OS_SPAWN_INHERIT) {
            err = -EBADF;   /* asked for a specific fd that isn't open */
        }
    }

    int64_t result = err;
    if (!err) {
        int spawn_err;
        result = process_spawn(path, argc, argv, &opts, &spawn_err) ? (int64_t)opts.pid
                                                                    : spawn_err;
    }
    kfree(strings);
    return result;
}

static int64_t sys_wait(struct process *p, int64_t pid, uint64_t ustatus, uint64_t flags) {
    int status = 0;
    int64_t r = process_wait_child(pid, &status, (int)flags);
    if (r > 0 && ustatus && !copy_to_user(p->space, ustatus, &status, sizeof(status))) {
        return -EFAULT;
    }
    return r;
}

static int64_t sys_chdir(struct process *p, uint64_t upath) {
    char path[VFS_PATH_MAX];
    int64_t err = copy_path(p, upath, path);
    if (err) {
        return err;
    }
    struct vnode *n;
    err = vfs_resolve(p->cwd, path, &n);
    if (err) {
        return err;
    }
    if (n->type != VNODE_DIR) {
        vfs_put(n);
        return -ENOTDIR;
    }
    vfs_put(p->cwd);
    p->cwd = n;
    return 0;
}

static int64_t sys_mkdir(struct process *p, uint64_t upath) {
    char path[VFS_PATH_MAX];
    int64_t err = copy_path(p, upath, path);
    return err ? err : vfs_create(p->cwd, path, VNODE_DIR, 0755, NULL);
}

static int64_t sys_unlink(struct process *p, uint64_t upath) {
    char path[VFS_PATH_MAX];
    int64_t err = copy_path(p, upath, path);
    return err ? err : vfs_unlink(p->cwd, path);
}

static int64_t sys_rename(struct process *p, uint64_t ufrom, uint64_t uto) {
    char from[VFS_PATH_MAX], to[VFS_PATH_MAX];
    int64_t err = copy_path(p, ufrom, from);
    if (!err) {
        err = copy_path(p, uto, to);
    }
    return err ? err : vfs_rename(p->cwd, from, to);
}

static int64_t sys_procinfo(struct process *p, uint64_t index, uint64_t uinfo) {
    struct os_proc_info info;
    int err = process_info(index, &info);
    if (err) {
        return err;
    }
    return copy_to_user(p->space, uinfo, &info, sizeof(info)) ? 0 : -EFAULT;
}

/* The caller's terminal: whichever of stdin, stdout, stderr is one (console otherwise). */
static struct tty *own_tty(struct process *p) {
    for (int fd = 0; fd < 3; fd++) {
        struct tty *t = tty_of(p->files[fd]);
        if (t) {
            return t;
        }
    }
    return tty_console();
}

static int64_t sys_setfg(struct process *p, uint64_t pid) {
    if (pid == 0) {
        tty_set_foreground(own_tty(p), 0);
        return 0;
    }
    uint64_t irq = irq_save();
    struct process *target = process_find(pid);
    bool ok = target && !target->exited;
    if (ok) {
        tty_set_foreground(own_tty(p), pid);
    }
    irq_restore(irq);
    return ok ? 0 : -ENOENT;
}

static int64_t sys_pty_open(struct process *p, uint64_t ufds) {
    struct file *master, *slave;
    int err = pty_open(&master, &slave);
    if (err) {
        return err;
    }
    int64_t fm = install(p, master);
    int64_t fs = fm >= 0 ? install(p, slave) : (file_put(slave), -EMFILE);
    if (fs < 0) {
        if (fm >= 0) {
            file_put(p->files[fm]);
            p->files[fm] = NULL;
        }
        return -EMFILE;
    }
    int32_t fds[2] = { (int32_t)fm, (int32_t)fs };
    return copy_to_user(p->space, ufds, fds, sizeof(fds)) ? 0 : -EFAULT;
}

static int64_t sys_sysinfo(struct process *p, uint64_t uinfo) {
    struct heap_stats hs;
    heap_get_stats(&hs);
    struct os_sysinfo info = {
        .memory_total = pmm_total_bytes(),
        .memory_free = pmm_free_bytes(),
        .kernel_heap_used = hs.used_bytes,
        .uptime_ms = uptime_ms(),
        .processes = process_count(),
        .cpus = smp_cpu_count(),
        .idle_ms = sched_idle_ms(),
    };
    return copy_to_user(p->space, uinfo, &info, sizeof(info)) ? 0 : -EFAULT;
}

/* --- drives -------------------------------------------------------------------------------------- */

static int64_t sys_drives(struct process *p, uint64_t index, uint64_t uinfo) {
    struct mount *m = mount_at((int)index);
    if (!m || index > 1000) {
        return -ENOENT;
    }
    static struct os_drive_info info;   /* (big: not on the stack) */
    static struct mutex lock = MUTEX_INIT;
    mutex_lock(&lock);
    memset(&info, 0, sizeof(info));
    strlcpy(info.path, m->path, sizeof(info.path));
    strlcpy(info.label, m->label, sizeof(info.label));
    strlcpy(info.filesystem, m->fs_name, sizeof(info.filesystem));
    strlcpy(info.device, m->dev->name, sizeof(info.device));
    strlcpy(info.model, m->dev->model, sizeof(info.model));
    mutex_lock(&m->sb.lock);
    if (!m->dead) {
        m->driver->statfs(m, &info.total, &info.free);
    }
    mutex_unlock(&m->sb.lock);
    info.flags = (m->read_only ? OS_DRIVE_READ_ONLY : 0) | (m->dev->removable ? OS_DRIVE_REMOVABLE : 0) |
                 (m->dev->external ? OS_DRIVE_EXTERNAL : 0);
    bool ok = copy_to_user(p->space, uinfo, &info, sizeof(info));
    mutex_unlock(&lock);
    return ok ? 0 : -EFAULT;
}

static int64_t sys_eject(struct process *p, uint64_t upath) {
    char path[VFS_PATH_MAX];
    int64_t err = copy_path(p, upath, path);
    return err < 0 ? err : mount_eject(path);
}

/* The index-th device that's still there. */
static struct block_device *live_device(uint64_t index) {
    for (int i = 0;; i++) {
        struct block_device *d = block_at(i);
        if (!d) {
            return NULL;
        }
        if (!d->gone && !d->disk->gone && index-- == 0) {
            return d;
        }
    }
}

static int64_t sys_blockdev(struct process *p, uint64_t index, uint64_t uinfo) {
    struct block_device *d = index < 4096 ? live_device(index) : NULL;
    if (!d) {
        return -ENOENT;
    }
    struct os_block_info *info = kzalloc(sizeof(*info));
    if (!info) {
        return -ENOMEM;
    }
    strlcpy(info->name, d->name, sizeof(info->name));
    strlcpy(info->disk, d->disk->name, sizeof(info->disk));
    strlcpy(info->model, d->disk->model, sizeof(info->model));
    strlcpy(info->serial, d->disk->serial, sizeof(info->serial));
    info->sectors = d->sectors;
    info->sector_size = d->sector_size;
    info->flags = (d->read_only || d->disk->read_only ? OS_BLOCK_READ_ONLY : 0) |
                  (d->disk->removable ? OS_BLOCK_REMOVABLE : 0) | (d->disk == d ? OS_BLOCK_DISK : 0) |
                  (d->disk->external ? OS_BLOCK_EXTERNAL : 0);
    strlcpy(info->table, d->table, sizeof(info->table));
    info->partition = d->partition;
    info->mbr_type = d->part_type;
    info->start = d->start;
    memcpy(info->type_guid, d->type_guid, 16);
    memcpy(info->part_guid, d->part_guid, 16);
    strlcpy(info->part_name, d->part_name, sizeof(info->part_name));
    struct mount *m = mount_of(d);
    if (m) {
        strlcpy(info->mounted_at, m->path, sizeof(info->mounted_at));
    }
    bool ok = copy_to_user(p->space, uinfo, info, sizeof(*info));
    kfree(info);
    return ok ? 0 : -EFAULT;
}

static int64_t sys_mount(struct process *p, uint64_t uname, uint64_t upath) {
    char name[VFS_PATH_MAX], path[VFS_PATH_MAX] = "";
    int64_t err = copy_path(p, uname, name);
    if (!err && upath) {
        err = copy_path(p, upath, path);
    }
    if (err) {
        return err;
    }
    struct block_device *d = block_find(name);
    return d ? mount_device(d, path[0] ? path : NULL) : -ENODEV;
}

static int64_t sys_unmount(struct process *p, uint64_t upath) {
    char path[VFS_PATH_MAX];
    int64_t err = copy_path(p, upath, path);
    return err ? err : mount_unmount(path);
}

static int64_t sys_rescan(struct process *p, uint64_t uname) {
    char name[VFS_PATH_MAX];
    int64_t err = copy_path(p, uname, name);
    if (err) {
        return err;
    }
    struct block_device *d = block_find(name);
    return d ? block_rescan(d) : -ENODEV;
}

static int64_t sys_userfs(struct process *p, uint64_t uarg) {
    static struct os_userfs in;   /* (big: not on the stack; the lock below keeps callers apart) */
    static struct mutex in_lock = MUTEX_INIT;
    mutex_lock(&in_lock);
    int64_t ret = -EFAULT;
    if (copy_from_user(p->space, &in, uarg, sizeof(in))) {
        in.device[sizeof(in.device) - 1] = '\0';
        in.path[sizeof(in.path) - 1] = '\0';
        in.fs_name[sizeof(in.fs_name) - 1] = '\0';
        in.label[sizeof(in.label) - 1] = '\0';
        in.uuid[sizeof(in.uuid) - 1] = '\0';
        int32_t buffer_fd = -1;
        ret = userfs_register(p, &in, &buffer_fd);
        if (ret >= 0) {
            copy_to_user(p->space, uarg + offsetof(struct os_userfs, buffer_fd), &buffer_fd, sizeof(buffer_fd));
        }
    }
    mutex_unlock(&in_lock);
    return ret;
}

static int64_t sys_seek(struct process *p, uint64_t fd, int64_t offset, uint64_t whence) {
    struct file *f = get_file(p, fd);
    if (!f) {
        return -EBADF;
    }
    if (f->kind != FILE_VNODE) {
        return -EINVAL;
    }
    int64_t base = whence == OS_SEEK_SET ? 0 : whence == OS_SEEK_CUR ? (int64_t)f->offset
                 : whence == OS_SEEK_END ? (int64_t)f->vnode->size : -1;
    if (base < 0 || base + offset < 0) {
        return -EINVAL;
    }
    f->offset = (uint64_t)(base + offset);
    return (int64_t)f->offset;
}

static int64_t sys_fsync(struct process *p, uint64_t fd) {
    struct file *f = get_file(p, fd);
    if (!f) {
        return -EBADF;
    }
    if (f->kind != FILE_VNODE) {
        return 0;
    }
    struct block_device *d = devfs_device(f->vnode);
    if (d) {
        return bio_sync(d);
    }
    if (f->vnode->sb) {
        struct mount *m = (struct mount *)f->vnode->sb;   /* (struct vfs_sb is a mount's first member) */
        return m->dead ? -EIO : bio_sync(m->dev);
    }
    return 0;
}

/* --- talking between processes ---------------------------------------------------------- */

/* Give `f` a descriptor in `p`, or drop it if the table is full. */
static int64_t install(struct process *p, struct file *f) {
    for (int fd = 0; fd < PROCESS_MAX_FILES; fd++) {
        if (!p->files[fd]) {
            p->files[fd] = f;
            return fd;
        }
    }
    file_put(f);
    return -EMFILE;
}

static struct file *get_kind(struct process *p, uint64_t fd, enum file_kind kind) {
    struct file *f = get_file(p, fd);
    return f && f->kind == kind ? f : NULL;
}

static int64_t sys_shm_create(struct process *p, uint64_t size) {
    struct file *f;
    int err = shm_create(size, &f);
    return err ? err : install(p, f);
}

static int64_t sys_shm_map(struct process *p, uint64_t fd, uint64_t usize) {
    struct file *f = get_kind(p, fd, FILE_SHM);
    if (!f) {
        return -EBADF;
    }
    uint64_t size;
    int64_t addr = shm_map(p, f, &size);
    if (addr > 0 && usize && !copy_to_user(p->space, usize, &size, sizeof(size))) {
        shm_unmap(p, (uint64_t)addr);
        return -EFAULT;
    }
    return addr;
}

static int64_t sys_channel_pair(struct process *p, uint64_t ufds) {
    struct file *a, *b;
    int err = channel_pair(&a, &b);
    if (err) {
        return err;
    }
    int64_t fa = install(p, a);
    int64_t fb = fa >= 0 ? install(p, b) : (file_put(b), -EMFILE);
    if (fb < 0) {
        if (fa >= 0) {
            file_put(p->files[fa]);
            p->files[fa] = NULL;
        }
        return -EMFILE;
    }
    int32_t fds[2] = { (int32_t)fa, (int32_t)fb };
    if (!copy_to_user(p->space, ufds, fds, sizeof(fds))) {
        return -EFAULT;
    }
    return 0;
}

static int64_t sys_service(struct process *p, uint64_t number, uint64_t arg) {
    if (number == SYS_SERVICE_ACCEPT) {
        struct file *l = get_kind(p, arg, FILE_LISTENER), *f;
        if (!l) {
            return -EBADF;
        }
        int err = service_accept(l, &f);
        return err ? err : install(p, f);
    }
    char name[OS_SERVICE_NAME_MAX];
    int64_t len = copy_string_from_user(p->space, name, arg, sizeof(name));
    if (len < 0) {
        return len == -ENAMETOOLONG ? -EINVAL : len;
    }
    struct file *f;
    int err = number == SYS_SERVICE_REGISTER ? service_register(name, &f)
                                             : service_connect(name, &f);
    return err ? err : install(p, f);
}

static int64_t sys_msg_send(struct process *p, uint64_t fd, uint64_t ubuf, uint64_t len,
                            int64_t attach_fd, uint64_t flags) {
    struct file *f = get_kind(p, fd, FILE_CHANNEL);
    if (!f) {
        return -EBADF;
    }
    if (len == 0 || len > OS_MSG_MAX) {
        return len ? -EMSGSIZE : -EINVAL;
    }
    struct file *attach = NULL;
    if (attach_fd >= 0) {
        attach = get_file(p, (uint64_t)attach_fd);
        if (!attach) {
            return -EBADF;
        }
        file_get(attach);   /* the message's own reference */
    }
    uint8_t *buf = kmalloc(len);
    int64_t err = buf ? 0 : -ENOMEM;
    if (!err && !copy_from_user(p->space, buf, ubuf, len)) {
        err = -EFAULT;
    }
    if (!err) {
        err = channel_send(f, buf, len, attach, !(flags & OS_MSG_NOWAIT));
    }
    kfree(buf);
    if (err && attach) {
        file_put(attach);
    }
    return err;
}

static int64_t sys_msg_recv(struct process *p, uint64_t fd, uint64_t ubuf, uint64_t size,
                            uint64_t uhandle) {
    struct file *f = get_kind(p, fd, FILE_CHANNEL);
    if (!f) {
        return -EBADF;
    }
    if (size > OS_MSG_MAX) {
        size = OS_MSG_MAX;
    }
    uint8_t *buf = kmalloc(size ? size : 1);
    if (!buf) {
        return -ENOMEM;
    }
    struct file *attached = NULL;
    int64_t len = channel_recv(f, buf, size, &attached);
    int32_t handle = -1;
    if (len > 0 && attached) {
        int64_t h = install(p, attached);   /* drops it if there's no free slot */
        handle = h >= 0 ? (int32_t)h : -1;
    }
    if (len > 0 && !copy_to_user(p->space, ubuf, buf, (size_t)len)) {
        len = -EFAULT;
    }
    if (uhandle && !copy_to_user(p->space, uhandle, &handle, sizeof(handle))) {
        len = -EFAULT;
    }
    kfree(buf);
    return len;
}

static int64_t sys_user(struct process *p, uint64_t op, uint64_t ubuf) {
    if (op == OS_USER_GET) {
        return copy_to_user(p->space, ubuf, p->user, sizeof(p->user)) ? 0 : -EFAULT;
    }
    if (op == OS_USER_SET) {
        char name[OS_USER_MAX];
        int64_t len = copy_string_from_user(p->space, name, ubuf, sizeof(name));
        if (len < 0) {
            return len;
        }
        for (int64_t i = 0; i < len; i++) {
            char c = name[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
                return -EINVAL;   /* (it becomes a folder name) */
            }
        }
        strlcpy(p->user, name, sizeof(p->user));
        return 0;
    }
    return -EINVAL;
}

static int64_t sys_poll(struct process *p, uint64_t ufds, uint64_t count, int64_t timeout) {
    if (count > OS_POLL_MAX) {
        return -EINVAL;
    }
    struct os_pollfd fds[OS_POLL_MAX];
    if (count && !copy_from_user(p->space, fds, ufds, count * sizeof(fds[0]))) {
        return -EFAULT;
    }
    int ready = ipc_poll(p, fds, (int)count, timeout);
    if (count && !copy_to_user(p->space, ufds, fds, count * sizeof(fds[0]))) {
        return -EFAULT;
    }
    return ready;
}

/* --- graphics ----------------------------------------------------------------------------- */

static int64_t sys_display_claim(struct process *p, uint64_t uinfo) {
    struct os_display_info info;
    int err = display_claim(p, &info);
    if (err) {
        return err;
    }
    return copy_to_user(p->space, uinfo, &info, sizeof(info)) ? 0 : -EFAULT;
}

static int64_t sys_input_read(struct process *p, uint64_t uevents, uint64_t max, int64_t timeout) {
    if (input_owner() != p->pid) {
        return -EBUSY;   /* only the display owner gets input events */
    }
    struct os_input_event events[16];
    int n = input_read(events, max < 16 ? (int)max : 16, timeout);
    if (n > 0 && !copy_to_user(p->space, uevents, events, (size_t)n * sizeof(events[0]))) {
        return -EFAULT;
    }
    return n;
}

/* Called from syscall_entry (user.S) with the saved user registers. */
void syscall_dispatch(struct interrupt_frame *f) {
    /* Kernel code: under the big kernel lock (smp.h). Then syscalls can block or take a while: let
       interrupts in. */
    kernel_lock();
    interrupts_enable();

    struct process *p = process_current();
    if (p->linux_abi && f->rax < OS_LINUX_NATIVE_BASE) {
        f->rax = (uint64_t)linux_syscall(p, f);
        process_exit_if_killed();
        interrupts_disable();
        kernel_unlock();
        return;
    }
    if (p->linux_abi) {
        f->rax -= OS_LINUX_NATIVE_BASE;   /* a ported program asking for one of ours (the userfs servers) */
    }
    uint64_t a1 = f->rdi, a2 = f->rsi, a3 = f->rdx, a4 = f->r10, a5 = f->r8;
    int64_t ret;

    switch (f->rax) {
    case SYS_EXIT:    process_exit((int)(a1 & 0xFF));
    case SYS_WRITE:   ret = sys_write(p, a1, a2, a3); break;
    case SYS_READ:    ret = sys_read(p, a1, a2, a3); break;
    case SYS_OPEN:    ret = sys_open(p, a1, a2); break;
    case SYS_CLOSE:   ret = sys_close(p, a1); break;
    case SYS_STAT:    ret = sys_stat(p, a1, a2); break;
    case SYS_READDIR: ret = sys_readdir(p, a1, a2, a3); break;
    case SYS_SLEEP:   sched_sleep_ms(a1); ret = 0; break;
    case SYS_GETPID:  ret = (int64_t)p->pid; break;
    case SYS_UPTIME:  ret = (int64_t)uptime_ms(); break;
    case SYS_GETCWD:  ret = sys_getcwd(p, a1, a2); break;
    case SYS_SPAWN:   ret = sys_spawn(p, a1); break;
    case SYS_WAIT:    ret = sys_wait(p, (int64_t)a1, a2, a3); break;
    case SYS_CHDIR:   ret = sys_chdir(p, a1); break;
    case SYS_MKDIR:   ret = sys_mkdir(p, a1); break;
    case SYS_UNLINK:  ret = sys_unlink(p, a1); break;
    case SYS_KILL:    ret = process_kill(a1) ? 0 : -ENOENT; break;
    case SYS_BRK:     ret = (int64_t)(a1 ? process_brk(p, a1) : p->heap_end); break;
    case SYS_PROCINFO: ret = sys_procinfo(p, a1, a2); break;
    case SYS_SETFG:   ret = sys_setfg(p, a1); break;
    case SYS_SYSINFO: ret = sys_sysinfo(p, a1); break;
    case SYS_TTY_MODE: ret = tty_set_mode(own_tty(p), (int)a1, p->pid); break;
    case SYS_DISPLAY_CLAIM:   ret = sys_display_claim(p, a1); break;
    case SYS_DISPLAY_RELEASE: display_release(p->pid); ret = 0; break;
    case SYS_INPUT_READ:      ret = sys_input_read(p, a1, a2, (int64_t)a3); break;
    case SYS_SHM_CREATE:      ret = sys_shm_create(p, a1); break;
    case SYS_SHM_MAP:         ret = sys_shm_map(p, a1, a2); break;
    case SYS_SHM_UNMAP:       ret = shm_unmap(p, a1); break;
    case SYS_CHANNEL_PAIR:    ret = sys_channel_pair(p, a1); break;
    case SYS_SERVICE_REGISTER:
    case SYS_SERVICE_CONNECT:
    case SYS_SERVICE_ACCEPT:  ret = sys_service(p, f->rax, a1); break;
    case SYS_MSG_SEND:        ret = sys_msg_send(p, a1, a2, a3, (int64_t)a4, a5); break;
    case SYS_MSG_RECV:        ret = sys_msg_recv(p, a1, a2, a3, a4); break;
    case SYS_POLL:            ret = sys_poll(p, a1, a2, (int64_t)a3); break;
    case SYS_TIME:            ret = (int64_t)rtc_time(); break;
    case SYS_PTY_OPEN:        ret = sys_pty_open(p, a1); break;
    case SYS_RENAME:          ret = sys_rename(p, a1, a2); break;
    case SYS_POWER:           ret = power_off_or_restart((int)a1); break;
    case SYS_AUDIO_OPEN:      ret = audio_open(p->pid, (uint32_t)a1, (uint32_t)a2); break;
    case SYS_AUDIO_WRITE:     ret = audio_write(p, a1, a2, a3); break;
    case SYS_AUDIO_CTL:       ret = audio_ctl(p->pid, a1, a2); break;
    case SYS_AUDIO_MIXER:     ret = audio_mixer(p, a1, a2, a3); break;
    case SYS_DRIVES:          ret = sys_drives(p, a1, a2); break;
    case SYS_EJECT:           ret = sys_eject(p, a1); break;
    case SYS_SEEK:            ret = sys_seek(p, a1, (int64_t)a2, a3); break;
    case SYS_FSYNC:           ret = sys_fsync(p, a1); break;
    case SYS_BLOCKDEV:        ret = sys_blockdev(p, a1, a2); break;
    case SYS_MOUNT:           ret = sys_mount(p, a1, a2); break;
    case SYS_UNMOUNT:         ret = sys_unmount(p, a1); break;
    case SYS_RESCAN:          ret = sys_rescan(p, a1); break;
    case SYS_USERFS:          ret = sys_userfs(p, a1); break;
    case SYS_SOCKET:          ret = sys_socket(p, a1, a2, a3); break;
    case SYS_BIND:            ret = sys_bind(p, a1, a2, a3); break;
    case SYS_CONNECT:         ret = sys_connect(p, a1, a2, a3); break;
    case SYS_LISTEN:          ret = sys_listen(p, a1, a2); break;
    case SYS_ACCEPT:          ret = sys_accept(p, a1, a2, a3); break;
    case SYS_SENDTO:          ret = sys_sendto(p, a1, a2); break;
    case SYS_RECVFROM:        ret = sys_recvfrom(p, a1, a2); break;
    case SYS_SETSOCKOPT:      ret = sys_setsockopt(p, a1, a2, a3, a4, a5); break;
    case SYS_GETSOCKOPT:      ret = sys_getsockopt(p, a1, a2, a3, a4, a5); break;
    case SYS_SOCKNAME:        ret = sys_sockname(p, a1, a2, a3, a4); break;
    case SYS_SHUTDOWN:        ret = sys_shutdown(p, a1, a2); break;
    case SYS_RESOLVE:         ret = sys_resolve(p, a1, a2); break;
    case SYS_NETIF:           ret = sys_netif(p, a1, a2); break;
    case SYS_NETIF_SET:       ret = sys_netif_set(p, a1); break;
    case SYS_INTERRUPT:       ret = process_interrupt_mode(p, (int)a1); break;
    case SYS_UPTIME_US:       ret = (int64_t)uptime_us(); break;
    case SYS_USER:            ret = sys_user(p, a1, a2); break;
    default:          ret = -ENOSYS; break;
    }
    f->rax = (uint64_t)ret;

    process_exit_if_killed();
    interrupts_disable();   /* iretq restores the user's flags */
    kernel_unlock();        /* back to the program (perhaps on another CPU than it came in on) */
}
