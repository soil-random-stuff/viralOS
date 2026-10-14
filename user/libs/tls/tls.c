/*
 * libtls: <tls.h> on mbedTLS (vendored in mbedtls/, configured by tls_config.h).
 *
 * Shared by every connection of a program: the random generator (CTR-DRBG fed by the kernel's
 * getrandom) and the trusted roots, read from the CA bundle the first time they're needed.
 * Each connection is a socket plus an mbedTLS context that reads and writes it through callbacks.
 */

#include <tls.h>

#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#define TIMEOUT_S 30   /* a server that says nothing for this long is gone */

struct tls {
    int fd;
    mbedtls_ssl_context ssl;
};

static bool ready;
static int ready_error;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt roots;
static mbedtls_ssl_config config;

/* --- what tls_config.h asks of us --------------------------------------------------------------- */

int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen) {
    (void)data;
    int r = getrandom(out, len);
    if (r < 0) {
        *olen = 0;
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
    *olen = len;
    return 0;
}

mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)uptime_ms();
}

/* (The clock holds local time, not UTC: certificate dates are off by the time zone, which only
   matters on the day one starts or ends.) */
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *t, struct tm *out) {
    time_t v = (time_t)*t;
    return localtime_r(&v, out);
}

void mbedtls_platform_zeroize(void *buf, size_t len) {
    volatile unsigned char *p = buf;
    while (len--) {
        *p++ = 0;
    }
}

/* --- set-up (once) ------------------------------------------------------------------------------- */

static void describe(int err, char *buf, size_t size) {
    mbedtls_strerror(err, buf, size);
}

/* The roots: the whole PEM bundle, NUL-terminated as mbedTLS wants. */
static int load_roots(void) {
    struct os_file_info info;
    int r = stat(TLS_CA_BUNDLE, &info);
    if (r < 0) {
        return r;
    }
    char *pem = malloc(info.size + 1);
    if (!pem) {
        return -ENOMEM;
    }
    int fd = open(TLS_CA_BUNDLE, O_RDONLY);
    if (fd < 0) {
        free(pem);
        return fd;
    }
    size_t got = 0;
    while (got < info.size) {
        long n = read(fd, pem + got, info.size - got);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    close(fd);
    pem[got] = 0;
    r = mbedtls_x509_crt_parse(&roots, (const unsigned char *)pem, got + 1);
    free(pem);
    /* (> 0: that many certificates in the bundle couldn't be read; the rest are in) */
    return r < 0 && !roots.raw.p ? -EINVAL : 0;
}

static int setup(char *err, size_t err_size) {
    if (ready) {
        return 0;
    }
    if (ready_error) {
        snprintf(err, err_size, "TLS isn't available: %s", strerror(ready_error));
        return ready_error;
    }
    int r = (int)psa_crypto_init();
    if (r != PSA_SUCCESS) {
        snprintf(err, err_size, "couldn't start the cryptography library (PSA error %d)", r);
        return ready_error = -EIO;
    }
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    static const char personal[] = "viralOS libtls";
    r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)personal,
                              sizeof(personal) - 1);
    if (r) {
        char why[100];
        describe(r, why, sizeof(why));
        snprintf(err, err_size, "no random numbers: %s", why);
        return ready_error = -EIO;
    }
    mbedtls_x509_crt_init(&roots);
    r = load_roots();
    if (r < 0) {
        snprintf(err, err_size, "couldn't read the trusted certificates (%s): %s", TLS_CA_BUNDLE,
                 strerror(r));
        return ready_error = r;
    }
    mbedtls_ssl_config_init(&config);
    r = mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (r) {
        char why[100];
        describe(r, why, sizeof(why));
        snprintf(err, err_size, "TLS set-up failed: %s", why);
        return ready_error = -EIO;
    }
    mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&config, &roots, NULL);
    mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &drbg);
    ready = true;
    return 0;
}

/* --- the socket under it --------------------------------------------------------------------------- */

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    struct tls *t = ctx;
    long n = send(t->fd, buf, len, 0);
    if (n >= 0) {
        return (int)n;
    }
    if (n == -EAGAIN) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return n == -ECONNRESET || n == -EPIPE ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    struct tls *t = ctx;
    long n = recv(t->fd, buf, len, 0);
    if (n >= 0) {
        return (int)n;
    }
    if (n == -EAGAIN) {
        return MBEDTLS_ERR_SSL_TIMEOUT;   /* (SO_RCVTIMEO ran out) */
    }
    return n == -ECONNRESET ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_RECV_FAILED;
}

