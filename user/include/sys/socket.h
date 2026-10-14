#pragma once

/*
 * Sockets (BSD/Linux style), on the kernel's TCP/IP stack. A socket is a file descriptor: read(),
 * write(), close() and poll() work on it too. Functions return >= 0 or a negative error number
 * (-ECONNREFUSED, ...), like the rest of this C library: there's no errno.
 * IPv4 only for now (AF_INET, struct sockaddr_in in <netinet/in.h>).
 */

#include <stddef.h>
#include <stdint.h>
#include <viralos.h>

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char ss_data[126];
};

#define AF_INET       OS_AF_INET
#define PF_INET       OS_AF_INET
#define AF_UNSPEC     0
#define SOCK_STREAM   OS_SOCK_STREAM
#define SOCK_DGRAM    OS_SOCK_DGRAM
#define SOCK_RAW      OS_SOCK_RAW
#define SOCK_NONBLOCK OS_SOCK_NONBLOCK

#define MSG_PEEK      OS_MSG_PEEK
#define MSG_DONTWAIT  OS_MSG_DONTWAIT
#define MSG_NOSIGNAL  0   /* (there are no signals) */

#define SHUT_RD   OS_SHUT_RD
#define SHUT_WR   OS_SHUT_WR
#define SHUT_RDWR OS_SHUT_RDWR

#define SOL_SOCKET    OS_SOL_SOCKET
#define SO_REUSEADDR  OS_SO_REUSEADDR
#define SO_ERROR      OS_SO_ERROR
#define SO_BROADCAST  OS_SO_BROADCAST
#define SO_SNDBUF     OS_SO_SNDBUF
#define SO_RCVBUF     OS_SO_RCVBUF
#define SO_KEEPALIVE  OS_SO_KEEPALIVE
#define SO_RCVTIMEO   OS_SO_RCVTIMEO   /* value: struct timeval (<sys/time.h>) */
#define SO_SNDTIMEO   OS_SO_SNDTIMEO

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
long send(int fd, const void *buf, size_t len, int flags);
long recv(int fd, void *buf, size_t len, int flags);
long sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen);
long recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen);
int setsockopt(int fd, int level, int name, const void *value, socklen_t len);
int getsockopt(int fd, int level, int name, void *value, socklen_t *len);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int shutdown(int fd, int how);
