/*
 * Sockets for programs: the SYS_SOCKET... system calls, each a thin layer over lwIP's sockets API
 * (lwip/sockets.h). A socket is a struct file of kind FILE_SOCKET whose `end` is lwIP's socket number.
 *
 * Programs use Linux's constants and address layout (<viralos.h>: OS_AF_INET, struct sockaddr_in
 * with a 16-bit family); lwIP has its own (BSD's sin_len + 8-bit family, SOL_SOCKET = 0xfff...), so
 * everything is translated here. Data goes through a kernel buffer (lwIP copies it again anyway).
 */

#include "net/net.h"

#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "ipc/ipc.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/file.h"
#include "proc/process.h"
#include "proc/usercopy.h"

#define IO_MAX (64 * 1024)   /* bytes per send/recv call */

/* Linux's struct sockaddr_in, as programs pass it. */
struct user_sockaddr_in {
    uint16_t family;
    uint16_t port;           /* network byte order */
    uint32_t addr;           /* network byte order */
    uint8_t zero[8];
};

void net_socket_event(void) {
    ipc_notify();   /* someone may be in poll() for it */
}

static int fail(void) {
    int e = errno;
    return e ? -e : -EIO;
}

static struct file *socket_file(struct process *p, uint64_t fd) {
    struct file *f = fd < PROCESS_MAX_FILES ? p->files[fd] : NULL;
    return f && f->kind == FILE_SOCKET ? f : NULL;
}

#define GET_SOCKET(p, fd, s)                          \
    struct file *f_ = socket_file(p, fd);             \
    if (!f_) {                                        \
        return (fd < PROCESS_MAX_FILES && p->files[fd]) ? -ENOTSOCK : -EBADF; \
    }                                                 \
    int s = f_->end

/* A program's address -> lwIP's. */
static int address_in(struct process *p, uint64_t uaddr, uint64_t len, struct sockaddr_in *out) {
    struct user_sockaddr_in u;
    if (len < 8 || !uaddr) {
        return -EINVAL;
    }
    memset(&u, 0, sizeof(u));
    if (!copy_from_user(p->space, &u, uaddr, len < sizeof(u) ? len : sizeof(u))) {
        return -EFAULT;
    }
    if (u.family != OS_AF_INET) {
        return -EAFNOSUPPORT;
    }
    memset(out, 0, sizeof(*out));
    out->sin_len = sizeof(*out);
    out->sin_family = AF_INET;
    out->sin_port = u.port;
    out->sin_addr.s_addr = u.addr;
    return 0;
}

/* lwIP's address -> the program's buffer of *ulen bytes; *ulen becomes the full size. */
static int address_out(struct process *p, const struct sockaddr_in *in, uint64_t uaddr, uint64_t ulen) {
    if (!uaddr || !ulen) {
        return 0;
    }
    uint32_t len;
    if (!copy_from_user(p->space, &len, ulen, sizeof(len))) {
        return -EFAULT;
    }
    struct user_sockaddr_in u;
    memset(&u, 0, sizeof(u));
    u.family = OS_AF_INET;
    u.port = in->sin_port;
    u.addr = in->sin_addr.s_addr;
    if (!copy_to_user(p->space, uaddr, &u, len < sizeof(u) ? len : sizeof(u))) {
        return -EFAULT;
    }
    len = sizeof(u);
    return copy_to_user(p->space, ulen, &len, sizeof(len)) ? 0 : -EFAULT;
}

static int msg_flags(int flags) {
    return (flags & OS_MSG_PEEK ? MSG_PEEK : 0) | (flags & OS_MSG_DONTWAIT ? MSG_DONTWAIT : 0);
}

static int64_t install_socket(struct process *p, int s) {
    struct file *f = file_new(FILE_SOCKET);
    if (!f) {
        lwip_close(s);
        return -ENOMEM;
    }
    f->end = s;
    for (int fd = 0; fd < PROCESS_MAX_FILES; fd++) {
        if (!p->files[fd]) {
            p->files[fd] = f;
            return fd;
        }
    }
    file_put(f);
    return -EMFILE;
}

