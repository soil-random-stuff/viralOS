#pragma once

/*
 * libtls: encrypted connections (TLS 1.2 and 1.3, through mbedTLS). tls_connect() opens a TCP
 * connection, does the handshake and checks the server's certificate: signed by one of the roots in
 * /etc/ssl/certs/ca-certificates.crt, in date, and for that host name. Then read and write go
 * through it like a socket.
 */

#include <stddef.h>
#include <stdint.h>

#define TLS_CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"

struct tls;

/* NULL on failure, with why in `err` ("certificate expired", "connection refused"...). */
struct tls *tls_connect(const char *host, uint16_t port, char *err, size_t err_size);
/* Up to len bytes; 0 at the end (the server closed), or a negative error. */
long tls_read(struct tls *t, void *buf, size_t len);
/* All of buf, or a negative error. */
long tls_write(struct tls *t, const void *buf, size_t len);
void tls_close(struct tls *t);
/* "TLSv1.3", and "TLS1-3-AES-128-GCM-SHA256" or the like. */
const char *tls_version(struct tls *t);
const char *tls_cipher(struct tls *t);
/* Describe a negative error from tls_read/tls_write. */
void tls_error_text(long err, char *buf, size_t size);

/* SHA-256, for checking downloads: start, add the data in pieces, finish (which frees it and
   writes 64 hex digits and a NUL). NULL from start: out of memory. */
struct tls_sha256;
struct tls_sha256 *tls_sha256_start(void);
void tls_sha256_add(struct tls_sha256 *h, const void *data, size_t len);
void tls_sha256_finish(struct tls_sha256 *h, char out[65]);
