#pragma once

/*
 * lwIP's configuration for the kernel (the full list with defaults: lwip/opt.h).
 *
 * lwIP runs as an operating system port (NO_SYS = 0): its own "tcpip" thread does the protocol work,
 * and programs' socket calls (net/socket.c -> lwIP's sockets API) talk to it, blocking on semaphores
 * and mailboxes (net/port/sys_arch.c). Memory comes from the kernel heap.
 */

#define NO_SYS                    0
#define SYS_LIGHTWEIGHT_PROT      1
#define LWIP_COMPAT_MUTEX         1   /* mutexes are semaphores of one */
#define LWIP_COMPAT_MUTEX_ALLOWED     /* (no thread priorities here, so no priority inversion) */
#define LWIP_TCPIP_CORE_LOCKING   1

/* Memory: everything from kmalloc. */
#define MEM_CUSTOM_ALLOCATOR      1
#define MEM_CUSTOM_MALLOC         kmalloc
#define MEM_CUSTOM_FREE           kfree
#define MEM_CUSTOM_CALLOC         net_calloc
#define MEMP_MEM_MALLOC           1
#define MEM_ALIGNMENT             8
#include <stddef.h>
void *kmalloc(size_t size);
void kfree(void *ptr);
void *net_calloc(size_t count, size_t size);

/* Protocols */
#define LWIP_IPV4                 1
#define LWIP_IPV6                 0
#define LWIP_ARP                  1
#define LWIP_ETHERNET             1
#define LWIP_ICMP                 1
#define LWIP_RAW                  1
#define LWIP_UDP                  1
#define LWIP_TCP                  1
#define LWIP_DHCP                 1
#define LWIP_DHCP_DOES_ACD_CHECK  0   /* (QEMU's and most home routers' leases are fine: no 2 s wait) */
#define LWIP_AUTOIP               0
#define LWIP_IGMP                 0
#define LWIP_DNS                  1
#define DNS_MAX_SERVERS           2
#define DNS_LOCAL_HOSTLIST        1
#define DNS_LOCAL_HOSTLIST_INIT   { DNS_LOCAL_HOSTLIST_ELEM("localhost", IPADDR4_INIT_BYTES(127, 0, 0, 1)) }
#define IP_REASSEMBLY             1
#define IP_FRAG                   1
#define ARP_TABLE_SIZE            32
#define ARP_QUEUEING              1
#define ETHARP_SUPPORT_STATIC_ENTRIES 0

/* TCP: windows sized for a browser's downloads. */
#define TCP_MSS                   1460
#define LWIP_WND_SCALE            1
#define TCP_RCV_SCALE             2
#define TCP_WND                   (128 * 1024)
#define TCP_SND_BUF               (64 * 1024)
#define TCP_SND_QUEUELEN          (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_QUEUE_OOSEQ           1
#define LWIP_TCP_KEEPALIVE        1
#define LWIP_TCP_SACK_OUT         1
#define TCP_LISTEN_BACKLOG        1
#define LWIP_TCP_TIMESTAMPS       0

/* Pools (sizes only cap how many: the memory itself is kmalloc'd). */
#define MEMP_NUM_NETCONN          128
#define MEMP_NUM_TCP_PCB          128
#define MEMP_NUM_TCP_PCB_LISTEN   32
#define MEMP_NUM_UDP_PCB          64
#define MEMP_NUM_RAW_PCB          32
#define MEMP_NUM_TCP_SEG          (TCP_SND_QUEUELEN * 4)
#define MEMP_NUM_NETBUF           256
#define MEMP_NUM_PBUF             256
#define MEMP_NUM_SYS_TIMEOUT      (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)
#define MEMP_NUM_TCPIP_MSG_INPKT  256
#define MEMP_NUM_TCPIP_MSG_API    64
#define PBUF_POOL_SIZE            256
#define PBUF_POOL_BUFSIZE         1600

/* Threads and mailboxes */
#define TCPIP_THREAD_NAME         "tcpip"
#define TCPIP_THREAD_STACKSIZE    0
#define TCPIP_THREAD_PRIO         0
#define TCPIP_MBOX_SIZE           256
#define DEFAULT_THREAD_STACKSIZE  0
#define DEFAULT_RAW_RECVMBOX_SIZE 64
#define DEFAULT_UDP_RECVMBOX_SIZE 128
#define DEFAULT_TCP_RECVMBOX_SIZE 256
#define DEFAULT_ACCEPTMBOX_SIZE   32

/* The sockets API (for net/socket.c; programs see the system's own calls, not these names). */
#define LWIP_NETCONN              1
#define LWIP_SOCKET               1
#define LWIP_COMPAT_SOCKETS       0
#define LWIP_POSIX_SOCKETS_IO_NAMES 0
#define LWIP_SOCKET_OFFSET        0
#define LWIP_SOCKET_SELECT        1
#define LWIP_SOCKET_POLL          1
#define LWIP_SO_RCVTIMEO          1
#define LWIP_SO_SNDTIMEO          1
#define LWIP_SO_RCVBUF            1
#define LWIP_SO_LINGER            0
#define SO_REUSE                  1
#define LWIP_NETCONN_FULLDUPLEX   0
#define LWIP_NETCONN_SEM_PER_THREAD 0
#define LWIP_DNS_API_DECLARE_H_ERRNO 1
#define LWIP_DNS_API_DEFINE_ERRORS 1
#define LWIP_DNS_API_DECLARE_STRUCTS 1
#define LWIP_TIMEVAL_PRIVATE      1
#define LWIP_FIONREAD_LINUXMODE   1
/* Readiness changes wake programs in poll() (a hook added to lwIP's sockets.c). */
void net_socket_event(void);
#define LWIP_HOOK_SOCKET_EVENT()  net_socket_event()

/* Network interfaces */
#define LWIP_NETIF_API            1
#define LWIP_NETIF_HOSTNAME       1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK  1
#define LWIP_NETIF_LOOPBACK       1
#define LWIP_HAVE_LOOPIF          1
#define LWIP_LOOPBACK_MAX_PBUFS   64

/* Checksums: in software (no driver offloads them yet). */
#define CHECKSUM_GEN_IP           1
#define CHECKSUM_GEN_UDP          1
#define CHECKSUM_GEN_TCP          1
#define CHECKSUM_GEN_ICMP         1
#define CHECKSUM_CHECK_IP         1
#define CHECKSUM_CHECK_UDP        1
#define CHECKSUM_CHECK_TCP        1
#define CHECKSUM_CHECK_ICMP       1
#define LWIP_CHECKSUM_ON_COPY     1

#define LWIP_STATS                0
#define LWIP_DEBUG                0
