/*
 * wget [-O file] [-q] URL...
 *
 * Downloads http:// and https:// URLs (libhttp, libtls) into the current folder, under the name the
 * server suggests or the URL's last part ("index.html" for a bare site), adding .1, .2... rather
 * than overwriting. -O names the file (-O - writes to the terminal), -q prints only errors.
 * Ctrl+C stops and deletes the unfinished file.
 */

#include <http.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool quiet;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    if (quiet) {
        return;
    }
    static char buf[HTTP_URL_MAX + 256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(2, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}

static void size_text(int64_t bytes, char *out, size_t size) {
    if (bytes < 1024) {
        snprintf(out, size, "%d B", (int)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, size, "%d.%d KB", (int)(bytes / 1024), (int)(bytes % 1024 * 10 / 1024));
    } else if (bytes < 1024ll * 1024 * 1024) {
        snprintf(out, size, "%d.%d MB", (int)(bytes >> 20), (int)((bytes & 0xFFFFF) * 10 >> 20));
    } else {
        snprintf(out, size, "%d.%d GB", (int)(bytes >> 30), (int)((bytes & 0x3FFFFFFF) * 10 >> 30));
    }
}

/* --- the file name ----------------------------------------------------------------------------- */

static int hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

/* The URL's last part, %XX decoded, without the query: ".../files/My%20Song.mp3?x=1" -> "My Song.mp3". */
static void name_from_url(const char *url, char *out, size_t size) {
    const char *start = strstr(url, "://");
    start = start ? start + 3 : url;
    const char *path = strchr(start, '/');
    out[0] = 0;
    if (!path) {
        return;
    }
    const char *end = path;
    while (*end && *end != '?' && *end != '#') {
        end++;
    }
    const char *last = end;
    while (last > path && last[-1] != '/') {
        last--;
    }
    size_t n = 0;
    for (const char *p = last; p < end && n + 1 < size; p++) {
        if (*p == '%' && p + 2 < end && hex(p[1]) >= 0 && hex(p[2]) >= 0) {
            out[n++] = (char)(hex(p[1]) * 16 + hex(p[2]));
            p += 2;
        } else {
            out[n++] = *p;
        }
    }
    out[n] = 0;
    /* (never a path, never hidden by accident) */
    for (char *c = out; *c; c++) {
        if (*c == '/' || *c == '\\') {
            *c = '_';
        }
    }
    if (strcmp(out, ".") == 0 || strcmp(out, "..") == 0) {
        out[0] = 0;
    }
}

static bool exists(const char *path) {
    struct os_file_info info;
    return stat(path, &info) == 0;
}

/* name, or name.1, name.2... whichever is free. */
static void free_name(const char *name, char *out, size_t size) {
    strlcpy(out, name, size);
    for (int i = 1; exists(out) && i < 1000; i++) {
        snprintf(out, size, "%s.%d", name, i);
    }
}

/* --- progress ------------------------------------------------------------------------------------ */

struct progress {
    int64_t total, got;
    uint64_t start, shown;
};

static void show(struct progress *p, bool last) {
    uint64_t now = uptime_ms();
    if (quiet || (!last && now - p->shown < 200)) {
        return;
    }
    p->shown = now;
    uint64_t ms = now - p->start ? now - p->start : 1;
    int64_t rate = p->got * 1000 / (int64_t)ms;
    char got[24], speed[24], line[160];
    size_text(p->got, got, sizeof(got));
    size_text(rate, speed, sizeof(speed));
    if (p->total > 0) {
        int pct = (int)(p->got * 100 / p->total);
        char bar[31];
        int fill = pct * 30 / 100;
        for (int i = 0; i < 30; i++) {
            bar[i] = i < fill ? '=' : i == fill ? '>' : ' ';
        }
        bar[30] = 0;
        char eta[24] = "";
        if (!last && rate > 0) {
            int64_t s = (p->total - p->got) / rate;
            snprintf(eta, sizeof(eta), "  eta %ds", (int)s);
        }
        snprintf(line, sizeof(line), "\r%3d%% [%s] %s  %s/s%s\x1b[K", pct, bar, got, speed, eta);
    } else {
        snprintf(line, sizeof(line), "\r%s  %s/s\x1b[K", got, speed);
    }
    write(2, line, strlen(line));
}

/* --- one download -------------------------------------------------------------------------------- */

static int download(const char *url, const char *out_name) {
    char err[300];
    struct http_response *res = malloc(sizeof(*res));
    if (!res) {
        printf("wget: out of memory\n");
        return 1;
    }
    say("%s\n", url);
    struct http *h = http_get(url, res, err, sizeof(err));
    if (!h) {
        printf("wget: %s\n", err);
        free(res);
        return 4;
    }
    if (strcmp(res->url, url) != 0) {
        /* (download sites redirect to signed links a screen long: then the start is enough) */
        if (strlen(res->url) > 120) {
            say("Redirected to %.100s...\n", res->url);
        } else {
            say("Redirected to %s\n", res->url);
        }
    }
    if (res->tls[0]) {
        say("Secure connection: %s\n", res->tls);
    }
    char total[24] = "length unknown";
    if (res->length >= 0) {
        size_text(res->length, total, sizeof(total));
    }
    say("%d %s, %s%s%s%s\n", res->status, res->reason, total, res->type[0] ? " (" : "", res->type,
        res->type[0] ? ")" : "");
    if (res->status < 200 || res->status >= 300) {
        printf("wget: the server answered %d %s; nothing saved\n", res->status, res->reason);
        http_close(h);
        free(res);
        return 8;
    }

    /* Where to. */
    bool to_stdout = out_name && strcmp(out_name, "-") == 0;
    char path[OS_PATH_MAX];
    if (out_name) {
        strlcpy(path, out_name, sizeof(path));
    } else {
        char name[256];
        strlcpy(name, res->filename, sizeof(name));
        if (!name[0]) {
            name_from_url(res->url, name, sizeof(name));
        }
        if (!name[0]) {
            strlcpy(name, "index.html", sizeof(name));
        }
        free_name(name, path, sizeof(path));
    }
    int fd = 1;
    if (!to_stdout) {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) {
            printf("wget: %s: %s\n", path, strerror(fd));
            http_close(h);
            free(res);
            return 3;
        }
        say("Saving to '%s'\n", path);
    }

    struct progress p = { res->length, 0, uptime_ms(), 0 };
    static char buf[32768];
    int status = 0;
    for (;;) {
        if (interrupt_mode(OS_INTERRUPT_CHECK)) {
            say("\n");
            printf("wget: stopped\n");
            status = 1;
            break;
        }
        long n = http_read(h, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            http_error_text(h, n, err, sizeof(err));
            say("\n");
            printf("wget: the download broke off: %s\n", err);
            status = 4;
            break;
        }
        for (long done = 0; done < n;) {
            long w = write(fd, buf + done, (size_t)(n - done));
            if (w <= 0) {
                say("\n");
                printf("wget: %s: %s\n", to_stdout ? "output" : path, strerror(w < 0 ? (int)w : -EIO));
                status = 3;
                break;
            }
            done += w;
        }
        if (status) {
            break;
        }
        p.got += n;
        if (!to_stdout) {
            show(&p, false);
        }
    }
    http_close(h);
    free(res);
    if (to_stdout) {
        return status;
    }
    close(fd);
    if (status) {
        unlink(path);   /* (half a file is worse than none) */
        return status;
    }
    show(&p, true);
    uint64_t ms = uptime_ms() - p.start;
    char got[24], speed[24];
    size_text(p.got, got, sizeof(got));
    size_text(p.got * 1000 / (int64_t)(ms ? ms : 1), speed, sizeof(speed));
    say("\n'%s' saved (%s in %d.%d s, %s/s)\n", path, got, (int)(ms / 1000), (int)(ms % 1000 / 100), speed);
    return 0;
}

int main(int argc, char **argv) {
    const char *out_name = NULL;
    const char *urls[32];
    int count = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-O") == 0 && i + 1 < argc) {
            out_name = argv[++i];
        } else if (strcmp(argv[i], "-q") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            count = 0;
            break;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            printf("wget: unknown option %s\n", argv[i]);
            return 2;
        } else if (count < 32) {
            urls[count++] = argv[i];
        }
    }
    if (count == 0) {
        printf("usage: wget [-O file] [-q] URL...\n"
               "  downloads http:// and https:// addresses into this folder\n"
               "  -O file   save as this name (- for the terminal)\n"
               "  -q        quiet: errors only\n");
        return 2;
    }
    if (out_name && count > 1 && strcmp(out_name, "-") != 0) {
        printf("wget: -O with several addresses would overwrite one with the next\n");
        return 2;
    }
    interrupt_mode(OS_INTERRUPT_CATCH);
    int status = 0;
    for (int i = 0; i < count; i++) {
        int s = download(urls[i], out_name);
        status = s ? s : status;
        if (s == 1) {
            break;   /* Ctrl+C: stop everything */
        }
    }
    return status;
}