int64_t sys_socket(struct process *p, uint64_t domain, uint64_t type, uint64_t protocol) {
    if (domain != OS_AF_INET) {
        return -EAFNOSUPPORT;
    }
    bool nonblock = type & OS_SOCK_NONBLOCK;
    type &= ~(uint64_t)OS_SOCK_NONBLOCK;
    int t = type == OS_SOCK_STREAM ? SOCK_STREAM : type == OS_SOCK_DGRAM ? SOCK_DGRAM :
            type == OS_SOCK_RAW ? SOCK_RAW : -1;
    if (t < 0) {
        return -EPROTOTYPE;
    }
    errno = 0;
    int s = lwip_socket(AF_INET, t, (int)protocol);
    if (s < 0) {
        return fail();
    }
    if (nonblock) {
        lwip_fcntl(s, F_SETFL, O_NONBLOCK);
    }
    return install_socket(p, s);
}

int64_t sys_bind(struct process *p, uint64_t fd, uint64_t uaddr, uint64_t len) {
    GET_SOCKET(p, fd, s);
    struct sockaddr_in a;
    int err = address_in(p, uaddr, len, &a);
    if (err) {
        return err;
    }
    errno = 0;
    return lwip_bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 ? fail() : 0;
}

int64_t sys_connect(struct process *p, uint64_t fd, uint64_t uaddr, uint64_t len) {
    GET_SOCKET(p, fd, s);
    struct sockaddr_in a;
    int err = address_in(p, uaddr, len, &a);
    if (err) {
        return err;
    }
    errno = 0;
    return lwip_connect(s, (struct sockaddr *)&a, sizeof(a)) < 0 ? fail() : 0;
}

int64_t sys_listen(struct process *p, uint64_t fd, uint64_t backlog) {
    GET_SOCKET(p, fd, s);
    errno = 0;
    return lwip_listen(s, (int)(backlog > 255 ? 255 : backlog)) < 0 ? fail() : 0;
}

int64_t sys_accept(struct process *p, uint64_t fd, uint64_t uaddr, uint64_t ulen) {
    GET_SOCKET(p, fd, s);
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    errno = 0;
    int n = lwip_accept(s, (struct sockaddr *)&a, &len);
    if (n < 0) {
        return process_current_killed() ? -EINTR : fail();
    }
    int err = address_out(p, &a, uaddr, ulen);
    if (err) {
        lwip_close(n);
        return err;
    }
    return install_socket(p, n);
}

int64_t sys_sendto(struct process *p, uint64_t fd, uint64_t umsg) {
    GET_SOCKET(p, fd, s);
    struct os_sockmsg m;
    if (!copy_from_user(p->space, &m, umsg, sizeof(m))) {
        return -EFAULT;
    }
    struct sockaddr_in a;
    if (m.addr) {
        int err = address_in(p, (uint64_t)m.addr, m.addrlen, &a);
        if (err) {
            return err;
        }
    }
    size_t len = m.len < IO_MAX ? m.len : IO_MAX;
    void *buf = kmalloc(len ? len : 1);
    if (!buf) {
        return -ENOMEM;
    }
    if (!copy_from_user(p->space, buf, (uint64_t)m.buf, len)) {
        kfree(buf);
        return -EFAULT;
    }
    errno = 0;
    ssize_t n = m.addr ? lwip_sendto(s, buf, len, msg_flags(m.flags), (struct sockaddr *)&a, sizeof(a))
                       : lwip_send(s, buf, len, msg_flags(m.flags));
    kfree(buf);
    return n < 0 ? fail() : n;
}

int64_t sys_recvfrom(struct process *p, uint64_t fd, uint64_t umsg) {
    GET_SOCKET(p, fd, s);
    struct os_sockmsg m;
    if (!copy_from_user(p->space, &m, umsg, sizeof(m))) {
        return -EFAULT;
    }
    size_t len = m.len < IO_MAX ? m.len : IO_MAX;
    void *buf = kmalloc(len ? len : 1);
    if (!buf) {
        return -ENOMEM;
    }
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    memset(&a, 0, sizeof(a));
    errno = 0;
    ssize_t n = lwip_recvfrom(s, buf, len, msg_flags(m.flags), (struct sockaddr *)&a, &alen);
    if (n < 0) {
        kfree(buf);
        /* (A wait cut short by Ctrl+C or kill looks like a timeout to lwIP.) */
        return process_current_killed() ? -EINTR : fail();
    }
    if (n > 0 && !copy_to_user(p->space, (uint64_t)m.buf, buf, (size_t)n)) {
        kfree(buf);
        return -EFAULT;
    }
    kfree(buf);
    if (m.addr) {
        struct user_sockaddr_in u;
        memset(&u, 0, sizeof(u));
        u.family = OS_AF_INET;
        u.port = a.sin_port;
        u.addr = a.sin_addr.s_addr;
        if (!copy_to_user(p->space, (uint64_t)m.addr, &u, m.addrlen < sizeof(u) ? m.addrlen : sizeof(u))) {
            return -EFAULT;
        }
        uint32_t full = sizeof(u);
        if (!copy_to_user(p->space, umsg + __builtin_offsetof(struct os_sockmsg, addrlen), &full, sizeof(full))) {
            return -EFAULT;
        }
    }
    return n;
}

