/*
 * libhttp: an HTTP/1.1 client for <http.h>.
 *
 * One request per connection ("Connection: close"), so the end of the body is the end of the
 * connection unless the server gives a Content-Length or sends it chunked. Responses come back as
 * they are sent (Accept-Encoding: identity): no compression to undo.
 */

#include <http.h>

#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <tls.h>
#include <unistd.h>

#define MAX_REDIRECTS 10
#define HEAD_MAX      16384   /* the status line and headers */
#define TIMEOUT_S     30
#define USER_AGENT    "viralOS-http/1.0"

struct http {
    int fd;                  /* http:// */
    struct tls *tls;         /* https:// */
    /* What was read past the headers, then refilled as the body is read. */
    char buf[8192];
    size_t pos, end;
    /* The body's framing. */
    bool chunked, eof;
    int64_t left;            /* bytes left: of the body (Content-Length), or of this chunk; -1 = to the close */
};

struct url {
    bool https;
    char host[256];
    uint16_t port;
    char path[HTTP_URL_MAX];
};

/* --- URLs ---------------------------------------------------------------------------------------- */

static int parse_url(const char *s, struct url *u, char *err, size_t err_size) {
    memset(u, 0, sizeof(*u));
    if (strncmp(s, "https://", 8) == 0) {
        u->https = true;
        s += 8;
    } else if (strncmp(s, "http://", 7) == 0) {
        s += 7;
    } else if (strstr(s, "://")) {
        snprintf(err, err_size, "%s: only http:// and https:// addresses are supported", s);
        return -EINVAL;
    }
    u->port = u->https ? 443 : 80;
    /* host[:port], then the path (with the query) */
    size_t n = 0;
    while (s[n] && s[n] != '/' && s[n] != '?' && s[n] != '#') {
        n++;
    }
    const char *at = memchr(s, '@', n);   /* (user:password@ isn't supported; skip it) */
    if (at) {
        n -= (size_t)(at + 1 - s);
        s = at + 1;
    }
    const char *colon = memchr(s, ':', n);
    size_t host_len = colon ? (size_t)(colon - s) : n;
    if (host_len == 0 || host_len >= sizeof(u->host)) {
        snprintf(err, err_size, "not a web address: no host name");
        return -EINVAL;
    }
    memcpy(u->host, s, host_len);
    if (colon) {
        long port = strtol(colon + 1, NULL, 10);
        if (port <= 0 || port > 65535) {
            snprintf(err, err_size, "not a web address: bad port");
            return -EINVAL;
        }
        u->port = (uint16_t)port;
    }
    s += n;
    if (*s != '/') {
        u->path[0] = '/';
    }
    size_t plen = strlen(u->path);
    for (; *s && *s != '#' && plen + 4 < sizeof(u->path); s++) {
        /* (spaces aren't allowed in a request line) */
        if (*s == ' ') {
            memcpy(u->path + plen, "%20", 3);
            plen += 3;
        } else {
            u->path[plen++] = *s;
        }
    }
    u->path[plen] = 0;
    return 0;
}

static void url_text(const struct url *u, char *out, size_t size) {
    bool default_port = u->port == (u->https ? 443 : 80);
    char port[8] = "";
    if (!default_port) {
        snprintf(port, sizeof(port), ":%u", u->port);
    }
    snprintf(out, size, "%s://%s%s%s", u->https ? "https" : "http", u->host, port, u->path);
}

/* A Location header, relative or not, against the URL it came from. */
static void resolve_location(const struct url *base, const char *loc, char *out, size_t size) {
    if (strstr(loc, "://")) {
        strlcpy(out, loc, size);
    } else if (loc[0] == '/' && loc[1] == '/') {
        snprintf(out, size, "%s:%s", base->https ? "https" : "http", loc);
    } else if (loc[0] == '/') {
        struct url u = *base;
        strlcpy(u.path, loc, sizeof(u.path));
        url_text(&u, out, size);
    } else {
        struct url u = *base;
        char *slash = strrchr(u.path, '/');
        if (slash) {
            slash[1] = 0;
        }
        strlcat(u.path, loc, sizeof(u.path));
        url_text(&u, out, size);
    }
}

