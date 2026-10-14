/*
 * Linux system calls for Linux programs (see linux.h). Numbers, structures
 * and flag values are the x86_64 Linux ones (arch/x86/entry/syscalls,
 * include/uapi). Error numbers are already the same as ours.
 */

#include <stddef.h>

#include "lib/random.h"
#include "proc/linux.h"
#include "proc/elf.h"
#include "proc/file.h"
#include "proc/process.h"
#include "proc/tty.h"
#include "proc/usercopy.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/idt.h"
#include "block/block.h"
#include "drivers/rtc.h"
#include "drivers/timer.h"
#include "fs/devfs.h"
#include "fs/mount.h"
#include "fs/vfs.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include <paths.h>

#define LINUX_STACK_SIZE (2 * 1024 * 1024)
#define MMAP_BASE        0x0000500000000000ULL
#define MMAP_END         USER_SHM_BASE
#define IO_CHUNK         (64 * 1024)

/* Linux errors we have no name for. */
#define ESRCH       3
#define ENOTTY      25
#define ESPIPE      29
#define ERANGE      34
#define EOPNOTSUPP  95

#define AT_FDCWD     (-100)
#define AT_EMPTY_PATH 0x1000
#define AT_REMOVEDIR  0x200

#define O_EXCL_L      0x80
#define O_DIRECTORY_L 0x10000

#define S_IFCHR 0020000
#define S_IFDIR 0040000
#define S_IFBLK 0060000
#define S_IFREG 0100000

struct linux_map {
    uint64_t start, len;
    struct linux_map *next;   /* sorted by start */
};

struct linux_stat {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad0;
    uint64_t rdev;
    int64_t size, blksize, blocks;
    uint64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    int64_t unused[3];
};

struct linux_iovec {
    uint64_t base, len;
};

/* --- the start-up stack ------------------------------------------------------------------------------------ */

/* Push bytes onto the new stack (growing down); returns their address. */
static int push(struct process *p, uint64_t *sp, const void *data, size_t len) {
    *sp -= len;
    return copy_to_user(p->space, *sp, data, len) ? 0 : -EFAULT;
}

int linux_setup_stack(struct process *p, int argc, char **argv, const struct elf_info *info) {
    int err = process_map_zeroed(p->space, USER_STACK_TOP - LINUX_STACK_SIZE, USER_STACK_TOP, VMM_WRITE);
    if (err) {
        return err;
    }
    char home[16 + OS_USER_MAX];   /* the user it runs for ("user" before anyone logged in) */
    ksnprintf(home, sizeof(home), "HOME=" _PATH_HOME "/%s", p->user[0] ? p->user : "user");
    const char *const env[] = { "PATH=" _PATH_BIN, home, "TERM=dumb", "LANG=C.UTF-8" };
    enum { ENVC = sizeof(env) / sizeof(env[0]) };
    /* Like Linux, the strings don't end right at the top: some programs read a little past the end of
       an argument (limine's memcmp(arg, "--uninstall-data-file=", 22) on a shorter one), which is
       harmless on Linux, where the program's path and a zero word sit above them. */
    uint64_t sp = USER_STACK_TOP - 256;
    uint64_t argp[PROCESS_MAX_ARGS], envp[ENVC];
    for (int i = argc - 1; i >= 0 && !err; i--) {
        err = push(p, &sp, argv[i], strlen(argv[i]) + 1);
        argp[i] = sp;
    }
    for (int i = ENVC - 1; i >= 0 && !err; i--) {
        err = push(p, &sp, env[i], strlen(env[i]) + 1);
        envp[i] = sp;
    }
    uint64_t at_random[2] = { random_u64(), random_u64() };
    if (!err) {
        err = push(p, &sp, "x86_64", 7);
    }
    uint64_t platform = sp;
    sp &= ~15ull;
    if (!err) {
        err = push(p, &sp, at_random, 16);
    }
    uint64_t random_at = sp;
    if (err) {
        return err;
    }

    uint64_t aux[][2] = {
        { 3, info->phdr },        /* AT_PHDR: musl finds its TLS segment through these */
        { 4, 56 },                /* AT_PHENT */
        { 5, info->phnum },       /* AT_PHNUM */
        { 6, PAGE_SIZE },         /* AT_PAGESZ */
        { 7, 0 },                 /* AT_BASE: no interpreter */
        { 8, 0 },                 /* AT_FLAGS */
        { 9, info->entry },       /* AT_ENTRY */
        { 11, 0 }, { 12, 0 }, { 13, 0 }, { 14, 0 },   /* uid, euid, gid, egid: root */
        { 15, platform },         /* AT_PLATFORM */
        { 16, 0 },                /* AT_HWCAP */
        { 17, 100 },              /* AT_CLKTCK */
        { 23, 0 },                /* AT_SECURE */
        { 25, random_at },        /* AT_RANDOM */
        { 31, argp[0] },          /* AT_EXECFN */
        { 0, 0 },                 /* AT_NULL */
    };
    size_t naux = sizeof(aux) / sizeof(aux[0]);
    size_t words = 1 + (size_t)argc + 1 + ENVC + 1 + 2 * naux;
    sp = (sp - words * 8) & ~15ull;   /* rsp % 16 == 0 at _start, pointing at argc */
    uint64_t table[1 + PROCESS_MAX_ARGS + 1 + ENVC + 1 + 2 * 18];
    size_t k = 0;
    table[k++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) {
        table[k++] = argp[i];
    }
    table[k++] = 0;
    for (int i = 0; i < ENVC; i++) {
        table[k++] = envp[i];
    }
    table[k++] = 0;
    for (size_t i = 0; i < naux; i++) {
        table[k++] = aux[i][0];
        table[k++] = aux[i][1];
    }
    if (!copy_to_user(p->space, sp, table, k * 8)) {
        return -EFAULT;
    }
    p->user_rsp = sp;
    p->mmap_next = MMAP_BASE;
    return 0;
}