/* Linux's option numbers -> lwIP's (level and name). False if lwIP has no such option. */
static bool translate_option(uint64_t level, uint64_t name, int *lv, int *nm) {
    if (level == OS_SOL_SOCKET) {
        *lv = SOL_SOCKET;
        switch (name) {
        case OS_SO_REUSEADDR: *nm = SO_REUSEADDR; return true;
        case OS_SO_ERROR:     *nm = SO_ERROR; return true;
        case OS_SO_BROADCAST: *nm = SO_BROADCAST; return true;
        case OS_SO_SNDBUF:    *nm = SO_SNDBUF; return true;
        case OS_SO_RCVBUF:    *nm = SO_RCVBUF; return true;
        case OS_SO_KEEPALIVE: *nm = SO_KEEPALIVE; return true;
        case OS_SO_RCVTIMEO:  *nm = SO_RCVTIMEO; return true;
        case OS_SO_SNDTIMEO:  *nm = SO_SNDTIMEO; return true;
        default:              return false;
        }
    }
    if (level == OS_IPPROTO_IP) {
        *lv = IPPROTO_IP;
        switch (name) {
        case OS_IP_TOS: *nm = IP_TOS; return true;
        case OS_IP_TTL: *nm = IP_TTL; return true;
        default:        return false;
        }
    }
    if (level == OS_IPPROTO_TCP) {
        *lv = IPPROTO_TCP;
        if (name == OS_TCP_NODELAY) {
            *nm = TCP_NODELAY;
            return true;
        }
    }
    return false;
}

static bool is_timeout_option(int lv, int nm) {
    return lv == SOL_SOCKET && (nm == SO_RCVTIMEO || nm == SO_SNDTIMEO);
}

int64_t sys_setsockopt(struct process *p, uint64_t fd, uint64_t level, uint64_t name, uint64_t uval, uint64_t len) {
    GET_SOCKET(p, fd, s);
    int lv, nm;
    if (!translate_option(level, name, &lv, &nm)) {
        return -ENOPROTOOPT;
    }
    errno = 0;
    if (is_timeout_option(lv, nm)) {
        struct os_timeval tv;
        if (len < sizeof(tv) || !copy_from_user(p->space, &tv, uval, sizeof(tv))) {
            return -EFAULT;
        }
        struct timeval t = { .tv_sec = (long)tv.tv_sec, .tv_usec = (long)tv.tv_usec };
        return lwip_setsockopt(s, lv, nm, &t, sizeof(t)) < 0 ? fail() : 0;
    }
    int v = 0;
    if (len < sizeof(int) || !copy_from_user(p->space, &v, uval, sizeof(int))) {
        return -EFAULT;
    }
    if (nm == IP_TTL || nm == IP_TOS) {   /* (lwIP reads these as a byte-sized int too) */
        v &= 0xFF;
    }
    return lwip_setsockopt(s, lv, nm, &v, sizeof(v)) < 0 ? fail() : 0;
}

int64_t sys_getsockopt(struct process *p, uint64_t fd, uint64_t level, uint64_t name, uint64_t uval, uint64_t ulen) {
    GET_SOCKET(p, fd, s);
    int lv, nm;
    if (!translate_option(level, name, &lv, &nm)) {
        return -ENOPROTOOPT;
    }
    uint32_t len;
    if (!copy_from_user(p->space, &len, ulen, sizeof(len))) {
        return -EFAULT;
    }
    errno = 0;
    if (is_timeout_option(lv, nm)) {
        struct timeval t;
        socklen_t tl = sizeof(t);
        if (lwip_getsockopt(s, lv, nm, &t, &tl) < 0) {
            return fail();
        }
        struct os_timeval tv = { t.tv_sec, t.tv_usec };
        if (len < sizeof(tv) || !copy_to_user(p->space, uval, &tv, sizeof(tv))) {
            return -EFAULT;
        }
        len = sizeof(tv);
    } else {
        int v = 0;
        socklen_t vl = sizeof(v);
        if (lwip_getsockopt(s, lv, nm, &v, &vl) < 0) {
            return fail();
        }
        if (len < sizeof(v) || !copy_to_user(p->space, uval, &v, sizeof(v))) {
            return -EFAULT;
        }
        len = sizeof(v);
    }
    return copy_to_user(p->space, ulen, &len, sizeof(len)) ? 0 : -EFAULT;
}

