#include <unistd.h>
#include "syscall.h"

int shm_create(uint64_t size) {
    return (int)syscall3(SYS_SHM_CREATE, (long)size, 0, 0);
}

void *shm_map(int fd, uint64_t *size) {
    long r = syscall3(SYS_SHM_MAP, fd, (long)size, 0);
    return r < 0 ? NULL : (void *)r;
}

int shm_unmap(void *address) {
    return (int)syscall3(SYS_SHM_UNMAP, (long)address, 0, 0);
}

int channel_pair(int32_t fds[2]) {
    return (int)syscall3(SYS_CHANNEL_PAIR, (long)fds, 0, 0);
}

int service_register(const char *name) {
    return (int)syscall3(SYS_SERVICE_REGISTER, (long)name, 0, 0);
}

int service_connect(const char *name) {
    return (int)syscall3(SYS_SERVICE_CONNECT, (long)name, 0, 0);
}

int service_accept(int listener) {
    return (int)syscall3(SYS_SERVICE_ACCEPT, listener, 0, 0);
}

int msg_send(int fd, const void *data, size_t len, int handle) {
    return (int)syscall5(SYS_MSG_SEND, fd, (long)data, (long)len, handle, 0);
}

int msg_send_flags(int fd, const void *data, size_t len, int handle, int flags) {
    return (int)syscall5(SYS_MSG_SEND, fd, (long)data, (long)len, handle, flags);
}

long msg_recv(int fd, void *buf, size_t size, int *handle) {
    int32_t h = -1;
    long r = syscall4(SYS_MSG_RECV, fd, (long)buf, (long)size, (long)&h);
    if (handle) {
        *handle = h;
    }
    return r;
}

int poll(struct os_pollfd *fds, int count, long timeout_ms) {
    return (int)syscall3(SYS_POLL, (long)fds, count, timeout_ms);
}

int pty_open(int32_t fds[2]) {
    return (int)syscall3(SYS_PTY_OPEN, (long)fds, 0, 0);
}

int rename(const char *from, const char *to) {
    return (int)syscall3(SYS_RENAME, (long)from, (long)to, 0);
}

int power(int how) {
    return (int)syscall3(SYS_POWER, how, 0, 0);
}

int audio_open(uint32_t rate, uint32_t channels) {
    return (int)syscall3(SYS_AUDIO_OPEN, rate, channels, 0);
}

long audio_write(const void *buf, size_t bytes, int flags) {
    return syscall3(SYS_AUDIO_WRITE, (long)buf, (long)bytes, flags);
}

long audio_ctl(int op, long arg) {
    return syscall3(SYS_AUDIO_CTL, op, arg, 0);
}

int audio_mixer(int op, long value, struct os_audio_mixer *out) {
    return (int)syscall3(SYS_AUDIO_MIXER, op, value, (long)out);
}

int drives(int index, struct os_drive_info *info) {
    return (int)syscall3(SYS_DRIVES, index, (long)info, 0);
}

int eject(const char *path) {
    return (int)syscall3(SYS_EJECT, (long)path, 0, 0);
}

long seek(int fd, long offset, int whence) {
    return syscall3(SYS_SEEK, fd, offset, whence);
}

int fsync(int fd) {
    return (int)syscall3(SYS_FSYNC, fd, 0, 0);
}

int blockdev(int index, struct os_block_info *info) {
    return (int)syscall3(SYS_BLOCKDEV, index, (long)info, 0);
}

int mount_device(const char *device, const char *path) {
    return (int)syscall3(SYS_MOUNT, (long)device, (long)path, 0);
}

int mount_device_writable(const char *device, const char *path) {
    return (int)syscall3(SYS_MOUNT, (long)device, (long)path, OS_MOUNT_WRITABLE);
}

int chmod(const char *path, uint32_t mode) {
    return (int)syscall3(SYS_CHMOD, (long)path, (long)mode, 0);
}

int getrandom(void *buf, size_t len) {
    return (int)syscall3(SYS_GETRANDOM, (long)buf, (long)len, 0);
}

int unmount(const char *path) {
    return (int)syscall3(SYS_UNMOUNT, (long)path, 0, 0);
}

int rescan(const char *disk) {
    return (int)syscall3(SYS_RESCAN, (long)disk, 0, 0);
}

uint64_t wall_time(void) {
    return (uint64_t)syscall3(SYS_TIME, 0, 0, 0);
}

void exit(int code) {
    syscall3(SYS_EXIT, code, 0, 0);
    __builtin_unreachable();
}

long write(int fd, const void *buf, size_t len) {
    return syscall3(SYS_WRITE, fd, (long)buf, (long)len);
}

long read(int fd, void *buf, size_t len) {
    return syscall3(SYS_READ, fd, (long)buf, (long)len);
}

int open(const char *path, int flags) {
    return (int)syscall3(SYS_OPEN, (long)path, flags, 0);
}

int close(int fd) {
    return (int)syscall3(SYS_CLOSE, fd, 0, 0);
}

int stat(const char *path, struct os_file_info *info) {
    return (int)syscall3(SYS_STAT, (long)path, (long)info, 0);
}

int readdir(int fd, uint64_t index, struct os_file_info *info) {
    return (int)syscall3(SYS_READDIR, fd, (long)index, (long)info);
}

int sleep_ms(uint64_t ms) {
    return (int)syscall3(SYS_SLEEP, (long)ms, 0, 0);
}

