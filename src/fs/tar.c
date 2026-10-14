/*
 * USTAR reader. An archive is a sequence of 512-byte blocks: a header block
 * per entry, followed by the file's data padded to a whole block; two
 * all-zero blocks mark the end. Numbers in headers are ASCII octal.
 */

#include <stdbool.h>

#include "fs/tar.h"
#include "fs/vfs.h"
#include "lib/errno.h"
#include "lib/string.h"

#define BLOCK 512

struct ustar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];       /* "ustar\0" (POSIX) or "ustar " (GNU) */
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];    /* long paths: prefix + "/" + name */
    char pad[12];
};

_Static_assert(sizeof(struct ustar_header) == BLOCK, "tar header must be one block");

static uint64_t parse_octal(const char *s, size_t len) {
    uint64_t v = 0;
    for (size_t i = 0; i < len && s[i] >= '0' && s[i] <= '7'; i++) {
        v = v * 8 + (uint64_t)(s[i] - '0');
    }
    return v;
}

/* The checksum is the sum of all header bytes, counting its own field as spaces. */
static bool checksum_ok(const struct ustar_header *h) {
    const uint8_t *p = (const uint8_t *)h;
    uint64_t sum = 0;
    for (size_t i = 0; i < BLOCK; i++) {
        bool in_field = i >= offsetof(struct ustar_header, checksum) &&
                        i < offsetof(struct ustar_header, checksum) + sizeof(h->checksum);
        sum += in_field ? ' ' : p[i];
    }
    return sum == parse_octal(h->checksum, sizeof(h->checksum));
}

static bool all_zero(const uint8_t *block) {
    for (size_t i = 0; i < BLOCK; i++) {
        if (block[i]) {
            return false;
        }
    }
    return true;
}

/* Copy a fixed-size, possibly unterminated header field. */
static size_t copy_field(char *dest, const char *src, size_t len) {
    size_t n = 0;
    while (n < len && src[n]) {
        dest[n] = src[n];
        n++;
    }
    dest[n] = '\0';
    return n;
}

/* Like `mkdir -p` for every directory above `path`. */
static int make_parents(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        int err = vfs_create(NULL, path, VNODE_DIR, 0755, NULL);
        *p = '/';
        if (err && err != -EEXIST) {
            return err;
        }
    }
    return 0;
}

static int extract(const struct ustar_header *h, const char *path, const uint8_t *data,
                   uint64_t size, struct tar_result *result) {
    char buf[VFS_PATH_MAX];
    strlcpy(buf, path, sizeof(buf));
    int err = make_parents(buf);
    if (err) {
        return err;
    }

    uint32_t mode = (uint32_t)parse_octal(h->mode, sizeof(h->mode)) & 07777;
    uint64_t mtime = parse_octal(h->mtime, sizeof(h->mtime));
    struct vnode *node;

    if (h->typeflag == '5') {
        err = vfs_create(NULL, path, VNODE_DIR, mode, &node);
        if (err == -EEXIST) {   /* already made as some file's parent */
            err = vfs_resolve(NULL, path, &node);
            if (!err) {
                node->mode = mode;
            }
        } else if (!err) {
            result->directories++;
        }
    } else {
        err = vfs_create(NULL, path, VNODE_FILE, mode, &node);
        if (!err) {
            int64_t written = vfs_write(node, 0, data, size);
            if (written < 0) {
                err = (int)written;
            }
            result->files++;
            result->bytes += size;
        }
    }
    if (!err) {
        node->mtime = node->ctime = mtime;
        vfs_put(node);
    }
    return err;
}

int tar_unpack(const void *archive, size_t size, struct tar_result *result) {
    const uint8_t *base = archive;
    *result = (struct tar_result){ 0 };

    for (size_t offset = 0; offset + BLOCK <= size;) {
        const struct ustar_header *h = (const struct ustar_header *)(base + offset);
        if (all_zero(base + offset)) {
            return 0;   /* end-of-archive marker */
        }
        if (memcmp(h->magic, "ustar", 5) != 0 || !checksum_ok(h)) {
            return -EINVAL;
        }

        uint64_t file_size = parse_octal(h->size, sizeof(h->size));
        const uint8_t *data = base + offset + BLOCK;
        if (file_size > size - offset - BLOCK) {
            return -EINVAL;   /* truncated archive */
        }

        /* Full path is "<prefix>/<name>"; strip "./" and leading slashes. */
        char path[1 + sizeof(h->prefix) + 1 + sizeof(h->name) + 1];
        size_t len = 1;
        path[0] = '/';
        if (h->prefix[0]) {
            len += copy_field(path + len, h->prefix, sizeof(h->prefix));
            path[len++] = '/';
        }
        copy_field(path + len, h->name, sizeof(h->name));

        char *p = path + 1;
        while (p[0] == '.' && p[1] == '/') {
            p += 2;
        }
        while (*p == '/') {
            p++;
        }
        memmove(path + 1, p, strlen(p) + 1);
        len = strlen(path);
        while (len > 1 && path[len - 1] == '/') {
            path[--len] = '\0';
        }
        if (len >= VFS_PATH_MAX) {
            return -ENAMETOOLONG;
        }

        bool is_root = strcmp(path, "/") == 0 || strcmp(path, "/.") == 0;
        bool supported = h->typeflag == '0' || h->typeflag == '\0' || h->typeflag == '5';
        if (!is_root) {
            if (supported) {
                int err = extract(h, path, data, file_size, result);
                if (err) {
                    return err;
                }
            } else {
                result->skipped++;
            }
        }

        offset += BLOCK + (file_size + BLOCK - 1) / BLOCK * BLOCK;
    }
    return 0;
}
