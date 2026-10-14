#pragma once

/* Dotted-decimal addresses <-> struct in_addr. */

#include <netinet/in.h>

/* "1.2.3.4" -> *out (network order); 1 if it was one, 0 if not. */
int inet_aton(const char *s, struct in_addr *out);
/* Same, returning INADDR_NONE for a bad one. */
in_addr_t inet_addr(const char *s);
/* -> "1.2.3.4" (in a buffer shared by every call). */
const char *inet_ntoa(struct in_addr a);
/* AF_INET only: inet_pton returns 1 on success; inet_ntop returns dst, or NULL if it doesn't fit. */
int inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
