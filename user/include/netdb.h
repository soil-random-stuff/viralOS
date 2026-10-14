#pragma once

/*
 * Looking up host names (DNS, through the kernel; "localhost" and dotted addresses work offline).
 * IPv4 only.
 */

#include <netinet/in.h>

/* A host name or "1.2.3.4" -> its address. 0, or -ENOENT (no such host), -ENETUNREACH (no DNS
   server: not connected), -EHOSTUNREACH (the DNS servers didn't answer). */
int resolve_host(const char *name, struct in_addr *out);

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE     0x01
#define AI_CANONNAME   0x02
#define AI_NUMERICHOST 0x04
#define AI_NUMERICSERV 0x400

#define EAI_NONAME  (-2)
#define EAI_AGAIN   (-3)
#define EAI_FAIL    (-4)
#define EAI_FAMILY  (-6)
#define EAI_SERVICE (-8)
#define EAI_MEMORY  (-10)

/* The usual one, for one IPv4 address (and a numeric port). */
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int err);
