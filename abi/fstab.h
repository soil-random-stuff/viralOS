#pragma once

/*
 * /etc/fstab lines, read by the kernel (fs/mount.c: where a drive goes when it's plugged in) and
 * read and written by the Disks app (Edit Mount Options). The usual format, one filesystem per line:
 *
 *   UUID=<uuid>  <mount point>  <type>  <options>  0  0
 *
 * The type is "auto" (the kernel finds out). Options, comma-separated: "noauto" (don't mount it
 * when it's plugged in), "rw" (NTFS and ext4: read-write, served by a program; FAT and exFAT are
 * always read-write); otherwise "defaults". Spaces and tabs in the mount point are written \040 and
 * \011, a backslash \134. Lines starting with # are comments.
 *
 * No library calls: the kernel and programs both include this.
 */

#include <stdbool.h>
#include <stddef.h>

struct fstab_entry {
    const char *uuid;            /* into the line */
    char path[1024];             /* the mount point, unescaped */
    bool noauto, rw;
};

static inline bool fstab_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

/* The next whitespace-separated field of *p (NUL-terminated in place), or NULL. */
static inline char *fstab_field(char **p) {
    char *s = *p;
    while (fstab_space(*s)) {
        s++;
    }
    if (!*s) {
        return NULL;
    }
    char *e = s;
    while (*e && !fstab_space(*e)) {
        e++;
    }
    if (*e) {
        *e++ = '\0';
    }
    *p = e;
    return s;
}

static inline bool fstab_same(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++, b++;
    }
    return *a == *b;
}

/* Parse one line (changed in place). False for comments, blank lines and anything not "UUID=". */
static inline bool fstab_parse(char *line, struct fstab_entry *e) {
    char *p = line;
    char *spec = fstab_field(&p), *path = fstab_field(&p);
    fstab_field(&p);   /* the type */
    char *opts = fstab_field(&p);
    if (!spec || !path || spec[0] == '#' || spec[0] != 'U' || spec[1] != 'U' || spec[2] != 'I' ||
        spec[3] != 'D' || spec[4] != '=' || !spec[5]) {
        return false;
    }
    e->uuid = spec + 5;
    size_t o = 0;
    for (const char *c = path; *c && o < sizeof(e->path) - 1; c++) {
        if (c[0] == '\\' && c[1] >= '0' && c[1] <= '3' && c[2] >= '0' && c[2] <= '7' && c[3] >= '0' && c[3] <= '7') {
            e->path[o++] = (char)((c[1] - '0') * 64 + (c[2] - '0') * 8 + (c[3] - '0'));
            c += 3;
        } else {
            e->path[o++] = *c;
        }
    }
    e->path[o] = '\0';
    e->noauto = e->rw = false;
    for (char *c = opts; c && *c;) {
        char *comma = c;
        while (*comma && *comma != ',') {
            comma++;
        }
        char saved = *comma;
        *comma = '\0';
        e->noauto |= fstab_same(c, "noauto");
        e->rw |= fstab_same(c, "rw");
        *comma = saved;
        c = *comma ? comma + 1 : comma;
    }
    return true;
}

/* One line for an entry (with its newline) into out; returns its length (0 if it didn't fit). */
static inline size_t fstab_format(char *out, size_t size, const char *uuid, const char *path, bool noauto,
                                  bool rw) {
    size_t o = 0;
#define FSTAB_PUT(s)                                         \
    for (const char *q_ = (s); *q_; q_++) {                  \
        if (o + 1 >= size) {                                 \
            return 0;                                        \
        }                                                    \
        out[o++] = *q_;                                      \
    }
    FSTAB_PUT("UUID=");
    FSTAB_PUT(uuid);
    FSTAB_PUT("\t");
    for (const char *c = path; *c; c++) {
        if (*c == ' ' || *c == '\t' || *c == '\\' || *c == '\n') {
            char esc[5] = { '\\', (char)('0' + ((unsigned char)*c >> 6)), (char)('0' + ((*c >> 3) & 7)),
                            (char)('0' + (*c & 7)), '\0' };
            FSTAB_PUT(esc);
        } else {
            char one[2] = { *c, '\0' };
            FSTAB_PUT(one);
        }
    }
    FSTAB_PUT("\tauto\t");
    FSTAB_PUT(noauto && rw ? "noauto,rw" : noauto ? "noauto" : rw ? "rw" : "defaults");
    FSTAB_PUT("\t0\t0\n");
#undef FSTAB_PUT
    out[o] = '\0';
    return o;
}