/* --- the connection ---------------------------------------------------------------------------- */

static long raw_read(struct http *h, void *buf, size_t len) {
    return h->tls ? tls_read(h->tls, buf, len) : recv(h->fd, buf, len, 0);
}

static long raw_write(struct http *h, const void *buf, size_t len) {
    if (h->tls) {
        return tls_write(h->tls, buf, len);
    }
    size_t done = 0;
    while (done < len) {
        long n = send(h->fd, (const char *)buf + done, len - done, 0);
        if (n < 0) {
            return n;
        }
        done += (size_t)n;
    }
    return (long)done;
}

static int open_plain(const struct url *u, char *err, size_t err_size) {
    struct in_addr addr;
    int r = resolve_host(u->host, &addr);
    if (r < 0) {
        snprintf(err, err_size, "%s: %s", u->host,
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
    sa.sin_port = htons(u->port);
    sa.sin_addr = addr;
    r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (r < 0) {
        snprintf(err, err_size, "%s port %u: %s", u->host, u->port, strerror(r));
        close(fd);
        return r;
    }
    return fd;
}

static void disconnect(struct http *h) {
    if (h->tls) {
        tls_close(h->tls);
        h->tls = NULL;
    }
    if (h->fd >= 0) {
        close(h->fd);
        h->fd = -1;
    }
}

/* --- the response's head ------------------------------------------------------------------------- */

static bool header_is(const char *line, const char *name, const char **value) {
    size_t n = strlen(name);
    for (size_t i = 0; i < n; i++) {
        char a = line[i], b = name[i];
        a = a >= 'A' && a <= 'Z' ? (char)(a + 32) : a;
        if (a != b) {
            return false;
        }
    }
    if (line[n] != ':') {
        return false;
    }
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    *value = v;
    return true;
}

static bool contains_word(const char *s, const char *word) {
    size_t n = strlen(word);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && (s[i] | 32) == word[i]) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

/* filename="report.pdf" (or filename=report.pdf) from a Content-Disposition. */
static void disposition_name(const char *v, char *out, size_t size) {
    const char *f = strstr(v, "filename=");
    if (!f) {
        return;
    }
    f += 9;
    bool quoted = *f == '"';
    f += quoted;
    size_t n = 0;
    while (f[n] && (quoted ? f[n] != '"' : f[n] != ';' && f[n] != ' ') && n + 1 < size) {
        n++;
    }
    memcpy(out, f, n);
    out[n] = 0;
    /* (only the name: never a path) */
    char *slash = strrchr(out, '/');
    if (slash) {
        memmove(out, slash + 1, strlen(slash + 1) + 1);
    }
}

/* Read up to the blank line; parse the status and the headers we use. */
static int read_head(struct http *h, struct http_response *res, char *location, size_t loc_size,
                     char *err, size_t err_size) {
    char *head = malloc(HEAD_MAX + 1);
    if (!head) {
        snprintf(err, err_size, "out of memory");
        return -ENOMEM;
    }
    size_t len = 0;
    char *body = NULL;
    while (!body) {
        if (len == HEAD_MAX) {
            snprintf(err, err_size, "the server's response headers are too long");
            free(head);
            return -EMSGSIZE;
        }
        long n = raw_read(h, head + len, HEAD_MAX - len);
        if (n <= 0) {
            char why[120];
            if (n == 0) {
                strlcpy(why, "the server closed the connection", sizeof(why));
            } else {
                http_error_text(h, n, why, sizeof(why));
            }
            snprintf(err, err_size, "no response: %s", why);
            free(head);
            return n == 0 ? -ECONNRESET : (int)n;
        }
        len += (size_t)n;
        head[len] = 0;
        body = strstr(head, "\r\n\r\n");
    }
    /* What came after the head is the start of the body. */
    size_t head_len = (size_t)(body + 4 - head);
    h->end = len - head_len;
    memcpy(h->buf, head + head_len, h->end);
    h->pos = 0;
    body[2] = 0;

    char *line = head, *next;
    if (strncmp(line, "HTTP/1.", 7) != 0) {
        snprintf(err, err_size, "that's not a web server's answer");
        free(head);
        return -EINVAL;
    }
    res->status = atoi(line + 9);
    next = strstr(line, "\r\n");
    *next = 0;
    strlcpy(res->reason, line + 9 + (strlen(line) > 12 ? 4 : 3), sizeof(res->reason));
    line = next + 2;

    res->length = -1;
    res->type[0] = res->filename[0] = 0;
    location[0] = 0;
    bool chunked = false;
    while (*line) {
        next = strstr(line, "\r\n");
        if (next) {
            *next = 0;
        }
        const char *v;
        if (header_is(line, "content-length", &v)) {
            res->length = 0;
            for (; *v >= '0' && *v <= '9'; v++) {
                res->length = res->length * 10 + (*v - '0');
            }
        } else if (header_is(line, "transfer-encoding", &v)) {
            chunked = contains_word(v, "chunked");
        } else if (header_is(line, "content-type", &v)) {
            strlcpy(res->type, v, sizeof(res->type));
        } else if (header_is(line, "location", &v)) {
            strlcpy(location, v, loc_size);
        } else if (header_is(line, "content-disposition", &v)) {
            disposition_name(v, res->filename, sizeof(res->filename));
        }
        if (!next) {
            break;
        }
        line = next + 2;
    }
    free(head);

    h->chunked = chunked;
    h->eof = false;
    if (chunked) {
        h->left = 0;                  /* (no chunk yet) */
        res->length = -1;
    } else {
        h->left = res->length;        /* -1: until the server closes */
    }
    if (res->status == 204 || res->status == 304 || (res->status >= 100 && res->status < 200)) {
        h->eof = true;
        res->length = 0;
    }
    return 0;
}

/* --- the request --------------------------------------------------------------------------------- */

static int request(struct http *h, const struct url *u, char *err, size_t err_size) {
    if (u->https) {
        h->tls = tls_connect(u->host, u->port, err, err_size);
        if (!h->tls) {
            return -ECONNREFUSED;
        }
    } else {
        h->fd = open_plain(u, err, err_size);
        if (h->fd < 0) {
            return h->fd;
        }
    }
    char host[300];
    if (u->port == (u->https ? 443 : 80)) {
        snprintf(host, sizeof(host), "%s", u->host);
    } else {
        snprintf(host, sizeof(host), "%s:%u", u->host, u->port);
    }
    char *req = malloc(HTTP_URL_MAX + 512);
    if (!req) {
        snprintf(err, err_size, "out of memory");
        return -ENOMEM;
    }
    int n = snprintf(req, HTTP_URL_MAX + 512,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: " USER_AGENT "\r\n"
                     "Accept: */*\r\n"
                     "Accept-Encoding: identity\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     u->path, host);
    long r = raw_write(h, req, (size_t)n);
    free(req);
    if (r < 0) {
        char why[120];
        http_error_text(h, r, why, sizeof(why));
        snprintf(err, err_size, "sending the request: %s", why);
        return (int)r;
    }
    return 0;
}

struct http *http_get(const char *url_text_in, struct http_response *res, char *err, size_t err_size) {
    struct http *h = calloc(1, sizeof(*h));
    char *current = malloc(HTTP_URL_MAX), *location = malloc(HTTP_URL_MAX);
    struct url *u = malloc(sizeof(*u));
    if (!h || !current || !location || !u) {
        snprintf(err, err_size, "out of memory");
        free(h), free(current), free(location), free(u);
        return NULL;
    }
    h->fd = -1;
    memset(res, 0, sizeof(*res));
    strlcpy(current, url_text_in, HTTP_URL_MAX);
    int r = 0;
    for (int hops = 0;; hops++) {
        r = parse_url(current, u, err, err_size);
        if (r < 0) {
            break;
        }
        url_text(u, res->url, sizeof(res->url));
        r = request(h, u, err, err_size);
        if (r < 0) {
            break;
        }
        r = read_head(h, res, location, HTTP_URL_MAX, err, err_size);
        if (r < 0) {
            break;
        }
        bool redirect = (res->status == 301 || res->status == 302 || res->status == 303 ||
                         res->status == 307 || res->status == 308) && location[0];
        if (!redirect) {
            if (h->tls) {
                snprintf(res->tls, sizeof(res->tls), "%s, %s", tls_version(h->tls), tls_cipher(h->tls));
            }
            break;
        }
        if (hops == MAX_REDIRECTS) {
            snprintf(err, err_size, "too many redirects (the last went to %s)", location);
            r = -EINVAL;
            break;
        }
        resolve_location(u, location, current, HTTP_URL_MAX);
        disconnect(h);
    }
    free(current), free(location), free(u);
    if (r < 0) {
        disconnect(h);
        free(h);
        return NULL;
    }
    return h;
}

/* --- the body ------------------------------------------------------------------------------------- */

/* Buffered bytes, refilling from the connection: up to len, 0 at its end. */
static long take(struct http *h, void *out, size_t len) {
    if (h->pos == h->end) {
        long n = raw_read(h, h->buf, sizeof(h->buf));
        if (n <= 0) {
            return n;
        }
        h->pos = 0;
        h->end = (size_t)n;
    }
    size_t n = h->end - h->pos < len ? h->end - h->pos : len;
    memcpy(out, h->buf + h->pos, n);
    h->pos += n;
    return (long)n;
}

/* One line (a chunk's size), without its CRLF. */
static int take_line(struct http *h, char *line, size_t size) {
    size_t n = 0;
    for (;;) {
        char c;
        long r = take(h, &c, 1);
        if (r <= 0) {
            return r == 0 ? -ECONNRESET : (int)r;
        }
        if (c == '\n') {
            break;
        }
        if (c != '\r' && n + 1 < size) {
            line[n++] = c;
        }
    }
    line[n] = 0;
    return 0;
}

long http_read(struct http *h, void *buf, size_t len) {
    if (h->eof || len == 0) {
        return 0;
    }
    if (h->chunked && h->left == 0) {
        char line[64];
        int r = take_line(h, line, sizeof(line));
        if (r < 0) {
            return r;
        }
        if (!line[0]) {   /* (the CRLF after the previous chunk) */
            r = take_line(h, line, sizeof(line));
            if (r < 0) {
                return r;
            }
        }
        int64_t size = 0;
        for (const char *p = line; *p; p++) {
            int d = *p >= '0' && *p <= '9' ? *p - '0' : (*p | 32) >= 'a' && (*p | 32) <= 'f' ? (*p | 32) - 'a' + 10 : -1;
            if (d < 0) {
                break;   /* (";extensions") */
            }
            size = size * 16 + d;
        }
        if (size == 0) {   /* the last chunk (trailers aren't read: the connection closes anyway) */
            h->eof = true;
            return 0;
        }
        h->left = size;
    }
    if (h->left == 0) {
        h->eof = true;
        return 0;
    }
    size_t want = h->left > 0 && (uint64_t)h->left < len ? (size_t)h->left : len;
    long n = take(h, buf, want);
    if (n == 0) {
        h->eof = true;
        /* A body that stops before its length is a broken download, not a short file. */
        return h->left > 0 ? -ECONNRESET : 0;
    }
    if (n > 0 && h->left > 0) {
        h->left -= n;
        if (h->left == 0 && !h->chunked) {
            h->eof = true;
        }
    }
    return n;
}

void http_close(struct http *h) {
    if (h) {
        disconnect(h);
        free(h);
    }
}

/* (Over https the errors are libtls's, whose numbers overlap the system's.) */
void http_error_text(struct http *h, long err, char *buf, size_t size) {
    if (err == -ECONNRESET) {
        snprintf(buf, size, "the connection was cut off");
    } else if (h && h->tls) {
        tls_error_text(err, buf, size);
    } else if (err == -EAGAIN || err == -ETIMEDOUT) {
        snprintf(buf, size, "the server stopped answering");
    } else {
        snprintf(buf, size, "%s", strerror((int)err));
    }
}