static int open_socket(const char *host, uint16_t port, char *err, size_t err_size) {
    struct in_addr addr;
    int r = resolve_host(host, &addr);
    if (r < 0) {
        snprintf(err, err_size, "%s: %s", host,
                 r == -ENOENT ? "no such host" : r == -ENETUNREACH ? "not connected to a network" : strerror(r));
        return r;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, err_size, "socket: %s", strerror(fd));
        return fd;
    }
    struct timeval tv = { TIMEOUT_S, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr = addr;
    r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (r < 0) {
        snprintf(err, err_size, "%s port %u: %s", host, port, strerror(r));
        close(fd);
        return r;
    }
    return fd;
}

/* --- connections ------------------------------------------------------------------------------- */

struct tls *tls_connect(const char *host, uint16_t port, char *err, size_t err_size) {
    if (setup(err, err_size) < 0) {
        return NULL;
    }
    struct tls *t = calloc(1, sizeof(*t));
    if (!t) {
        snprintf(err, err_size, "out of memory");
        return NULL;
    }
    t->fd = open_socket(host, port, err, err_size);
    if (t->fd < 0) {
        free(t);
        return NULL;
    }
    mbedtls_ssl_init(&t->ssl);
    int r = mbedtls_ssl_setup(&t->ssl, &config);
    if (!r) {
        r = mbedtls_ssl_set_hostname(&t->ssl, host);   /* the name sent (SNI) and checked */
    }
    if (!r) {
        mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
        do {
            r = mbedtls_ssl_handshake(&t->ssl);
        } while (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE);
    }
    if (r) {
        uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
        if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags) {
            char info[256];
            mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
            /* (one reason per line; the first is the most useful) */
            char *nl = strchr(info, '\n');
            if (nl) {
                *nl = 0;
            }
            snprintf(err, err_size, "%s: the certificate isn't trusted: %s", host, info);
        } else {
            char why[160];
            describe(r, why, sizeof(why));
            snprintf(err, err_size, "%s: secure connection failed: %s", host, why);
        }
        mbedtls_ssl_free(&t->ssl);
        close(t->fd);
        free(t);
        return NULL;
    }
    return t;
}

long tls_read(struct tls *t, void *buf, size_t len) {
    for (;;) {
        int r = mbedtls_ssl_read(&t->ssl, buf, len);
        if (r >= 0) {
            return r;
        }
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return 0;
        }
        /* (TLS 1.3 servers send session tickets after the handshake: nothing to read yet) */
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE ||
            r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }
        return r;
    }
}

long tls_write(struct tls *t, const void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        int r = mbedtls_ssl_write(&t->ssl, (const unsigned char *)buf + done, len - done);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (r < 0) {
            return r;
        }
        done += (size_t)r;
    }
    return (long)done;
}

void tls_close(struct tls *t) {
    if (!t) {
        return;
    }
    mbedtls_ssl_close_notify(&t->ssl);
    mbedtls_ssl_free(&t->ssl);
    close(t->fd);
    free(t);
}

const char *tls_version(struct tls *t) {
    return mbedtls_ssl_get_version(&t->ssl);
}

const char *tls_cipher(struct tls *t) {
    return mbedtls_ssl_get_ciphersuite(&t->ssl);
}

void tls_error_text(long err, char *buf, size_t size) {
    if (err == MBEDTLS_ERR_SSL_TIMEOUT) {
        snprintf(buf, size, "the server stopped answering");
    } else if (err == MBEDTLS_ERR_NET_CONN_RESET) {
        snprintf(buf, size, "the connection was reset");
    } else {
        describe((int)err, buf, size);
    }
}

/* --- SHA-256 ------------------------------------------------------------------------------------- */

struct tls_sha256 {
    mbedtls_sha256_context c;
};

struct tls_sha256 *tls_sha256_start(void) {
    struct tls_sha256 *h = malloc(sizeof(*h));
    if (h) {
        mbedtls_sha256_init(&h->c);
        mbedtls_sha256_starts(&h->c, 0);
    }
    return h;
}

void tls_sha256_add(struct tls_sha256 *h, const void *data, size_t len) {
    mbedtls_sha256_update(&h->c, data, len);
}

void tls_sha256_finish(struct tls_sha256 *h, char out[65]) {
    unsigned char sum[32];
    mbedtls_sha256_finish(&h->c, sum);
    mbedtls_sha256_free(&h->c);
    free(h);
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = digits[sum[i] >> 4];
        out[i * 2 + 1] = digits[sum[i] & 15];
    }
    out[64] = 0;
}
