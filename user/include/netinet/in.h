#pragma once

/* Internet addresses (IPv4). Ports and addresses in a sockaddr_in are in network byte order. */

#include <stdint.h>
#include <sys/socket.h>

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    sa_family_t sin_family;      /* AF_INET */
    in_port_t sin_port;
    struct in_addr sin_addr;
    uint8_t sin_zero[8];
};

#define IPPROTO_IP   OS_IPPROTO_IP
#define IPPROTO_ICMP OS_IPPROTO_ICMP
#define IPPROTO_TCP  OS_IPPROTO_TCP
#define IPPROTO_UDP  OS_IPPROTO_UDP

#define IP_TOS OS_IP_TOS
#define IP_TTL OS_IP_TTL

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xFFFFFFFF)
#define INADDR_LOOPBACK  ((in_addr_t)0x7F000001)   /* (host byte order, like everywhere) */
#define INADDR_NONE      ((in_addr_t)0xFFFFFFFF)

#define INET_ADDRSTRLEN 16

static inline uint16_t htons(uint16_t x) { return (uint16_t)(x << 8 | x >> 8); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint32_t ntohl(uint32_t x) { return __builtin_bswap32(x); }
