/* Sockets, addresses and host names (<sys/socket.h>, <arpa/inet.h>, <netdb.h>), and the network
   adapters (<unistd.h>: netif_info, netif_set). */

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "syscall.h"

int socket(int domain, int type, int protocol) {
    return (int)syscall3(SYS_SOCKET, domain, type, protocol);
}

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    return (int)syscall3(SYS_BIND, fd, (long)addr, len);
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    return (int)syscall3(SYS_CONNECT, fd, (long)addr, len);
}

int listen(int fd, int backlog) {
    return (int)syscall3(SYS_LISTEN, fd, backlog, 0);
}

int accept(int fd, struct sockaddr *addr, socklen_t *len) {
    return (int)syscall3(SYS_ACCEPT, fd, (long)addr, (long)len);
}

long sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen) {
    struct os_sockmsg m = { .buf = (void *)buf, .len = len, .addr = (void *)to, .addrlen = tolen, .flags = flags };
    return syscall3(SYS_SENDTO, fd, (long)&m, 0);
}

long recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen) {
    struct os_sockmsg m = { .buf = buf, .len = len, .addr = from, .addrlen = fromlen ? *fromlen : 0, .flags = flags };
    if (!fromlen) {
        m.addr = NULL;
    }
    long r = syscall3(SYS_RECVFROM, fd, (long)&m, 0);
    if (r >= 0 && fromlen) {
        *fromlen = m.addrlen;
    }
    return r;
}

long send(int fd, const void *buf, size_t len, int flags) {
    return sendto(fd, buf, len, flags, NULL, 0);
}

long recv(int fd, void *buf, size_t len, int flags) {
    return recvfrom(fd, buf, len, flags, NULL, NULL);
}

int setsockopt(int fd, int level, int name, const void *value, socklen_t len) {
    return (int)syscall5(SYS_SETSOCKOPT, fd, level, name, (long)value, len);
}

int getsockopt(int fd, int level, int name, void *value, socklen_t *len) {
    return (int)syscall5(SYS_GETSOCKOPT, fd, level, name, (long)value, (long)len);
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
    return (int)syscall4(SYS_SOCKNAME, fd, 0, (long)addr, (long)len);
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len) {
    return (int)syscall4(SYS_SOCKNAME, fd, 1, (long)addr, (long)len);
}

int shutdown(int fd, int how) {
    return (int)syscall3(SYS_SHUTDOWN, fd, how, 0);
}

/* --- addresses --------------------------------------------------------------------------------- */

int inet_aton(const char *s, struct in_addr *out) {
    uint32_t parts[4];
    int n = 0;
    while (n < 4) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s++ - '0');
            if (++digits > 3 || v > 255) {
                return 0;
            }
        }
        parts[n++] = v;
        if (n < 4) {
            if (*s != '.') {
                return 0;
            }
            s++;
        }
    }
    if (*s) {
        return 0;
    }
    out->s_addr = htonl(parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3]);
    return 1;
}

in_addr_t inet_addr(const char *s) {
    struct in_addr a;
    return inet_aton(s, &a) ? a.s_addr : INADDR_NONE;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (af != AF_INET) {
        return NULL;
    }
    const uint8_t *b = src;
    char tmp[INET_ADDRSTRLEN];
    int n = snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    if (n < 0 || (socklen_t)n >= size) {
        return NULL;
    }
    memcpy(dst, tmp, (size_t)n + 1);
    return dst;
}

const char *inet_ntoa(struct in_addr a) {
    static char buf[INET_ADDRSTRLEN];
    return inet_ntop(AF_INET, &a, buf, sizeof(buf));
}

int inet_pton(int af, const char *src, void *dst) {
    if (af != AF_INET) {
        return -1;
    }
    return inet_aton(src, dst);
}

/* --- host names ------------------------------------------------------------------------------- */

int resolve_host(const char *name, struct in_addr *out) {
    uint32_t a;
    int r = (int)syscall3(SYS_RESOLVE, (long)name, (long)&a, 0);
    if (r == 0) {
        out->s_addr = a;
    }
    return r;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET) {
        return EAI_FAMILY;
    }
    uint16_t port = 0;
    if (service) {
        char *end;
        long p = strtol(service, &end, 10);
        if (*end || p < 0 || p > 65535) {
            if (strcmp(service, "http") == 0) {
                p = 80;
            } else if (strcmp(service, "https") == 0) {
                p = 443;
            } else {
                return EAI_SERVICE;
            }
        }
        port = (uint16_t)p;
    }
    struct in_addr a = { 0 };
    if (node) {
        if (!inet_aton(node, &a)) {
            if (hints && (hints->ai_flags & AI_NUMERICHOST)) {
                return EAI_NONAME;
            }
            int r = resolve_host(node, &a);
            if (r == -ENOENT) {
                return EAI_NONAME;
            }
            if (r < 0) {
                return EAI_AGAIN;
            }
        }
    } else if (!hints || !(hints->ai_flags & AI_PASSIVE)) {
        a.s_addr = htonl(INADDR_LOOPBACK);
    }
    struct addrinfo *ai = calloc(1, sizeof(*ai) + sizeof(struct sockaddr_in));
    if (!ai) {
        return EAI_MEMORY;
    }
    struct sockaddr_in *sin = (struct sockaddr_in *)(ai + 1);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    sin->sin_addr = a;
    ai->ai_family = AF_INET;
    ai->ai_socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    ai->ai_addrlen = sizeof(*sin);
    ai->ai_addr = (struct sockaddr *)sin;
    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res);
        res = next;
    }
}

const char *gai_strerror(int err) {
    switch (err) {
    case EAI_NONAME:  return "Name or service not known";
    case EAI_AGAIN:   return "Temporary failure in name resolution";
    case EAI_FAIL:    return "Non-recoverable failure in name resolution";
    case EAI_FAMILY:  return "Address family not supported";
    case EAI_SERVICE: return "Service not known";
    case EAI_MEMORY:  return "Out of memory";
    default:          return "Unknown error";
    }
}

/* --- adapters, Ctrl+C and the fine clock ------------------------------------------------------- */

int netif_info(int index, struct os_netif_info *info) {
    return (int)syscall3(SYS_NETIF, index, (long)info, 0);
}

int netif_set(const struct os_netif_config *config) {
    return (int)syscall3(SYS_NETIF_SET, (long)config, 0, 0);
}

int interrupt_mode(int op) {
    return (int)syscall3(SYS_INTERRUPT, op, 0, 0);
}

uint64_t uptime_us(void) {
    return (uint64_t)syscall3(SYS_UPTIME_US, 0, 0, 0);
}