/* --- memory ------------------------------------------------------------------------------------------------ */

void linux_free_maps(struct process *p) {
    while (p->maps) {
        struct linux_map *m = p->maps;
        p->maps = m->next;
        kfree(m);
    }
}

static void unmap_pages(struct process *p, uint64_t start, uint64_t end) {
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t phys;
        if (vmm_unmap_user(p->space, va, &phys) && phys) {
            pmm_free_page(phys);
        }
    }
}

/* Take [start, start+len) out of the mapping list (splitting what straddles it) and free its pages. */
static void unmap_range(struct process *p, uint64_t start, uint64_t len) {
    uint64_t end = start + len;
    for (struct linux_map **link = &p->maps; *link;) {
        struct linux_map *m = *link;
        uint64_t ms = m->start, me = m->start + m->len;
        if (me <= start || ms >= end) {
            link = &m->next;
            continue;
        }
        uint64_t cut_from = ms > start ? ms : start, cut_to = me < end ? me : end;
        unmap_pages(p, cut_from, cut_to);
        if (ms < start && me > end) {
            /* A hole in the middle: split in two. */
            struct linux_map *tail = kmalloc(sizeof(*tail));
            if (tail) {
                tail->start = end;
                tail->len = me - end;
                tail->next = m->next;
                m->next = tail;
            }
            m->len = start - ms;
            link = &m->next;
        } else if (ms < start) {
            m->len = start - ms;
            link = &m->next;
        } else if (me > end) {
            m->start = end;
            m->len = me - end;
            link = &m->next;
        } else {
            *link = m->next;
            kfree(m);
        }
    }
}

static void insert_map(struct process *p, uint64_t start, uint64_t len) {
    struct linux_map *n = kmalloc(sizeof(*n));
    if (!n) {
        return;
    }
    n->start = start;
    n->len = len;
    struct linux_map **link = &p->maps;
    while (*link && (*link)->start < start) {
        link = &(*link)->next;
    }
    n->next = *link;
    *link = n;
}

/* First gap of `len` bytes in the mmap area. */
static uint64_t find_gap(struct process *p, uint64_t len) {
    uint64_t at = MMAP_BASE;
    for (struct linux_map *m = p->maps; m; m = m->next) {
        if (m->start >= at + len) {
            break;
        }
        if (m->start + m->len > at) {
            at = m->start + m->len;
        }
    }
    return at + len <= MMAP_END ? at : 0;
}

static int64_t l_mmap(struct process *p, uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, int64_t fd) {
    (void)fd;
    if (!(flags & 0x20)) {   /* MAP_ANONYMOUS: files can't be mapped */
        return -ENODEV;
    }
    if (!len) {
        return -EINVAL;
    }
    len = ALIGN_UP(len, PAGE_SIZE);
    uint64_t start;
    if (flags & 0x10) {   /* MAP_FIXED */
        if (addr & (PAGE_SIZE - 1) || addr < MMAP_BASE || addr + len > MMAP_END) {
            return -EINVAL;
        }
        unmap_range(p, addr, len);
        start = addr;
    } else {
        start = find_gap(p, len);
        if (!start) {
            return -ENOMEM;
        }
    }
    uint32_t vflags = VMM_WRITE | (prot & 4 ? VMM_EXEC : 0);
    if (process_map_zeroed(p->space, start, start + len, vflags) != 0) {
        unmap_pages(p, start, start + len);
        return -ENOMEM;
    }
    insert_map(p, start, len);
    return (int64_t)start;
}

static int64_t l_munmap(struct process *p, uint64_t addr, uint64_t len) {
    if (addr & (PAGE_SIZE - 1) || !len) {
        return -EINVAL;
    }
    unmap_range(p, addr, ALIGN_UP(len, PAGE_SIZE));
    return 0;
}

/* --- files ------------------------------------------------------------------------------------------------- */

static struct file *get_file(struct process *p, int64_t fd) {
    return fd >= 0 && fd < PROCESS_MAX_FILES ? p->files[fd] : NULL;
}

static int64_t install(struct process *p, struct file *f, int min) {
    for (int fd = min; fd < PROCESS_MAX_FILES; fd++) {
        if (!p->files[fd]) {
            p->files[fd] = f;
            return fd;
        }
    }
    file_put(f);
    return -EMFILE;
}

static bool is_tty(struct file *f) {
    return f && (f->kind == FILE_CONSOLE || f->kind == FILE_PTY);
}

/* The directory a *at() call is relative to. */
static int at_dir(struct process *p, int64_t dirfd, const char *path, struct vnode **out) {
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        *out = p->cwd;
        return 0;
    }
    struct file *f = get_file(p, dirfd);
    if (!f) {
        return -EBADF;
    }
    if (f->kind != FILE_VNODE || f->vnode->type != VNODE_DIR) {
        return -ENOTDIR;
    }
    *out = f->vnode;
    return 0;
}

