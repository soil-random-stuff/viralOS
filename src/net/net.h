#pragma once

/*
 * The network stack, as the rest of the kernel sees it (lwIP stays inside src/net/).
 *   net.c     the adapters: lwIP's interfaces over the card drivers, their settings, DHCP, DNS
 *   socket.c  sockets for programs (the SYS_SOCKET... system calls)
 */

#include <stddef.h>
#include <stdint.h>

#include <viralos.h>

struct file;
struct process;

/* Start the TCP/IP stack and the "net" thread (before any card driver registers). */
void net_init(void);
/* Find the network cards and start their drivers (drivers/net/). */
void net_drivers_init(void);

int net_netif_info(int index, struct os_netif_info *out);
int net_netif_set(const struct os_netif_config *config);
/* A host name or dotted address -> IPv4 address (network byte order). Waits for DNS. */
int net_resolve(const char *name, uint32_t *address);

/* socket.c: the system calls (user pointers are checked here) */
int64_t sys_socket(struct process *p, uint64_t domain, uint64_t type, uint64_t protocol);
int64_t sys_bind(struct process *p, uint64_t fd, uint64_t uaddr, uint64_t len);
int64_t sys_connect(struct process *p, uint64_t fd, uint64_t uaddr, uint64_t len);
int64_t sys_listen(struct process *p, uint64_t fd, uint64_t backlog);
int64_t sys_accept(struct process *p, uint64_t fd, uint64_t uaddr, uint64_t ulen);
int64_t sys_sendto(struct process *p, uint64_t fd, uint64_t umsg);
int64_t sys_recvfrom(struct process *p, uint64_t fd, uint64_t umsg);
int64_t sys_setsockopt(struct process *p, uint64_t fd, uint64_t level, uint64_t name, uint64_t uval, uint64_t len);
int64_t sys_getsockopt(struct process *p, uint64_t fd, uint64_t level, uint64_t name, uint64_t uval, uint64_t ulen);
int64_t sys_sockname(struct process *p, uint64_t fd, uint64_t peer, uint64_t uaddr, uint64_t ulen);
int64_t sys_shutdown(struct process *p, uint64_t fd, uint64_t how);
int64_t sys_resolve(struct process *p, uint64_t uname, uint64_t uaddr);
int64_t sys_netif(struct process *p, uint64_t index, uint64_t uinfo);
int64_t sys_netif_set(struct process *p, uint64_t uconfig);

/* read()/write() on a socket (straight to/from the program's memory, in big pieces). */
int64_t net_socket_read(struct process *p, struct file *f, uint64_t ubuf, uint64_t len);
int64_t net_socket_write(struct process *p, struct file *f, uint64_t ubuf, uint64_t len);
/* For file_put and poll. */
void net_socket_close(struct file *f);
uint16_t net_socket_poll(struct file *f);
