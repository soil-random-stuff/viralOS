#pragma once

/*
 * libhttp: fetching http:// and https:// URLs (HTTP/1.1 GET, through libtls for https). Redirects
 * are followed; the body comes out as plain bytes whether the server sent it with a length, in
 * chunks, or until it closed the connection.
 *
 *   struct http_response res;
 *   struct http *h = http_get("https://example.com/", &res, err, sizeof(err));
 *   while ((n = http_read(h, buf, sizeof(buf))) > 0) ...
 *   http_close(h);
 */

#include <stddef.h>
#include <stdint.h>

#define HTTP_URL_MAX 2048

struct http_response {
    int status;                 /* 200, 404... (after redirects) */
    char reason[64];            /* "OK", "Not Found" */
    int64_t length;             /* the body's size, -1 if the server didn't say */
    char type[128];             /* Content-Type, may be empty */
    char url[HTTP_URL_MAX];     /* where it came from in the end */
    char filename[256];         /* from Content-Disposition, may be empty */
    char tls[96];               /* "TLSv1.3, TLS1-3-AES-256-GCM-SHA384", empty for http:// */
};

struct http;

/* Connect, send the request, read the response's head. NULL on failure (why in `err`); otherwise
   the body is ready to read, whatever the status (check res->status). */
struct http *http_get(const char *url, struct http_response *res, char *err, size_t err_size);
/* Body bytes: up to len, 0 at the end, a negative error (http_error_text describes it). */
long http_read(struct http *h, void *buf, size_t len);
void http_close(struct http *h);
void http_error_text(struct http *h, long err, char *buf, size_t size);