static int64_t user_path(struct process *p, uint64_t upath, char path[VFS_PATH_MAX]) {
    int64_t len = copy_string_from_user(p->space, path, upath, VFS_PATH_MAX);
    return len < 0 ? len : 0;
}

/* mtab escapes spaces, tabs, newlines and backslashes as octal. */
static size_t mtab_escape(char *out, size_t size, const char *s) {
    size_t o = 0;
    for (; *s && o + 5 < size; s++) {
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\\') {
            ksnprintf(out + o, size - o, "\\%03o", (unsigned char)*s);
            o += 4;
        } else {
            out[o++] = *s;
        }
    }
    out[o] = '\0';
    return o;
}

/* /proc/mounts and /etc/mtab: what's mounted, in Linux's format, so the tools see it (and refuse to
   format a mounted drive). Made as a file in /tmp that's deleted at once: it lives while open. */
static int64_t open_mtab(struct process *p) {
    char *text = kmalloc(16384);
    if (!text) {
        return -ENOMEM;
    }
    size_t len = 0;
    for (int i = 0; len < 15000; i++) {
        struct mount *m = mount_at(i);
        if (!m) {
            break;
        }
        const char *type = strncmp(m->fs_name, "FAT", 3) == 0 ? "vfat" : strcmp(m->fs_name, "exFAT") == 0 ? "exfat"
                         : strcmp(m->fs_name, "NTFS") == 0 ? "ntfs3" : m->fs_name;
        char where[VFS_PATH_MAX];
        mtab_escape(where, sizeof(where), m->path);
        len += (size_t)ksnprintf(text + len, 16384 - len, _PATH_DEV "/%s %s %s %s 0 0\n", m->dev->name, where, type,
                                 m->read_only ? "ro" : "rw");
    }
    char path[64];
    static uint32_t counter;
    ksnprintf(path, sizeof(path), _PATH_TMP "/.mtab-%lu-%u", p->pid, counter++);
    int err;
    struct file *f = file_open(NULL, path, O_RDWR | O_CREAT | O_TRUNC, &err);
    if (f) {
        vfs_write(f->vnode, 0, text, len);
        vfs_unlink(NULL, path);
        f->flags = O_RDONLY;
    }
    kfree(text);
    return f ? install(p, f, 0) : err;
}

static int64_t l_openat(struct process *p, int64_t dirfd, uint64_t upath, uint64_t flags) {
    char path[VFS_PATH_MAX];
    struct vnode *dir;
    int64_t err = user_path(p, upath, path);
    if (!err) {
        err = at_dir(p, dirfd, path, &dir);
    }
    if (err) {
        return err;
    }
    if (strcmp(path, "/proc/mounts") == 0 || strcmp(path, "/proc/self/mounts") == 0 ||
        strcmp(path, "/etc/mtab") == 0) {
        return open_mtab(p);
    }
    if ((flags & O_CREAT) && (flags & O_EXCL_L)) {
        struct vnode *existing;
        if (vfs_resolve(dir, path, &existing) == 0) {
            vfs_put(existing);
            return -EEXIST;
        }
    }
    int ferr;
    struct file *f = file_open(dir, path, (int)(flags & (O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND)), &ferr);
    if (!f) {
        return ferr;
    }
    if ((flags & O_DIRECTORY_L) && f->vnode->type != VNODE_DIR) {
        file_put(f);
        return -ENOTDIR;
    }
    return install(p, f, 0);
}

static int64_t l_read(struct process *p, int64_t fd, uint64_t ubuf, uint64_t len, int64_t at) {
    struct file *f = get_file(p, fd);
    if (!f) {
        return -EBADF;
    }
    if (f->kind != FILE_VNODE) {
        if (at >= 0) {
            return -ESPIPE;
        }
        /* Terminals and the like: one piece, as their read gives it. */
        char buf[512];
        int64_t n = file_read(f, buf, len < sizeof(buf) ? len : sizeof(buf));
        if (n > 0 && !copy_to_user(p->space, ubuf, buf, (size_t)n)) {
            return -EFAULT;
        }
        return n;
    }
    if ((f->flags & O_ACCMODE) == O_WRONLY) {
        return -EBADF;
    }
    if (f->vnode->type == VNODE_DIR) {
        return -EISDIR;
    }
    uint8_t *buf = kmalloc(IO_CHUNK);
    if (!buf) {
        return -ENOMEM;
    }
    uint64_t pos = at >= 0 ? (uint64_t)at : f->offset, done = 0;
    int64_t err = 0;
    while (done < len) {
        size_t n = len - done < IO_CHUNK ? (size_t)(len - done) : IO_CHUNK;
        int64_t r = vfs_read(f->vnode, pos, buf, n);
        if (r < 0) {
            err = r;
            break;
        }
        if (r && !copy_to_user(p->space, ubuf + done, buf, (size_t)r)) {
            err = -EFAULT;
            break;
        }
        pos += (uint64_t)r;
        done += (uint64_t)r;
        if ((size_t)r < n) {
            break;   /* end of file */
        }
    }
    kfree(buf);
    if (at < 0) {
        f->offset = pos;
    }
    return done ? (int64_t)done : err;
}

