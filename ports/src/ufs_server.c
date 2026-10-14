#include "ufs_server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

/* Our system calls, from a Linux program: the number + OS_LINUX_NATIVE_BASE, results as -errno. */
static long native(long n, long a, long b, long c, long d, long e) {
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n + OS_LINUX_NATIVE_BASE), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}

int ufs_add_entry(uint8_t *buf, size_t size, size_t *used, uint64_t node, uint64_t bytes, int64_t mtime,
                  int is_dir, const char *name, size_t name_len) {
    size_t need = OS_USERFS_ENTRY_SIZE(name_len);
    if (name_len > 255 || *used + need > size) {
        return 0;
    }
    struct os_userfs_entry *e = (struct os_userfs_entry *)(buf + *used);
    memset(e, 0, need);
    e->node = node;
    e->size = bytes;
    e->mtime = mtime;
    e->type = is_dir ? OS_DIRECTORY : OS_FILE;
    e->name_len = (uint16_t)name_len;
    memcpy(e->name, name, name_len);
    *used += need;
    return 1;
}

int ufs_serve(struct os_userfs *desc, const struct ufs_ops *ops) {
    long chan = native(SYS_USERFS, (long)desc, 0, 0, 0, 0);
    if (chan < 0) {
        fprintf(stderr, "%s: the kernel wouldn't mount it: %s\n", desc->device, strerror((int)-chan));
        return (int)chan;
    }
    uint64_t size = 0;
    long addr = native(SYS_SHM_MAP, desc->buffer_fd, (long)&size, 0, 0, 0);
    if (addr < 0 || size < OS_USERFS_BUFFER) {
        fprintf(stderr, "%s: no shared buffer\n", desc->device);
        return -EIO;
    }
    uint8_t *buf = (uint8_t *)addr;

    static struct os_userfs_request req;
    for (;;) {
        int32_t handle = -1;
        long n = native(SYS_MSG_RECV, chan, (long)&req, sizeof(req), (long)&handle, 0);
        if (handle >= 0) {
            close(handle);
        }
        if (n <= 0) {
            /* The kernel let go without asking (the disk was pulled out): save what we can. */
            ops->destroy();
            return 0;
        }
        if (n != sizeof(req)) {
            continue;
        }
        req.name[sizeof(req.name) - 1] = 0;
        req.new_name[sizeof(req.new_name) - 1] = 0;
        struct os_userfs_reply rep = { .seq = req.seq };
        size_t len = req.length > OS_USERFS_BUFFER ? OS_USERFS_BUFFER : (size_t)req.length;
        int more = 0;
        switch (req.op) {
        case OS_UFS_LIST: {
            int64_t count = 0;
            int err = ops->list(req.node, req.offset, buf, OS_USERFS_BUFFER, &count, &more);
            rep.result = err ? err : count;
            rep.more = (uint32_t)more;
            break;
        }
        case OS_UFS_READ:     rep.result = ops->read(req.node, req.offset, buf, len); break;
        case OS_UFS_WRITE:    rep.result = ops->write(req.node, req.offset, buf, len); break;
        case OS_UFS_TRUNCATE: rep.result = ops->truncate(req.node, req.length); break;
        case OS_UFS_CREATE:   rep.result = ops->create(req.dir, req.name, req.type == OS_DIRECTORY, &rep.node); break;
        case OS_UFS_UNLINK:   rep.result = ops->unlink(req.dir, req.node, req.name, req.type == OS_DIRECTORY); break;
        case OS_UFS_RENAME:
            rep.result = ops->rename(req.dir, req.node, req.name, req.new_dir, req.new_name, req.type == OS_DIRECTORY);
            break;
        case OS_UFS_STATFS:   rep.result = ops->statfs(&rep.total, &rep.free); break;
        case OS_UFS_SYNC:     rep.result = ops->sync(); break;
        case OS_UFS_DESTROY:
            /* Everything goes to the disk, then the answer; then out at once. Closing anything here
               (the device included) would wait for the kernel, which is busy unmounting until we've
               answered: exiting leaves the closing to the kernel, after the unmount. */
            rep.result = ops->destroy();
            native(SYS_MSG_SEND, chan, (long)&rep, sizeof(rep), -1, 0);
            _exit(0);
        default:              rep.result = -ENOSYS; break;
        }
        if (rep.result < 0 && rep.result != -ENOENT && rep.result != -ENOTEMPTY && rep.result != -EEXIST) {
            fprintf(stderr, "%s: request %u (%s) failed: %s\n", desc->device, req.op, req.name,
                    strerror((int)-rep.result));
        }
        if (native(SYS_MSG_SEND, chan, (long)&rep, sizeof(rep), -1, 0) < 0) {
            ops->destroy();
            return 0;
        }
    }
}