int64_t sys_sockname(struct process *p, uint64_t fd, uint64_t peer, uint64_t uaddr, uint64_t ulen) {
    GET_SOCKET(p, fd, s);
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    errno = 0;
    int r = peer ? lwip_getpeername(s, (struct sockaddr *)&a, &len) : lwip_getsockname(s, (struct sockaddr *)&a, &len);
    if (r < 0) {
        return fail();
    }
    return address_out(p, &a, uaddr, ulen);
}

int64_t sys_shutdown(struct process *p, uint64_t fd, uint64_t how) {
    GET_SOCKET(p, fd, s);
    int h = how == OS_SHUT_RD ? SHUT_RD : how == OS_SHUT_WR ? SHUT_WR : SHUT_RDWR;
    errno = 0;
    return lwip_shutdown(s, h) < 0 ? fail() : 0;
}

int64_t sys_resolve(struct process *p, uint64_t uname, uint64_t uaddr) {
    char name[256];
    int64_t len = copy_string_from_user(p->space, name, uname, sizeof(name));
    if (len < 0) {
        return len;
    }
    uint32_t a;
    int err = net_resolve(name, &a);
    if (err) {
        return err;
    }
    return copy_to_user(p->space, uaddr, &a, sizeof(a)) ? 0 : -EFAULT;
}

int64_t sys_netif(struct process *p, uint64_t index, uint64_t uinfo) {
    struct os_netif_info info;
    int err = net_netif_info((int)index, &info);
    if (err) {
        return err;
    }
    return copy_to_user(p->space, uinfo, &info, sizeof(info)) ? 0 : -EFAULT;
}

int64_t sys_netif_set(struct process *p, uint64_t uconfig) {
    struct os_netif_config c;
    if (!copy_from_user(p->space, &c, uconfig, sizeof(c))) {
        return -EFAULT;
    }
    c.name[sizeof(c.name) - 1] = '\0';
    return net_netif_set(&c);
}

/* --- as a file --------------------------------------------------------------------------------- */

int64_t net_socket_read(struct process *p, struct file *f, uint64_t ubuf, uint64_t len) {
    size_t n = len < IO_MAX ? len : IO_MAX;
    void *buf = kmalloc(n ? n : 1);
    if (!buf) {
        return -ENOMEM;
    }
    errno = 0;
    ssize_t r = lwip_recv(f->end, buf, n, 0);
    if (r < 0) {
        kfree(buf);
        return process_current_killed() ? -EINTR : fail();
    }
    if (r > 0 && !copy_to_user(p->space, ubuf, buf, (size_t)r)) {
        r = -EFAULT;
    }
    kfree(buf);
    return r;
}

int64_t net_socket_write(struct process *p, struct file *f, uint64_t ubuf, uint64_t len) {
    size_t n = len < IO_MAX ? len : IO_MAX;
    void *buf = kmalloc(n ? n : 1);
    if (!buf) {
        return -ENOMEM;
    }
    if (!copy_from_user(p->space, buf, ubuf, n)) {
        kfree(buf);
        return -EFAULT;
    }
    errno = 0;
    ssize_t r = lwip_send(f->end, buf, n, 0);
    kfree(buf);
    return r < 0 ? fail() : r;
}

void net_socket_close(struct file *f) {
    lwip_close(f->end);
}

uint16_t net_socket_poll(struct file *f) {
    struct pollfd pfd = { .fd = f->end, .events = POLLIN | POLLOUT };
    if (lwip_poll(&pfd, 1, 0) < 0) {
        return OS_POLL_ERR;
    }
    return (pfd.revents & POLLIN ? OS_POLL_IN : 0) | (pfd.revents & POLLOUT ? OS_POLL_OUT : 0) |
           (pfd.revents & (POLLERR | POLLNVAL) ? OS_POLL_ERR : 0);
}