static int64_t l_write(struct process *p, int64_t fd, uint64_t ubuf, uint64_t len, int64_t at) {
    struct file *f = get_file(p, fd);
    if (!f) {
        return -EBADF;
    }
    if ((f->flags & O_ACCMODE) == O_RDONLY) {
        return -EBADF;
    }
    if (f->kind != FILE_VNODE) {
        if (at >= 0) {
            return -ESPIPE;
        }
        char buf[512];
        uint64_t done = 0;
        while (done < len) {
            size_t n = len - done < sizeof(buf) ? (size_t)(len - done) : sizeof(buf);
            if (!copy_from_user(p->space, buf, ubuf + done, n)) {
                return done ? (int64_t)done : -EFAULT;
            }
            int64_t w = file_write(f, buf, n);
            if (w <= 0) {
                return done ? (int64_t)done : w;
            }
            done += (uint64_t)w;
        }
        return (int64_t)done;
    }
    uint8_t *buf = kmalloc(IO_CHUNK);
    if (!buf) {
        return -ENOMEM;
    }
    uint64_t pos = at >= 0 ? (uint64_t)at : (f->flags & O_APPEND) ? f->vnode->size : f->offset, done = 0;
    int64_t err = 0;
    while (done < len) {
        size_t n = len - done < IO_CHUNK ? (size_t)(len - done) : IO_CHUNK;
        if (!copy_from_user(p->space, buf, ubuf + done, n)) {
            err = -EFAULT;
            break;
        }
        int64_t w = vfs_write(f->vnode, pos, buf, n);
        if (w <= 0) {
            err = w ? w : -ENOSPC;
            break;
        }
        pos += (uint64_t)w;
        done += (uint64_t)w;
    }
    kfree(buf);
    if (at < 0) {
        f->offset = pos;
    }
    return done ? (int64_t)done : err;
}

static int64_t l_iov(struct process *p, int64_t fd, uint64_t uiov, int64_t count, bool write, int64_t at) {
    if (count < 0 || count > 1024) {
        return -EINVAL;
    }
    int64_t total = 0;
    for (int64_t i = 0; i < count; i++) {
        struct linux_iovec v;
        if (!copy_from_user(p->space, &v, uiov + (uint64_t)i * sizeof(v), sizeof(v))) {
            return total ? total : -EFAULT;
        }
        if (!v.len) {
            continue;
        }
        int64_t r = write ? l_write(p, fd, v.base, v.len, at < 0 ? -1 : at + total)
                          : l_read(p, fd, v.base, v.len, at < 0 ? -1 : at + total);
        if (r < 0) {
            return total ? total : r;
        }
        total += r;
        if ((uint64_t)r < v.len) {
            break;
        }
    }
    return total;
}

static int64_t l_lseek(struct process *p, int64_t fd, int64_t offset, int64_t whence) {
    struct file *f = get_file(p, fd);
    if (!f) {
        return -EBADF;
    }
    if (f->kind != FILE_VNODE) {
        return -ESPIPE;
    }
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->offset : whence == 2 ? (int64_t)f->vnode->size : -1;
    if (base < 0 || base + offset < 0) {
        return -EINVAL;
    }
    f->offset = (uint64_t)(base + offset);
    return (int64_t)f->offset;
}

static void fill_stat(struct linux_stat *st, struct file *f, struct vnode *n) {
    memset(st, 0, sizeof(*st));
    st->blksize = 4096;
    if (!n) {
        st->mode = S_IFCHR | 0620;   /* a terminal (or IPC end: close enough) */
        st->rdev = 0x8800;
        st->nlink = 1;
        st->ino = (uint64_t)(uintptr_t)f >> 4;
        return;
    }
    struct block_device *d = devfs_device(n);
    st->ino = (uint64_t)(uintptr_t)n >> 4;
    st->nlink = n->type == VNODE_DIR ? 2 : 1;
    st->size = (int64_t)n->size;
    st->blocks = (int64_t)((n->size + 511) / 512);
    st->mtime = st->atime = n->mtime;
    st->ctime = n->ctime ? n->ctime : n->mtime;
    st->dev = 0x10 + (uint64_t)(uintptr_t)n->sb % 251;
    if (d) {
        /* A disk "sd" major 8: minor = registry position, so every device is different. */
        int index = 0;
        while (block_at(index) && block_at(index) != d) {
            index++;
        }
        st->mode = S_IFBLK | (n->mode & 0777);
        st->rdev = (8ull << 8) | (uint64_t)(index & 0xFF) | ((uint64_t)(index & ~0xFF) << 12);
        st->blksize = d->sector_size;
    } else {
        st->mode = (n->type == VNODE_DIR ? S_IFDIR : S_IFREG) | (n->mode & 0777);
    }
}

static int64_t l_fstatat(struct process *p, int64_t dirfd, uint64_t upath, uint64_t ustat, uint64_t flags) {
    struct linux_stat st;
    char path[VFS_PATH_MAX];
    int64_t err = upath ? user_path(p, upath, path) : (path[0] = '\0', 0);
    if (err) {
        return err;
    }
    if (!path[0] && (flags & AT_EMPTY_PATH || !upath)) {
        struct file *f = get_file(p, dirfd);
        if (!f) {
            return -EBADF;
        }
        fill_stat(&st, f, f->kind == FILE_VNODE ? f->vnode : NULL);
    } else {
        struct vnode *dir, *n;
        err = at_dir(p, dirfd, path, &dir);
        if (!err) {
            err = vfs_resolve(dir, path, &n);
        }
        if (err) {
            return err;
        }
        fill_stat(&st, NULL, n);
        vfs_put(n);
    }
    return copy_to_user(p->space, ustat, &st, sizeof(st)) ? 0 : -EFAULT;
}