long getpid(void) {
    return syscall3(SYS_GETPID, 0, 0, 0);
}

uint64_t uptime_ms(void) {
    return (uint64_t)syscall3(SYS_UPTIME, 0, 0, 0);
}

long getcwd(char *buf, size_t size) {
    return syscall3(SYS_GETCWD, (long)buf, (long)size, 0);
}

int chdir(const char *path) {
    return (int)syscall3(SYS_CHDIR, (long)path, 0, 0);
}

int mkdir(const char *path) {
    return (int)syscall3(SYS_MKDIR, (long)path, 0, 0);
}

int unlink(const char *path) {
    return (int)syscall3(SYS_UNLINK, (long)path, 0, 0);
}

long spawn(const struct os_spawn *args) {
    return syscall3(SYS_SPAWN, (long)args, 0, 0);
}

long spawn_simple(const char *path, const char *const *argv) {
    struct os_spawn args = {
        .path = path,
        .argv = argv,
        .fds = { OS_SPAWN_INHERIT, OS_SPAWN_INHERIT, OS_SPAWN_INHERIT },
    };
    return spawn(&args);
}

long wait(long pid, int *status, int flags) {
    return syscall3(SYS_WAIT, pid, (long)status, flags);
}

int kill(long pid) {
    return (int)syscall3(SYS_KILL, pid, 0, 0);
}

int set_foreground(long pid) {
    return (int)syscall3(SYS_SETFG, pid, 0, 0);
}

int procinfo(uint64_t index, struct os_proc_info *info) {
    return (int)syscall3(SYS_PROCINFO, (long)index, (long)info, 0);
}

int sysinfo(struct os_sysinfo *info) {
    return (int)syscall3(SYS_SYSINFO, (long)info, 0, 0);
}

int tty_mode(int mode) {
    return (int)syscall3(SYS_TTY_MODE, mode, 0, 0);
}

int display_claim(struct os_display_info *info) {
    return (int)syscall3(SYS_DISPLAY_CLAIM, (long)info, 0, 0);
}

int display_release(void) {
    return (int)syscall3(SYS_DISPLAY_RELEASE, 0, 0, 0);
}

int input_read(struct os_input_event *events, int max, long timeout_ms) {
    return (int)syscall3(SYS_INPUT_READ, (long)events, max, timeout_ms);
}

uintptr_t brk(uintptr_t new_end) {
    return (uintptr_t)syscall3(SYS_BRK, (long)new_end, 0, 0);
}

void *sbrk(intptr_t increment) {
    static uintptr_t current;
    if (!current) {
        current = brk(0);
    }
    uintptr_t old = current;
    if (increment != 0) {
        uintptr_t wanted = current + (uintptr_t)increment;
        if (brk(wanted) != wanted) {
            return (void *)-1;
        }
        current = wanted;
    }
    return (void *)old;
}

const char *strerror(int err) {
    switch (err < 0 ? -err : err) {
    case 0:            return "success";
    case ENOENT:       return "no such file or directory";
    case EINTR:        return "interrupted";
    case EIO:          return "I/O error";
    case ENOEXEC:      return "not an executable";
    case EBADF:        return "bad file descriptor";
    case ECHILD:       return "no child processes";
    case ENOTEMPTY:    return "directory not empty";
    case ENODEV:       return "no such device";
    case EAGAIN:       return "try again";
    case EPIPE:        return "the other end is closed";
    case EMSGSIZE:     return "message too long";
    case ECONNREFUSED: return "connection refused";
    case ENFILE:       return "too many open files in the system";
    case ERANGE:       return "out of range";
    case ENOTSOCK:     return "not a socket";
    case EDESTADDRREQ: return "destination address required";
    case EPROTOTYPE:   return "wrong protocol for this socket";
    case ENOPROTOOPT:  return "protocol option not available";
    case EPROTONOSUPPORT: return "protocol not supported";
    case EOPNOTSUPP:   return "operation not supported";
    case EAFNOSUPPORT: return "address family not supported";
    case EADDRINUSE:   return "address already in use";
    case EADDRNOTAVAIL: return "address not available";
    case ENETDOWN:     return "network is down";
    case ENETUNREACH:  return "network is unreachable";
    case ECONNABORTED: return "connection aborted";
    case ECONNRESET:   return "connection reset by peer";
    case ENOBUFS:      return "no buffer space available";
    case EISCONN:      return "already connected";
    case ENOTCONN:     return "not connected";
    case ESHUTDOWN:    return "connection shut down";
    case ETIMEDOUT:    return "timed out";
    case EHOSTDOWN:    return "host is down";
    case EHOSTUNREACH: return "no route to host";
    case EALREADY:     return "already in progress";
    case EINPROGRESS:  return "in progress";
    case EBUSY:        return "resource busy";
    case ENOMEM:       return "out of memory";
    case EACCES:       return "permission denied";
    case EFAULT:       return "bad address";
    case EEXIST:       return "file exists";
    case EXDEV:        return "can't move between drives";
    case EROFS:        return "the drive is read-only";
    case ENOTDIR:      return "not a directory";
    case EISDIR:       return "is a directory";
    case EINVAL:       return "invalid argument";
    case EMFILE:       return "too many open files";
    case ENAMETOOLONG: return "name too long";
    case ENOSYS:       return "function not implemented";
    default:           return "unknown error";
    }
}