static int64_t l_getdents64(struct process *p, int64_t fd, uint64_t ubuf, uint64_t size) {
    struct file *f = get_file(p, fd);
    if (!f || f->kind != FILE_VNODE) {
        return -EBADF;
    }
    if (f->vnode->type != VNODE_DIR) {
        return -ENOTDIR;
    }
    uint64_t used = 0;
    uint8_t rec[24 + VFS_NAME_MAX + 8];
    for (;;) {
        struct vnode *child;
        if (vfs_readdir(f->vnode, f->offset, &child) != 0) {
            break;
        }
        size_t name_len = strlen(child->name);
        size_t reclen = (19 + name_len + 1 + 7) & ~7ul;
        if (used + reclen > size) {
            vfs_put(child);
            if (!used) {
                return -EINVAL;
            }
            break;
        }
        memset(rec, 0, reclen);
        uint64_t ino = (uint64_t)(uintptr_t)child >> 4, off = f->offset + 1;
        memcpy(rec, &ino, 8);
        memcpy(rec + 8, &off, 8);
        uint16_t rl = (uint16_t)reclen;
        memcpy(rec + 16, &rl, 2);
        rec[18] = devfs_device(child) ? 6 : child->type == VNODE_DIR ? 4 : 8;   /* DT_BLK / DT_DIR / DT_REG */
        memcpy(rec + 19, child->name, name_len + 1);
        vfs_put(child);
        if (!copy_to_user(p->space, ubuf + used, rec, reclen)) {
            return -EFAULT;
        }
        used += reclen;
        f->offset++;
    }
    return (int64_t)used;
}

static int sync_file(struct file *f) {
    if (!f || f->kind != FILE_VNODE) {
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

static void sync_all(void) {
    for (int i = 0;; i++) {
        struct block_device *d = block_at(i);
        if (!d) {
            break;
        }
        if (d->disk == d && !d->gone) {
            bio_sync(d);
        }
    }
}

/* --- ioctl --------------------------------------------------------------------------------------------------- */

static int64_t put_user(struct process *p, uint64_t addr, const void *v, size_t len) {
    return copy_to_user(p->space, addr, v, len) ? 0 : -EFAULT;
}

static int64_t l_ioctl(struct process *p, int64_t fd, uint64_t req, uint64_t arg) {
    req &= 0xFFFFFFFF;   /* an unsigned int in Linux; musl passes an int, so 0x8008... comes sign-extended */
    struct file *f = get_file(p, fd);
    if (!f) {
        return -EBADF;
    }
    if (is_tty(f) || f->kind == FILE_PTY_MASTER) {
        switch (req) {
        case 0x5401: {   /* TCGETS: a plain terminal */
            uint8_t termios[60] = { 0 };
            return put_user(p, arg, termios, sizeof(termios));
        }
        case 0x5402: case 0x5403: case 0x5404:   /* TCSETS* */
            return 0;
        case 0x5413: {   /* TIOCGWINSZ */
            uint16_t ws[4] = { 25, 80, 0, 0 };
            return put_user(p, arg, ws, sizeof(ws));
        }
        case 0x540F: {   /* TIOCGPGRP */
            int32_t pg = (int32_t)p->pid;
            return put_user(p, arg, &pg, 4);
        }
        default:
            return -ENOTTY;
        }
    }
    struct block_device *d = f->kind == FILE_VNODE ? devfs_device(f->vnode) : NULL;
    if (!d) {
        return -ENOTTY;
    }
    uint64_t bytes = d->sectors * d->sector_size;
    switch (req) {
    case 0x80081272:   /* BLKGETSIZE64 */
        return put_user(p, arg, &bytes, 8);
    case 0x1260: {     /* BLKGETSIZE: 512-byte sectors */
        uint64_t s = bytes / 512;
        return put_user(p, arg, &s, 8);
    }
    case 0x1268:       /* BLKSSZGET */
    case 0x127B: {     /* BLKPBSZGET */
        int32_t s = (int32_t)d->sector_size;
        return put_user(p, arg, &s, 4);
    }
    case 0x1278: case 0x1279: case 0x127A: {   /* BLKIOMIN, BLKIOOPT, BLKALIGNOFF */
        int32_t z = 0;
        return put_user(p, arg, &z, 4);
    }
    case 0x125E: {     /* BLKROGET */
        int32_t ro = d->read_only || d->disk->read_only;
        return put_user(p, arg, &ro, 4);
    }
    case 0x1261:       /* BLKFLSBUF */
        return bio_sync(d);
    case 0x125F:       /* BLKRRPART: read the partition table again */
        return block_rescan(d);
    case 0x1269:       /* BLKPG: add/remove/resize one partition (libparted). Ours come from reading
                          the table again (the Disks app rescans after a change), so: done. */
        return 0;
    case 0x0301: {     /* HDIO_GETGEO: heads, sectors, cylinders, start (in 512-byte sectors) */
        uint8_t geo[16] = { 0 };
        geo[0] = 255;
        geo[1] = 63;
        uint64_t cyl = bytes / 512 / (255 * 63), start = d->start * (d->sector_size / 512);
        uint16_t c16 = cyl > 0xFFFF ? 0xFFFF : (uint16_t)cyl;
        memcpy(geo + 2, &c16, 2);
        memcpy(geo + 8, &start, 8);
        return put_user(p, arg, geo, sizeof(geo));
    }
    case 0x1277:       /* BLKDISCARD */
    case 0x127F:       /* BLKZEROOUT */
    case 0x127D:       /* BLKSECDISCARD */
        return -EOPNOTSUPP;
    default:
        return -ENOTTY;
    }
}

/* --- everything else ----------------------------------------------------------------------------------------- */

static int64_t l_uname(struct process *p, uint64_t ubuf) {
    char u[6][65];
    memset(u, 0, sizeof(u));
    strlcpy(u[0], "Linux", 65);            /* (what the tools expect to be running on) */
    strlcpy(u[1], "viralos", 65);
    strlcpy(u[2], "6.0.0-viralKernel", 65);
    strlcpy(u[3], "#1", 65);
    strlcpy(u[4], "x86_64", 65);
    return put_user(p, ubuf, u, sizeof(u));
}

static int64_t l_clock_gettime(struct process *p, int64_t clock, uint64_t uts) {
    uint64_t ms = uptime_ms();
    int64_t ts[2];
    if (clock == 0 || clock == 8 || clock == 11) {   /* REALTIME, REALTIME_ALARM, TAI */
        ts[0] = (int64_t)rtc_time();
        ts[1] = (int64_t)(ms % 1000) * 1000000;
    } else {
        ts[0] = (int64_t)(ms / 1000);
        ts[1] = (int64_t)(ms % 1000) * 1000000;
    }
    return put_user(p, uts, ts, sizeof(ts));
}

static int64_t l_rlimit(struct process *p, int64_t resource, uint64_t uold) {
    if (!uold) {
        return 0;
    }
    uint64_t lim[2] = { ~0ull, ~0ull };
    if (resource == 3) {          /* RLIMIT_STACK */
        lim[0] = lim[1] = LINUX_STACK_SIZE;
    } else if (resource == 7) {   /* RLIMIT_NOFILE */
        lim[0] = lim[1] = PROCESS_MAX_FILES;
    }
    return put_user(p, uold, lim, sizeof(lim));
}

static int64_t l_sysinfo(struct process *p, uint64_t ubuf) {
    uint8_t s[112] = { 0 };
    uint64_t up = uptime_ms() / 1000, total = pmm_total_bytes(), free = pmm_free_bytes();
    memcpy(s, &up, 8);
    memcpy(s + 32, &total, 8);
    memcpy(s + 40, &free, 8);
    uint32_t unit = 1;
    memcpy(s + 104, &unit, 4);
    return put_user(p, ubuf, s, sizeof(s));
}

static int64_t l_getrandom(struct process *p, uint64_t ubuf, uint64_t len) {
    uint8_t chunk[256];
    uint64_t done = 0;
    while (done < len) {
        size_t n = len - done < sizeof(chunk) ? (size_t)(len - done) : sizeof(chunk);
        random_bytes(chunk, n);
        if (!copy_to_user(p->space, ubuf + done, chunk, n)) {
            return -EFAULT;
        }
        done += n;
    }
    memset(chunk, 0, sizeof(chunk));
    return (int64_t)len;
}

static int64_t l_fcntl(struct process *p, int64_t fd, int64_t cmd, uint64_t arg) {
    struct file *f = get_file(p, fd);
    if (!f) {
        return -EBADF;
    }
    switch (cmd) {
    case 0:       /* F_DUPFD */
    case 1030:    /* F_DUPFD_CLOEXEC */
        file_get(f);
        return install(p, f, (int)arg);
    case 1:       /* F_GETFD */
    case 2:       /* F_SETFD */
    case 4:       /* F_SETFL */
    case 6: case 7: case 37: case 38:   /* F_SETLK(W), F_OFD_SETLK(W): nobody else locks anything */
        return 0;
    case 3:       /* F_GETFL */
        return f->flags;
    case 5: case 36: {   /* F_GETLK: unlocked */
        int16_t unlocked = 2;
        return put_user(p, arg, &unlocked, 2);
    }
    default:
        return -EINVAL;
    }
}

static int64_t l_dup(struct process *p, int64_t fd, int64_t to) {
    struct file *f = get_file(p, fd);
    if (!f) {
        return -EBADF;
    }
    if (to < 0) {
        file_get(f);
        return install(p, f, 0);
    }
    if (to >= PROCESS_MAX_FILES) {
        return -EBADF;
    }
    if (to == fd) {
        return to;
    }
    file_get(f);
    if (p->files[to]) {
        file_put(p->files[to]);
    }
    p->files[to] = f;
    return to;
}

static int64_t l_path_op(struct process *p, int64_t dirfd, uint64_t upath, int what, uint64_t flags) {
    char path[VFS_PATH_MAX];
    struct vnode *dir;
    int64_t err = user_path(p, upath, path);
    if (!err) {
        err = at_dir(p, dirfd, path, &dir);
    }
    if (err) {
        return err;
    }
    switch (what) {
    case 0: {   /* access */
        struct vnode *n;
        err = vfs_resolve(dir, path, &n);
        if (!err) {
            vfs_put(n);
        }
        return err;
    }
    case 1:     /* mkdir */
        return vfs_create(dir, path, VNODE_DIR, 0755, NULL);
    case 2: {   /* unlink / rmdir */
        struct vnode *n;
        err = vfs_resolve(dir, path, &n);
        if (err) {
            return err;
        }
        bool is_dir = n->type == VNODE_DIR;
        vfs_put(n);
        if (is_dir != (flags == AT_REMOVEDIR)) {
            return is_dir ? -EISDIR : -ENOTDIR;
        }
        return vfs_unlink(dir, path);
    }
    case 3: {   /* chdir */
        struct vnode *n;
        err = vfs_resolve(dir, path, &n);
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
    default:
        return -ENOSYS;
    }
}

static int64_t l_rename(struct process *p, int64_t olddir, uint64_t uold, int64_t newdir, uint64_t unew) {
    char from[VFS_PATH_MAX], to[VFS_PATH_MAX];
    struct vnode *d1, *d2;
    int64_t err = user_path(p, uold, from);
    if (!err) {
        err = user_path(p, unew, to);
    }
    if (!err) {
        err = at_dir(p, olddir, from, &d1);
    }
    if (!err) {
        err = at_dir(p, newdir, to, &d2);
    }
    if (err) {
        return err;
    }
    if (d1 != p->cwd || d2 != p->cwd) {
        return -ENOSYS;   /* (only paths relative to the working folder, or absolute) */
    }
    struct vnode *existing;
    if (vfs_resolve(p->cwd, to, &existing) == 0) {
        vfs_put(existing);
        err = vfs_unlink(p->cwd, to);   /* rename() replaces */
        if (err) {
            return err;
        }
    }
    return vfs_rename(p->cwd, from, to);
}

static int64_t l_getcwd(struct process *p, uint64_t ubuf, uint64_t size) {
    char path[VFS_PATH_MAX];
    vfs_path(p->cwd, path, sizeof(path));
    size_t len = strlen(path) + 1;
    if (size < len) {
        return -ERANGE;
    }
    return copy_to_user(p->space, ubuf, path, len) ? (int64_t)len : -EFAULT;
}

static int64_t l_sleep(struct process *p, uint64_t uts) {
    int64_t ts[2];
    if (!copy_from_user(p->space, ts, uts, sizeof(ts))) {
        return -EFAULT;
    }
    uint64_t ms = (uint64_t)ts[0] * 1000 + (uint64_t)ts[1] / 1000000;
    sched_sleep_ms(ms ? ms : (ts[1] ? 1 : 0));
    return 0;
}

int64_t linux_syscall(struct process *p, struct interrupt_frame *f) {
    uint64_t a1 = f->rdi, a2 = f->rsi, a3 = f->rdx, a4 = f->r10, a5 = f->r8;
    (void)a5;
    switch (f->rax) {
    case 0:   return l_read(p, (int64_t)a1, a2, a3, -1);
    case 1:   return l_write(p, (int64_t)a1, a2, a3, -1);
    case 2:   return l_openat(p, AT_FDCWD, a1, a2);
    case 3: {   /* close */
        struct file *file = get_file(p, (int64_t)a1);
        if (!file) {
            return -EBADF;
        }
        p->files[a1] = NULL;
        file_put(file);
        return 0;
    }
    case 4:   /* stat */
    case 6:   return l_fstatat(p, AT_FDCWD, a1, a2, 0);   /* lstat: there are no links */
    case 5:   return l_fstatat(p, (int64_t)a1, 0, a2, AT_EMPTY_PATH);
    case 7:   return (int64_t)a2;   /* poll: everything is ready */
    case 8:   return l_lseek(p, (int64_t)a1, (int64_t)a2, (int64_t)a3);
    case 9:   return l_mmap(p, a1, a2, a3, a4, (int64_t)a5);
    case 10:  return 0;             /* mprotect */
    case 11:  return l_munmap(p, a1, a2);
    case 12:  return (int64_t)(a1 ? process_brk(p, a1) : p->heap_end);
    case 13:  /* rt_sigaction: no signals are ever delivered */
        if (a3) {
            uint8_t zero[32] = { 0 };
            return put_user(p, a3, zero, sizeof(zero));
        }
        return 0;
    case 14:  /* rt_sigprocmask */
        if (a3) {
            uint64_t zero = 0;
            return put_user(p, a3, &zero, 8);
        }
        return 0;
    case 16:  return l_ioctl(p, (int64_t)a1, a2, a3);
    case 17:  return l_read(p, (int64_t)a1, a2, a3, (int64_t)a4);
    case 18:  return l_write(p, (int64_t)a1, a2, a3, (int64_t)a4);
    case 19:  return l_iov(p, (int64_t)a1, a2, (int64_t)a3, false, -1);
    case 20:  return l_iov(p, (int64_t)a1, a2, (int64_t)a3, true, -1);
    case 21:  return l_path_op(p, AT_FDCWD, a1, 0, 0);   /* access */
    case 24:  sched_yield(); return 0;
    case 25:  return -ENOMEM;       /* mremap: musl then allocates and copies */
    case 28:  return 0;             /* madvise */
    case 32:  return l_dup(p, (int64_t)a1, -1);
    case 33:  return l_dup(p, (int64_t)a1, (int64_t)a2);
    case 35:  return l_sleep(p, a1);
    case 39: case 110: case 186: case 111: case 121: case 124: case 112:
        return (int64_t)p->pid;    /* getpid, getppid, gettid, getpgrp, getpgid, getsid, setsid */
    case 60: case 231:
        process_exit((int)(a1 & 0xFF));
    case 62: case 200: case 234: {  /* kill, tkill, tgkill: only ever ourselves (abort()) */
        uint64_t target = f->rax == 234 ? a2 : a1;
        uint64_t sig = f->rax == 234 ? a3 : a2;
        if (target == p->pid || target == 0) {
            process_exit((int)(128 + (sig & 0x7F)));
        }
        return process_kill(target) ? 0 : -ESRCH;
    }
    case 63:  return l_uname(p, a1);
    case 72:  return l_fcntl(p, (int64_t)a1, (int64_t)a2, a3);
    case 73:  return 0;             /* flock */
    case 74: case 75:               /* fsync, fdatasync */
        return sync_file(get_file(p, (int64_t)a1));
    case 76: case 77: {             /* truncate, ftruncate */
        if (f->rax == 77) {
            struct file *file = get_file(p, (int64_t)a1);
            if (!file || file->kind != FILE_VNODE) {
                return -EBADF;
            }
            return vfs_truncate(file->vnode, a2);
        }
        char path[VFS_PATH_MAX];
        struct vnode *n;
        int64_t err = user_path(p, a1, path);
        if (!err) {
            err = vfs_resolve(p->cwd, path, &n);
        }
        if (err) {
            return err;
        }
        err = vfs_truncate(n, a2);
        vfs_put(n);
        return err;
    }
    case 79:  return l_getcwd(p, a1, a2);
    case 80:  return l_path_op(p, AT_FDCWD, a1, 3, 0);   /* chdir */
    case 82:  return l_rename(p, AT_FDCWD, a1, AT_FDCWD, a2);
    case 83:  return l_path_op(p, AT_FDCWD, a1, 1, 0);   /* mkdir */
    case 84:  return l_path_op(p, AT_FDCWD, a1, 2, AT_REMOVEDIR);
    case 87:  return l_path_op(p, AT_FDCWD, a1, 2, 0);   /* unlink */
    case 89: case 267: return -EINVAL;                   /* readlink(at): no links */
    case 90: case 91: case 92: case 93: case 94: case 132: case 235: case 260: case 268: case 280:
        return 0;                   /* chmod, fchmod, chown, fchown, lchown, utime(s), fchownat, fchmodat, utimensat */
    case 95:  return 022;           /* umask */
    case 96: {                      /* gettimeofday */
        if (!a1) {
            return 0;
        }
        int64_t tv[2] = { (int64_t)rtc_time(), (int64_t)(uptime_ms() % 1000) * 1000 };
        return put_user(p, a1, tv, sizeof(tv));
    }
    case 97:  return l_rlimit(p, (int64_t)a1, a2);
    case 99:  return l_sysinfo(p, a1);
    case 102: case 104: case 107: case 108: return 0;   /* root */
    case 105: case 106: case 113: case 114: case 117: case 119: return 0;   /* set*id */
    case 131: return 0;             /* sigaltstack */
    case 158:                       /* arch_prctl */
        if (a1 == 0x1002) {         /* ARCH_SET_FS */
            p->thread->fs_base = a2;
            wrmsr(MSR_FS_BASE, a2);
            return 0;
        }
        if (a1 == 0x1003) {         /* ARCH_GET_FS */
            return put_user(p, a2, &p->thread->fs_base, 8);
        }
        return -EINVAL;
    case 162: sync_all(); return 0;
    case 201: {                     /* time */
        int64_t t = (int64_t)rtc_time();
        if (a1 && put_user(p, a1, &t, 8)) {
            return -EFAULT;
        }
        return t;
    }
    case 202: return 0;             /* futex: single-threaded, nothing ever waits */
    case 204: {                     /* sched_getaffinity: one CPU */
        uint64_t mask = 1;
        if (a2 < 8) {
            return -EINVAL;
        }
        return put_user(p, a3, &mask, 8) ? -EFAULT : 8;
    }
    case 217: return l_getdents64(p, (int64_t)a1, a2, a3);
    case 218: return (int64_t)p->pid;   /* set_tid_address */
    case 228: return l_clock_gettime(p, (int64_t)a1, a2);
    case 229: {                     /* clock_getres: 1 ms */
        int64_t ts[2] = { 0, 1000000 };
        return a2 ? put_user(p, a2, ts, sizeof(ts)) : 0;
    }
    case 230: return l_sleep(p, a3);   /* clock_nanosleep (relative) */
    case 257: return l_openat(p, (int64_t)a1, a2, a3);
    case 258: return l_path_op(p, (int64_t)a1, a2, 1, 0);
    case 262: return l_fstatat(p, (int64_t)a1, a2, a3, a4);
    case 263: return l_path_op(p, (int64_t)a1, a2, 2, a3);
    case 264: case 316: return l_rename(p, (int64_t)a1, a2, (int64_t)a3, a4);
    case 269: case 439: return l_path_op(p, (int64_t)a1, a2, 0, 0);   /* faccessat(2) */
    case 273: return 0;             /* set_robust_list */
    case 292: return l_dup(p, (int64_t)a1, (int64_t)a2);   /* dup3 */
    case 295: return l_iov(p, (int64_t)a1, a2, (int64_t)a3, false, (int64_t)a4);   /* preadv */
    case 296: return l_iov(p, (int64_t)a1, a2, (int64_t)a3, true, (int64_t)a4);    /* pwritev */
    case 302: return l_rlimit(p, (int64_t)a2, a4);   /* prlimit64 */
    case 306: return 0;             /* syncfs */
    case 318: return l_getrandom(p, a1, a2);
    default:  return -ENOSYS;       /* fork, exec, sockets, statx...: musl falls back where it can */
    }
}
