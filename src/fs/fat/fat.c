/*
 * FAT12/16/32 with long file names (VFAT), read-write. After Linux's fs/fat:
 *
 *   inode.c        the boot sector / BPB checks (fat_fill_super), the "dirty"
 *                  state byte set while mounted (fat_set_state), FSInfo
 *   fatent.c       reading and writing FAT entries (12-bit ones straddle bytes),
 *                  finding free clusters, all FAT copies kept the same
 *   dir.c          walking directory entries, long-name slots and their
 *                  checksum (fat_parse_long, fat_checksum)
 *   namei_vfat.c   short names for long ones: uppercase, bad characters to '_',
 *                  "~N" tails (vfat_create_shortname, "mixed" mode: a short name
 *                  alone only for an all-uppercase 8.3 name)
 *
 * Times are local time, as FAT stores them (and as the OS clock runs).
 */

#include "fs/filesystems.h"
#include "fs/diskfs.h"
#include "block/block.h"
#include "drivers/rtc.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"

#define ATTR_RO     0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYS    0x04
#define ATTR_VOLUME 0x08
#define ATTR_DIR    0x10
#define ATTR_ARCH   0x20
#define ATTR_LFN    0x0F

#define DELETED     0xE5
#define CASE_LOWER_BASE 0x08
#define CASE_LOWER_EXT  0x10
#define STATE_DIRTY 0x01

#define ENTRY 32

struct fat {
    struct mount *m;
    struct block_device *dev;
    int bits;                    /* 12, 16 or 32 */
    uint32_t sector_size;
    uint32_t cluster_size;       /* bytes */
    uint64_t fat_start;          /* bytes */
    uint64_t fat_bytes;          /* one FAT */
    uint32_t fats;
    uint64_t root_start;         /* FAT12/16: the fixed root directory */
    uint32_t root_entries;
    uint64_t data_start;         /* cluster 2 */
    uint32_t clusters;           /* data clusters: valid numbers are 2 .. clusters + 1 */
    uint32_t root_cluster;       /* FAT32 */
    uint32_t free_count;         /* UINT32_MAX: not known yet */
    uint32_t next_free;
    uint32_t fsinfo_sector;      /* FAT32, 0 if none */
    bool was_dirty;              /* not cleanly unmounted last time: leave the state alone */
};

struct fnode {
    uint32_t cluster;            /* first cluster, 0 if none (empty file, or the FAT12/16 root) */
    bool fixed_root;             /* the FAT12/16 root directory */
    bool is_root;
    uint32_t slot;               /* the short entry's index in the parent */
    uint8_t lfn_slots;           /* long-name slots just before it */
    uint8_t attr;
    /* Where the last read/write was: chain walks restart from here. */
    uint32_t cursor_index, cursor_cluster;
};

static struct fat *F(struct dnode *n) {
    return n->m->fs;
}

static struct fnode *FN(struct dnode *n) {
    return n->priv;
}

static uint32_t eoc(struct fat *f) {
    return f->bits == 12 ? 0xFFF : f->bits == 16 ? 0xFFFF : 0x0FFFFFFF;
}

static bool is_eoc(struct fat *f, uint32_t v) {
    return v >= (f->bits == 12 ? 0xFF8u : f->bits == 16 ? 0xFFF8u : 0x0FFFFFF8u);
}

static bool valid_cluster(struct fat *f, uint32_t c) {
    return c >= 2 && c < f->clusters + 2;
}

static uint64_t cluster_pos(struct fat *f, uint32_t c) {
    return f->data_start + (uint64_t)(c - 2) * f->cluster_size;
}

/* --- the FAT (fatent.c) -------------------------------------------------------------------------------- */

static int fat_get(struct fat *f, uint32_t c, uint32_t *out) {
    uint8_t b[4] = { 0 };
    int err;
    if (f->bits == 12) {
        err = bio_read(f->dev, f->fat_start + c + c / 2, b, 2);
        uint16_t v = (uint16_t)(b[0] | b[1] << 8);
        *out = c & 1 ? v >> 4 : v & 0xFFF;
    } else if (f->bits == 16) {
        err = bio_read(f->dev, f->fat_start + (uint64_t)c * 2, b, 2);
        *out = get16(b);
    } else {
        err = bio_read(f->dev, f->fat_start + (uint64_t)c * 4, b, 4);
        *out = get32(b) & 0x0FFFFFFF;
    }
    return err;
}

static int fat_set(struct fat *f, uint32_t c, uint32_t v) {
    int err = 0;
    for (uint32_t i = 0; i < f->fats && !err; i++) {
        uint64_t base = f->fat_start + i * f->fat_bytes;
        uint8_t b[4];
        if (f->bits == 12) {
            uint64_t at = base + c + c / 2;
            err = bio_read(f->dev, at, b, 2);
            uint16_t old = (uint16_t)(b[0] | b[1] << 8), nv;
            nv = c & 1 ? (uint16_t)((old & 0x000F) | (v & 0xFFF) << 4) : (uint16_t)((old & 0xF000) | (v & 0xFFF));
            put16(b, nv);
            if (!err) {
                err = bio_write(f->dev, at, b, 2);
            }
        } else if (f->bits == 16) {
            put16(b, (uint16_t)v);
            err = bio_write(f->dev, base + (uint64_t)c * 2, b, 2);
        } else {
            uint64_t at = base + (uint64_t)c * 4;
            err = bio_read(f->dev, at, b, 4);
            put32(b, (get32(b) & 0xF0000000) | (v & 0x0FFFFFFF));   /* the top 4 bits are reserved */
            if (!err) {
                err = bio_write(f->dev, at, b, 4);
            }
        }
    }
    return err;
}

/* Count free clusters, reading the FAT a chunk at a time. */
static int count_free(struct fat *f) {
    static uint8_t chunk[4096];
    uint32_t free = 0;
    uint64_t total_bytes = f->bits == 12 ? (uint64_t)(f->clusters + 2) * 3 / 2 + 1
                                         : (uint64_t)(f->clusters + 2) * (uint32_t)(f->bits / 8);
    if (f->bits == 12) {
        for (uint32_t c = 2; c < f->clusters + 2; c++) {
            uint32_t v;
            int err = fat_get(f, c, &v);
            if (err) {
                return err;
            }
            free += v == 0;
        }
    } else {
        uint32_t per = (uint32_t)f->bits / 8;
        for (uint64_t at = 0; at < total_bytes; at += sizeof(chunk)) {
            uint32_t n = total_bytes - at < sizeof(chunk) ? (uint32_t)(total_bytes - at) : sizeof(chunk);
            int err = bio_read(f->dev, f->fat_start + at, chunk, n);
            if (err) {
                return err;
            }
            for (uint32_t i = 0; i + per <= n; i += per) {
                uint32_t c = (uint32_t)((at + i) / per);
                if (c < 2 || c >= f->clusters + 2) {
                    continue;
                }
                uint32_t v = per == 2 ? get16(chunk + i) : get32(chunk + i) & 0x0FFFFFFF;
                free += v == 0;
            }
        }
    }
    f->free_count = free;
    return 0;
}

/* A free cluster, marked end-of-chain (and linked after `prev` if that's a cluster). */
static int alloc_cluster(struct fat *f, uint32_t prev, uint32_t *out) {
    if (f->free_count == 0) {
        return -ENOSPC;
    }
    uint32_t start = valid_cluster(f, f->next_free) ? f->next_free : 2;
    uint32_t c = start;
    for (uint32_t tries = 0; tries < f->clusters; tries++) {
        uint32_t v;
        int err = fat_get(f, c, &v);
        if (err) {
            return err;
        }
        if (v == 0) {
            err = fat_set(f, c, eoc(f));
            if (!err && prev) {
                err = fat_set(f, prev, c);
            }
            if (err) {
                return err;
            }
            if (f->free_count != UINT32_MAX) {
                f->free_count--;
            }
            f->next_free = c + 1;
            *out = c;
            return 0;
        }
        if (++c >= f->clusters + 2) {
            c = 2;
        }
    }
    f->free_count = 0;
    return -ENOSPC;
}

static int free_chain(struct fat *f, uint32_t c) {
    for (uint32_t guard = 0; valid_cluster(f, c) && guard <= f->clusters; guard++) {
        uint32_t next;
        int err = fat_get(f, c, &next);
        if (!err) {
            err = fat_set(f, c, 0);
        }
        if (err) {
            return err;
        }
        if (f->free_count != UINT32_MAX) {
            f->free_count++;
        }
        if (c < f->next_free) {
            f->next_free = c;
        }
        if (is_eoc(f, next) || next == 0) {
            break;
        }
        c = next;
    }
    return 0;
}

static int zero_cluster(struct fat *f, uint32_t c) {
    static uint8_t zeros[4096];
    for (uint32_t at = 0; at < f->cluster_size; at += sizeof(zeros)) {
        uint32_t n = f->cluster_size - at < sizeof(zeros) ? f->cluster_size - at : sizeof(zeros);
        int err = bio_write(f->dev, cluster_pos(f, c) + at, zeros, n);
        if (err) {
            return err;
        }
    }
    return 0;
}

/* The cluster holding the `index`-th cluster of the node's chain; 0 past the end. */
static int nth_cluster(struct fat *f, struct fnode *fn, uint32_t index, uint32_t *out) {
    uint32_t c = fn->cluster, i = 0;
    if (fn->cursor_cluster && fn->cursor_index <= index) {
        c = fn->cursor_cluster;
        i = fn->cursor_index;
    }
    while (i < index) {
        if (!valid_cluster(f, c)) {
            *out = 0;
            return 0;
        }
        uint32_t next;
        int err = fat_get(f, c, &next);
        if (err) {
            return err;
        }
        if (is_eoc(f, next) || next == 0) {
            *out = 0;
            return 0;
        }
        c = next;
        i++;
    }
    if (!valid_cluster(f, c)) {
        *out = 0;
        return 0;
    }
    fn->cursor_index = index;
    fn->cursor_cluster = c;
    *out = c;
    return 0;
}

/* Make the chain at least `count` clusters long (new ones zeroed if `zero`). */
static int extend_chain(struct fat *f, struct fnode *fn, uint32_t count, bool zero) {
    if (!count) {
        return 0;
    }
    uint32_t last = 0, have = 0;
    if (fn->cluster) {
        /* Find the end (and how long the chain is). */
        uint32_t c = fn->cluster;
        have = 1;
        if (fn->cursor_cluster) {
            c = fn->cursor_cluster;
            have = fn->cursor_index + 1;
        }
        for (;;) {
            uint32_t next;
            int err = fat_get(f, c, &next);
            if (err) {
                return err;
            }
            if (is_eoc(f, next) || !valid_cluster(f, next)) {
                break;
            }
            c = next;
            have++;
        }
        last = c;
        fn->cursor_cluster = c;
        fn->cursor_index = have - 1;
    }
    while (have < count) {
        uint32_t c;
        int err = alloc_cluster(f, last, &c);
        if (!err && zero) {
            err = zero_cluster(f, c);
        }
        if (err) {
            return err;
        }
        if (!fn->cluster) {
            fn->cluster = c;
        }
        fn->cursor_cluster = c;
        fn->cursor_index = have;
        last = c;
        have++;
    }
    return 0;
}

/* --- reading and writing a chain ------------------------------------------------------------------------- */

/* Move bytes between buf and the node's clusters, a run of consecutive clusters at a time. */
static int64_t chain_io(struct fat *f, struct fnode *fn, uint64_t offset, uint8_t *buf, size_t len, bool write) {
    size_t done = 0;
    while (done < len) {
        uint32_t index = (uint32_t)((offset + done) / f->cluster_size);
        uint32_t within = (uint32_t)((offset + done) % f->cluster_size);
        uint32_t c;
        int err = nth_cluster(f, fn, index, &c);
        if (err) {
            return err;
        }
        if (!c) {
            return -EIO;   /* the chain is shorter than the size says */
        }
        /* How many of the following clusters come right after it on disk? */
        size_t span = f->cluster_size - within;
        uint32_t run_end = c;
        while (done + span < len && span < 1024 * 1024) {
            uint32_t next;
            if (fat_get(f, run_end, &next) || next != run_end + 1) {
                break;
            }
            run_end = next;
            span += f->cluster_size;
            fn->cursor_index++;
            fn->cursor_cluster = run_end;
        }
        size_t n = len - done < span ? len - done : span;
        uint64_t pos = cluster_pos(f, c) + within;
        err = write ? bio_write(f->dev, pos, buf + done, n) : bio_read(f->dev, pos, buf + done, n);
        if (err) {
            return err;
        }
        done += n;
    }
    return (int64_t)done;
}

/* --- directory entries (dir.c) ------------------------------------------------------------------------------ */

/* Byte position of entry `slot` of a directory; 0 if the directory isn't that long. */
static int slot_pos(struct fat *f, struct fnode *dir, uint32_t slot, uint64_t *pos) {
    if (dir->fixed_root) {
        *pos = slot < f->root_entries ? f->root_start + (uint64_t)slot * ENTRY : 0;
        return 0;
    }
    uint64_t byte = (uint64_t)slot * ENTRY;
    uint32_t c;
    int err = nth_cluster(f, dir, (uint32_t)(byte / f->cluster_size), &c);
    if (err) {
        return err;
    }
    *pos = c ? cluster_pos(f, c) + byte % f->cluster_size : 0;
    return 0;
}

static int read_slot(struct fat *f, struct fnode *dir, uint32_t slot, uint8_t *e, bool *end) {
    uint64_t pos;
    int err = slot_pos(f, dir, slot, &pos);
    if (err) {
        return err;
    }
    if (!pos) {
        *end = true;
        return 0;
    }
    *end = false;
    return bio_read(f->dev, pos, e, ENTRY);
}

static int write_slot(struct fat *f, struct fnode *dir, uint32_t slot, const uint8_t *e) {
    uint64_t pos;
    int err = slot_pos(f, dir, slot, &pos);
    if (err) {
        return err;
    }
    return pos ? bio_write(f->dev, pos, e, ENTRY) : -EIO;
}

static uint8_t sfn_checksum(const uint8_t *name11) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name11[i]);
    }
    return sum;
}

static uint64_t fat_time(uint16_t date, uint16_t time) {
    if (!date) {
        return 0;
    }
    struct dtime t = { 1980 + (date >> 9), (date >> 5) & 15, date & 31, time >> 11, (time >> 5) & 63,
                       (time & 31) * 2 };
    return dtime_to_unix(&t);
}

static void to_fat_time(uint64_t unix_time, uint16_t *date, uint16_t *time) {
    struct dtime t;
    dtime_from_unix(unix_time, &t);
    if (t.year < 1980) {
        t.year = 1980;
        t.month = t.day = 1;
        t.hour = t.minute = t.second = 0;
    }
    *date = (uint16_t)((t.year - 1980) << 9 | t.month << 5 | t.day);
    *time = (uint16_t)(t.hour << 11 | t.minute << 5 | t.second / 2);
}

/* "README  TXT" + case flags -> "readme.txt" */
static void sfn_to_name(const uint8_t *e, char *out) {
    int o = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        char c = (char)(i == 0 && e[0] == 0x05 ? 0xE5 : e[i]);
        out[o++] = (e[12] & CASE_LOWER_BASE) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    if (e[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            char c = (char)e[i];
            out[o++] = (e[12] & CASE_LOWER_EXT) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
        }
    }
    out[o] = '\0';
    /* Bytes above 0x7F are in some DOS code page we don't know: keep them printable. */
    for (int i = 0; out[i]; i++) {
        if ((unsigned char)out[i] >= 0x80) {
            out[i] = '_';
        }
    }
}

static struct fnode *new_fnode(uint32_t cluster, uint8_t attr, uint32_t slot, uint8_t lfn_slots) {
    struct fnode *fn = kzalloc(sizeof(*fn));
    if (fn) {
        fn->cluster = cluster;
        fn->attr = attr;
        fn->slot = slot;
        fn->lfn_slots = lfn_slots;
    }
    return fn;
}

/* fat_search_long / fat_parse_long: the entries, with long names where their checksums match. */
static int fat_list(struct dnode *dir) {
    struct fat *f = F(dir);
    struct fnode *d = FN(dir);
    uint16_t lfn[20 * 13 + 1];
    int lfn_count = 0, lfn_expect = 0;
    uint8_t lfn_sum = 0;
    for (uint32_t slot = 0; slot < 65536; slot++) {
        uint8_t e[ENTRY];
        bool end;
        int err = read_slot(f, d, slot, e, &end);
        if (err) {
            return err;
        }
        if (end || e[0] == 0x00) {
            break;
        }
        if (e[0] == DELETED) {
            lfn_count = lfn_expect = 0;
            continue;
        }
        if (e[11] == ATTR_LFN) {
            int seq = e[0] & 0x1F;
            if (e[0] & 0x40) {
                lfn_expect = seq;
                lfn_count = 0;
                lfn_sum = e[13];
                memset(lfn, 0, sizeof(lfn));
            }
            if (!lfn_expect || seq < 1 || seq > 20 || e[13] != lfn_sum) {
                lfn_expect = 0;
                continue;
            }
            uint16_t *p = lfn + (seq - 1) * 13;
            static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int i = 0; i < 13; i++) {
                p[i] = get16(e + at[i]);
            }
            lfn_count++;
            continue;
        }
        if (e[11] & ATTR_VOLUME) {
            lfn_count = lfn_expect = 0;
            continue;
        }
        if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) {
            lfn_count = lfn_expect = 0;
            continue;
        }
        char name[VFS_NAME_MAX];
        int slots = 0;
        if (lfn_expect && lfn_count == lfn_expect && sfn_checksum(e) == lfn_sum) {
            for (int i = 0; i < lfn_expect * 13; i++) {
                if (lfn[i] == 0xFFFF) {
                    lfn[i] = 0;
                }
            }
            lfn[lfn_expect * 13] = 0;
            utf16_to_utf8(lfn, (size_t)lfn_expect * 13, name, sizeof(name));
            slots = lfn_expect;
        } else {
            sfn_to_name(e, name);
        }
        lfn_count = lfn_expect = 0;
        if (!name[0]) {
            continue;
        }
        uint32_t cluster = get16(e + 26) | (f->bits == 32 ? (uint32_t)get16(e + 20) << 16 : 0);
        bool is_dir = e[11] & ATTR_DIR;
        struct fnode *fn = new_fnode(cluster, e[11], slot, (uint8_t)slots);
        struct dnode *n = fn ? diskfs_new(dir->m, name, is_dir ? VNODE_DIR : VNODE_FILE, get32(e + 28), fn) : NULL;
        if (!n) {
            kfree(fn);
            return -ENOMEM;
        }
        n->v.mtime = fat_time(get16(e + 24), get16(e + 22));
        n->v.ctime = fat_time(get16(e + 16), get16(e + 14));
        if ((e[11] & ATTR_RO) && !dir->m->read_only) {
            n->v.mode &= ~0222u;
        }
        diskfs_add(dir, n);
    }
    return 0;
}

/* Rewrite a node's short entry from what we know (size, first cluster, times). */
static int update_entry(struct dnode *n) {
    struct fat *f = F(n);
    struct fnode *fn = FN(n);
    if (fn->is_root) {
        return 0;
    }
    struct fnode *dir = FN((struct dnode *)n->v.parent);
    uint8_t e[ENTRY];
    bool end;
    int err = read_slot(f, dir, fn->slot, e, &end);
    if (err || end) {
        return err ? err : -EIO;
    }
    put16(e + 26, (uint16_t)fn->cluster);
    put16(e + 20, f->bits == 32 ? (uint16_t)(fn->cluster >> 16) : 0);
    put32(e + 28, n->v.type == VNODE_DIR ? 0 : (uint32_t)n->v.size);
    uint16_t date, time;
    to_fat_time(n->v.mtime ? n->v.mtime : rtc_time(), &date, &time);
    put16(e + 22, time);
    put16(e + 24, date);
    put16(e + 18, date);   /* last access */
    if (n->v.type == VNODE_FILE) {
        e[11] |= ATTR_ARCH;
    }
    return write_slot(f, dir, fn->slot, e);
}

/* --- making names (namei_vfat.c) ------------------------------------------------------------------------- */

static bool sfn_char_ok(uint8_t c) {
    if (c < 0x20 || c >= 0x7F) {
        return false;
    }
    static const char bad[] = "\"*+,./:;<=>?[\\]| ";
    for (const char *b = bad; *b; b++) {
        if (c == (uint8_t)*b) {
            return false;
        }
    }
    return true;
}

static bool name_ok(const char *name) {
    static const char bad[] = "\"*/:<>?\\|";
    size_t len = strlen(name);
    if (!len || len > 255 || name[len - 1] == '.' || name[len - 1] == ' ') {
        return false;   /* Windows can't reach names ending in a dot or space */
    }
    for (const char *c = name; *c; c++) {
        if ((uint8_t)*c < 0x20) {
            return false;
        }
        for (const char *b = bad; *b; b++) {
            if (*c == *b) {
                return false;
            }
        }
    }
    return true;
}

/* Is there an entry with this 11-byte short name in the directory? */
static int sfn_taken(struct fat *f, struct fnode *dir, const uint8_t *name11, bool *taken) {
    *taken = false;
    for (uint32_t slot = 0; slot < 65536; slot++) {
        uint8_t e[ENTRY];
        bool end;
        int err = read_slot(f, dir, slot, e, &end);
        if (err) {
            return err;
        }
        if (end || e[0] == 0) {
            return 0;
        }
        if (e[0] != DELETED && e[11] != ATTR_LFN && memcmp(e, name11, 11) == 0) {
            *taken = true;
            return 0;
        }
    }
    return 0;
}

/* The short name for `name`: *lfn_needed says whether a long name has to go with it. */
static int make_sfn(struct fat *f, struct fnode *dir, const char *name, uint8_t *out, uint8_t *lcase, bool *lfn_needed) {
    const char *dot = strrchr(name, '.');
    if (dot == name) {
        dot = NULL;   /* ".bashrc": all name, no extension */
    }
    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext_len = dot ? strlen(dot + 1) : 0;

    /* Already a valid all-uppercase 8.3 name? Then that's all it needs. */
    bool exact = base_len >= 1 && base_len <= 8 && ext_len <= 3 && (!dot || ext_len > 0);
    for (size_t i = 0; exact && name[i]; i++) {
        uint8_t c = (uint8_t)name[i];
        if (name + i == dot) {
            continue;
        }
        if (!sfn_char_ok(c) || (c >= 'a' && c <= 'z')) {
            exact = false;
        }
    }
    memset(out, ' ', 11);
    *lcase = 0;
    if (exact) {
        memcpy(out, name, base_len);
        memcpy(out + 8, dot ? dot + 1 : "", ext_len);
        if (out[0] == DELETED) {
            out[0] = 0x05;
        }
        bool taken;
        int err = sfn_taken(f, dir, out, &taken);
        if (err) {
            return err;
        }
        *lfn_needed = false;
        return taken ? -EEXIST : 0;
    }

    /* Base: up to 6 usable characters, then ~N; extension: up to 3. */
    *lfn_needed = true;
    uint8_t base[8], ext[3];
    int bl = 0, el = 0;
    for (size_t i = 0; i < base_len && bl < 8; i++) {
        uint8_t c = (uint8_t)name[i];
        if (c == ' ' || c == '.') {
            continue;
        }
        if (c >= 0x80) {
            while (i + 1 < base_len && ((uint8_t)name[i + 1] & 0xC0) == 0x80) {
                i++;   /* one '_' per UTF-8 character */
            }
            c = '_';
        } else if (c >= 'a' && c <= 'z') {
            c = (uint8_t)(c - 32);
        } else if (!sfn_char_ok(c)) {
            c = '_';
        }
        base[bl++] = c;
    }
    for (size_t i = 0; dot && i < ext_len && el < 3; i++) {
        uint8_t c = (uint8_t)dot[1 + i];
        if (c == ' ' || c == '.') {
            continue;
        }
        if (c >= 0x80) {
            while (i + 1 < ext_len && ((uint8_t)dot[2 + i] & 0xC0) == 0x80) {
                i++;
            }
            c = '_';
        } else if (c >= 'a' && c <= 'z') {
            c = (uint8_t)(c - 32);
        } else if (!sfn_char_ok(c)) {
            c = '_';
        }
        ext[el++] = c;
    }
    if (bl == 0) {
        base[bl++] = '_';
    }
    memcpy(out + 8, ext, (size_t)el);
    for (int n = 1; n < 1000000; n++) {
        char tail[8];
        int tl = 0;
        int v = n;
        char digits[7];
        int dl = 0;
        do {
            digits[dl++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        tail[tl++] = '~';
        while (dl) {
            tail[tl++] = digits[--dl];
        }
        int keep = bl < 8 - tl ? bl : 8 - tl;
        memset(out, ' ', 8);
        memcpy(out, base, (size_t)keep);
        memcpy(out + keep, tail, (size_t)tl);
        if (out[0] == DELETED) {
            out[0] = 0x05;
        }
        bool taken;
        int err = sfn_taken(f, dir, out, &taken);
        if (err) {
            return err;
        }
        if (!taken) {
            return 0;
        }
    }
    return -EEXIST;
}

/* `count` free slots in a row (growing the directory if needed); the first one's index. */
static int find_free_slots(struct fat *f, struct dnode *dirnode, int count, uint32_t *first) {
    struct fnode *dir = FN(dirnode);
    int run = 0;
    uint32_t slot = 0;
    for (;; slot++) {
        uint8_t e[ENTRY];
        bool end;
        int err = read_slot(f, dir, slot, e, &end);
        if (err) {
            return err;
        }
        if (end) {
            break;
        }
        if (e[0] == 0x00) {
            /* Everything from here on is free (as far as the directory goes). */
            uint64_t have = dir->fixed_root ? f->root_entries : 0;
            if (!dir->fixed_root) {
                uint64_t pos;
                uint32_t s2 = slot;
                for (;; s2++) {
                    err = slot_pos(f, dir, s2, &pos);
                    if (err) {
                        return err;
                    }
                    if (!pos) {
                        break;
                    }
                }
                have = s2;
            }
            if (have - (slot - (uint32_t)run) >= (uint64_t)count) {
                *first = slot - (uint32_t)run;
                return 0;
            }
            break;
        }
        if (e[0] == DELETED) {
            if (++run == count) {
                *first = slot - (uint32_t)count + 1;
                return 0;
            }
        } else {
            run = 0;
        }
    }
    if (dir->fixed_root) {
        return -ENOSPC;   /* the FAT12/16 root can't grow */
    }
    /* Add clusters (zeroed: all free, and the 0x00 end marker) until there's room, starting after the
       last used slot (free ones at the end count too). */
    uint32_t per = f->cluster_size / ENTRY;
    uint32_t start = slot - (uint32_t)run;
    uint32_t clusters = (uint32_t)((start + (uint32_t)count + per - 1) / per);
    if (clusters > 65536 * ENTRY / f->cluster_size) {
        return -ENOSPC;   /* a directory can have at most 65536 entries */
    }
    int err = extend_chain(f, dir, clusters, true);
    if (err) {
        return err;
    }
    *first = start;
    return 0;
}

/* Write the long-name slots and the short entry for `name` at `first`. */
static int write_entries(struct fat *f, struct fnode *dir, uint32_t first, const char *name, const uint8_t *sfn,
                         uint8_t lcase, bool lfn, const uint8_t *template_entry, uint8_t *slots_out) {
    uint16_t u[20 * 13];
    int len = 0, slots = 0;
    if (lfn) {
        len = utf8_to_utf16(name, u, 255);
        if (len <= 0) {
            return -EINVAL;
        }
        slots = (len + 12) / 13;
    }
    uint8_t sum = sfn_checksum(sfn);
    static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int s = 0; s < slots; s++) {
        int seq = slots - s;   /* the last part of the name comes first */
        uint8_t e[ENTRY];
        memset(e, 0, sizeof(e));
        e[0] = (uint8_t)(seq | (s == 0 ? 0x40 : 0));
        e[11] = ATTR_LFN;
        e[13] = sum;
        for (int i = 0; i < 13; i++) {
            int k = (seq - 1) * 13 + i;
            uint16_t ch = k < len ? u[k] : k == len ? 0x0000 : 0xFFFF;
            put16(e + at[i], ch);
        }
        int err = write_slot(f, dir, first + (uint32_t)s, e);
        if (err) {
            return err;
        }
    }
    uint8_t e[ENTRY];
    memcpy(e, template_entry, ENTRY);
    memcpy(e, sfn, 11);
    e[12] = lcase;
    *slots_out = (uint8_t)slots;
    return write_slot(f, dir, first + (uint32_t)slots, e);
}

static int erase_entries(struct fat *f, struct fnode *dir, struct fnode *fn) {
    for (uint32_t s = fn->slot - fn->lfn_slots; s <= fn->slot; s++) {
        uint8_t e[ENTRY];
        bool end;
        int err = read_slot(f, dir, s, e, &end);
        if (err || end) {
            return err ? err : -EIO;
        }
        e[0] = DELETED;
        err = write_slot(f, dir, s, e);
        if (err) {
            return err;
        }
    }
    return 0;
}

/* Place `name` in `dirnode` with the short entry `template_entry`; fills fn->slot / lfn_slots. */
static int add_entry(struct fat *f, struct dnode *dirnode, const char *name, uint8_t *template_entry, struct fnode *fn) {
    if (!name_ok(name)) {
        return -EINVAL;
    }
    struct fnode *dir = FN(dirnode);
    uint8_t sfn[11], lcase;
    bool lfn;
    int err = make_sfn(f, dir, name, sfn, &lcase, &lfn);
    if (err) {
        return err;
    }
    int need = 1;
    if (lfn) {
        uint16_t u[20 * 13];
        int len = utf8_to_utf16(name, u, 255);
        if (len <= 0) {
            return -ENAMETOOLONG;
        }
        need += (len + 12) / 13;
    }
    uint32_t first;
    err = find_free_slots(f, dirnode, need, &first);
    if (err) {
        return err;
    }
    uint8_t slots;
    err = write_entries(f, dir, first, name, sfn, lcase, lfn, template_entry, &slots);
    if (err) {
        return err;
    }
    fn->slot = first + slots;
    fn->lfn_slots = slots;
    return 0;
}

/* --- operations ---------------------------------------------------------------------------------------- */

static int64_t fat_read(struct dnode *n, uint64_t offset, void *buf, size_t len) {
    return chain_io(F(n), FN(n), offset, buf, len, false);
}

static int64_t fat_write(struct dnode *n, uint64_t offset, const void *buf, size_t len) {
    struct fat *f = F(n);
    struct fnode *fn = FN(n);
    if (offset + len > 0xFFFFFFFFull) {
        return -ENOSPC;   /* FAT files stop at 4 GiB */
    }
    if (!len) {
        return 0;
    }
    uint64_t end = offset + len;
    uint32_t need = (uint32_t)((end + f->cluster_size - 1) / f->cluster_size);
    /* Writing past the end leaves a gap: that must read as zeros. */
    int err = extend_chain(f, fn, need, offset > n->v.size);
    if (err) {
        if (n->v.size < end && fn->cluster) {
            update_entry(n);   /* keep what got allocated reachable */
        }
        return err;
    }
    if (offset > n->v.size) {
        /* Zero from the old end to the cluster boundary (the rest of the gap is in new, zeroed clusters). */
        uint64_t from = n->v.size, to = offset;
        uint64_t boundary = (from + f->cluster_size - 1) / f->cluster_size * f->cluster_size;
        if (to > boundary) {
            to = boundary;
        }
        static uint8_t zeros[4096];
        while (from < to) {
            size_t z = to - from < sizeof(zeros) ? (size_t)(to - from) : sizeof(zeros);
            int64_t r = chain_io(f, fn, from, zeros, z, true);
            if (r < 0) {
                return r;
            }
            from += z;
        }
    }
    int64_t r = chain_io(f, fn, offset, (uint8_t *)buf, len, true);
    if (r < 0) {
        return r;
    }
    if (end > n->v.size) {
        n->v.size = end;
    }
    n->v.mtime = rtc_time();
    err = update_entry(n);
    return err ? err : r;
}

static int fat_truncate(struct dnode *n, uint64_t size) {
    struct fat *f = F(n);
    struct fnode *fn = FN(n);
    if (size > 0xFFFFFFFFull) {
        return -ENOSPC;
    }
    if (size > n->v.size) {
        /* Grow: zeros, through write (which handles the gap). */
        uint8_t zero = 0;
        int64_t r = fat_write(n, size - 1, &zero, 1);
        return r < 0 ? (int)r : 0;
    }
    uint32_t keep = (uint32_t)((size + f->cluster_size - 1) / f->cluster_size);
    int err = 0;
    if (keep == 0) {
        uint32_t first = fn->cluster;
        fn->cluster = 0;
        fn->cursor_cluster = fn->cursor_index = 0;
        n->v.size = 0;
        err = update_entry(n);   /* detach first, then free: never a live entry pointing at free clusters */
        if (!err && first) {
            err = free_chain(f, first);
        }
    } else {
        uint32_t last;
        fn->cursor_cluster = fn->cursor_index = 0;
        err = nth_cluster(f, fn, keep - 1, &last);
        if (!err && last) {
            uint32_t next;
            err = fat_get(f, last, &next);
            if (!err && valid_cluster(f, next)) {
                err = fat_set(f, last, eoc(f));
                if (!err) {
                    err = free_chain(f, next);
                }
            }
        }
        n->v.size = size;
        if (!err) {
            err = update_entry(n);
        }
    }
    n->v.mtime = rtc_time();
    return err;
}

static int fat_create(struct dnode *dirnode, struct dnode *node) {
    struct fat *f = F(dirnode);
    struct fnode *fn = new_fnode(0, node->v.type == VNODE_DIR ? ATTR_DIR : ATTR_ARCH, 0, 0);
    if (!fn) {
        return -ENOMEM;
    }
    node->priv = fn;
    uint16_t date, time;
    to_fat_time(rtc_time(), &date, &time);
    int err = 0;
    if (node->v.type == VNODE_DIR) {
        /* A new folder: one zeroed cluster with "." and "..". */
        err = extend_chain(f, fn, 1, true);
        if (err) {
            return err;
        }
        uint8_t e[ENTRY];
        memset(e, 0, sizeof(e));
        memset(e, ' ', 11);
        e[0] = '.';
        e[11] = ATTR_DIR;
        put16(e + 14, time);
        put16(e + 16, date);
        put16(e + 18, date);
        put16(e + 22, time);
        put16(e + 24, date);
        put16(e + 26, (uint16_t)fn->cluster);
        put16(e + 20, f->bits == 32 ? (uint16_t)(fn->cluster >> 16) : 0);
        err = bio_write(f->dev, cluster_pos(f, fn->cluster), e, ENTRY);
        struct fnode *parent = FN(dirnode);
        uint32_t up = parent->is_root ? 0 : parent->cluster;   /* ".." of a root child is 0 */
        e[1] = '.';
        put16(e + 26, (uint16_t)up);
        put16(e + 20, f->bits == 32 ? (uint16_t)(up >> 16) : 0);
        if (!err) {
            err = bio_write(f->dev, cluster_pos(f, fn->cluster) + ENTRY, e, ENTRY);
        }
        if (err) {
            free_chain(f, fn->cluster);
            return err;
        }
    }
    uint8_t e[ENTRY];
    memset(e, 0, sizeof(e));
    e[11] = fn->attr;
    e[13] = 0;
    put16(e + 14, time);
    put16(e + 16, date);
    put16(e + 18, date);
    put16(e + 22, time);
    put16(e + 24, date);
    put16(e + 26, (uint16_t)fn->cluster);
    put16(e + 20, f->bits == 32 ? (uint16_t)(fn->cluster >> 16) : 0);
    err = add_entry(f, dirnode, node->v.name, e, fn);
    if (err && fn->cluster) {
        free_chain(f, fn->cluster);
    }
    return err;
}

static int fat_unlink(struct dnode *dirnode, struct dnode *child) {
    struct fat *f = F(dirnode);
    struct fnode *fn = FN(child);
    int err = erase_entries(f, FN(dirnode), fn);
    if (!err && fn->cluster) {
        err = free_chain(f, fn->cluster);
    }
    return err;
}

static int fat_rename(struct dnode *dirnode, struct dnode *child, struct dnode *new_dir, const char *new_name) {
    struct fat *f = F(dirnode);
    struct fnode *fn = FN(child);
    /* Keep the short entry's contents (cluster, size, times, attributes), give it the new name. */
    uint8_t e[ENTRY];
    bool end;
    int err = read_slot(f, FN(dirnode), fn->slot, e, &end);
    if (err || end) {
        return err ? err : -EIO;
    }
    /* Free the old entries first (they may be reused by the new name in the same folder). */
    struct fnode old = *fn;
    err = erase_entries(f, FN(dirnode), fn);
    if (!err) {
        err = add_entry(f, new_dir, new_name, e, fn);
    }
    if (err) {
        /* Put the old entries back as they were. */
        *fn = old;
        uint8_t back[ENTRY];
        for (uint32_t s = old.slot - old.lfn_slots; s <= old.slot; s++) {
            bool end2;
            if (read_slot(f, FN(dirnode), s, back, &end2) == 0 && !end2) {
                back[0] = s == old.slot ? e[0] : (uint8_t)((old.slot - s) | (s == old.slot - old.lfn_slots ? 0x40 : 0));
                write_slot(f, FN(dirnode), s, back);
            }
        }
        return err;
    }
    /* A folder that changed parents: its ".." must point at the new one. */
    if (child->v.type == VNODE_DIR && dirnode != new_dir && fn->cluster) {
        struct fnode *np = FN(new_dir);
        uint32_t up = np->is_root ? 0 : np->cluster;
        uint8_t dd[ENTRY];
        uint64_t pos = cluster_pos(f, fn->cluster) + ENTRY;
        err = bio_read(f->dev, pos, dd, ENTRY);
        if (!err && dd[0] == '.' && dd[1] == '.') {
            put16(dd + 26, (uint16_t)up);
            put16(dd + 20, f->bits == 32 ? (uint16_t)(up >> 16) : 0);
            err = bio_write(f->dev, pos, dd, ENTRY);
        }
    }
    return err;
}

static void fat_release(struct dnode *n) {
    kfree(n->priv);
}

static const struct diskfs_ops fat_ops = {
    .list = fat_list,
    .read = fat_read,
    .write = fat_write,
    .truncate = fat_truncate,
    .create = fat_create,
    .unlink = fat_unlink,
    .rename = fat_rename,
    .release = fat_release,
};

/* --- mounting (inode.c fat_fill_super) ----------------------------------------------------------------- */

static void set_state(struct fat *f, bool dirty) {
    if (f->was_dirty || f->m->read_only) {
        return;   /* don't hide that the last unmount wasn't clean */
    }
    uint8_t b;
    uint64_t at = f->bits == 32 ? 0x41 : 0x25;
    if (bio_read(f->dev, at, &b, 1) == 0) {
        b = dirty ? (uint8_t)(b | STATE_DIRTY) : (uint8_t)(b & ~STATE_DIRTY);
        bio_write(f->dev, at, &b, 1);
    }
}

static void write_fsinfo(struct fat *f) {
    if (!f->fsinfo_sector || f->m->read_only) {
        return;
    }
    uint8_t s[512];
    uint64_t at = (uint64_t)f->fsinfo_sector * f->sector_size;
    if (bio_read(f->dev, at, s, 512) || get32(s) != 0x41615252 || get32(s + 484) != 0x61417272) {
        return;
    }
    put32(s + 488, f->free_count);
    put32(s + 492, valid_cluster(f, f->next_free) ? f->next_free : 0xFFFFFFFF);
    bio_write(f->dev, at, s, 512);
}

static int fat_mount(struct mount *m) {
    uint8_t *b = kmalloc(512);
    if (!b) {
        return -ENOMEM;
    }
    int err = bio_read(m->dev, 0, b, 512);
    if (err) {
        kfree(b);
        return err;
    }
    uint32_t bps = get16(b + 11), spc = b[13], reserved = get16(b + 14), fats = b[16];
    uint32_t root_entries = get16(b + 17), total16 = get16(b + 19), fat16 = get16(b + 22);
    uint32_t total32 = get32(b + 32), fat32 = get32(b + 36);
    bool ok = (b[0] == 0xEB || b[0] == 0xE9) && b[510] == 0x55 && b[511] == 0xAA && bps >= 512 && bps <= 4096 &&
              !(bps & (bps - 1)) && spc && !(spc & (spc - 1)) && reserved && fats >= 1 && fats <= 2 &&
              (b[21] == 0xF0 || b[21] >= 0xF8) && memcmp(b + 3, "NTFS", 4) != 0 && memcmp(b + 3, "EXFAT", 5) != 0;
    if (!ok) {
        kfree(b);
        return -EINVAL;
    }
    uint32_t fat_sectors = fat16 ? fat16 : fat32;
    uint64_t total = total16 ? total16 : total32;
    uint32_t root_sectors = (root_entries * ENTRY + bps - 1) / bps;
    uint64_t data_sector = reserved + (uint64_t)fats * fat_sectors + root_sectors;
    if (!fat_sectors || !total || data_sector >= total || total * bps > m->dev->sectors * m->dev->sector_size) {
        kfree(b);
        return -EINVAL;
    }
    uint32_t clusters = (uint32_t)((total - data_sector) / spc);

    struct fat *f = kzalloc(sizeof(*f));
    if (!f) {
        kfree(b);
        return -ENOMEM;
    }
    f->m = m;
    f->dev = m->dev;
    /* The FAT type is decided by the cluster count alone (that's the spec, and Linux). */
    f->bits = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    f->sector_size = bps;
    f->cluster_size = bps * spc;
    f->fat_start = (uint64_t)reserved * bps;
    f->fat_bytes = (uint64_t)fat_sectors * bps;
    f->fats = fats;
    f->root_start = f->fat_start + fats * f->fat_bytes;
    f->root_entries = root_entries;
    f->data_start = data_sector * bps;
    f->clusters = clusters;
    f->free_count = UINT32_MAX;
    f->next_free = 2;
    /* The FAT must have room for every cluster's entry. */
    uint64_t fat_need = f->bits == 12 ? ((uint64_t)clusters + 2) * 3 / 2 : ((uint64_t)clusters + 2) * (uint32_t)(f->bits / 8);
    if (fat_need > f->fat_bytes || (f->bits == 32 && fat16) || (f->bits != 32 && !root_entries)) {
        kfree(f);
        kfree(b);
        return -EINVAL;
    }

    const uint8_t *ext = f->bits == 32 ? b + 64 : b + 36;   /* the extended BPB: drive, state, signature... */
    f->was_dirty = ext[1] & STATE_DIRTY;
    char label[12] = "";
    if (ext[2] == 0x29 || ext[2] == 0x28) {
        uint32_t serial = get32(ext + 3);
        ksnprintf(m->uuid, sizeof(m->uuid), "%04X-%04X", serial >> 16, serial & 0xFFFF);
    }
    if (ext[2] == 0x29) {
        memcpy(label, ext + 7, 11);
    }
    if (f->bits == 32) {
        f->root_cluster = get32(b + 44);
        f->fsinfo_sector = get16(b + 48);
        if (f->fsinfo_sector && f->fsinfo_sector < reserved) {
            uint8_t s[512];
            if (bio_read(m->dev, (uint64_t)f->fsinfo_sector * bps, s, 512) == 0 && get32(s) == 0x41615252 &&
                get32(s + 484) == 0x61417272) {
                uint32_t fc = get32(s + 488), nf = get32(s + 492);
                if (fc <= clusters && !f->was_dirty) {
                    f->free_count = fc;
                }
                if (valid_cluster(f, nf)) {
                    f->next_free = nf;
                }
            } else {
                f->fsinfo_sector = 0;
            }
        } else {
            f->fsinfo_sector = 0;
        }
        if (!valid_cluster(f, f->root_cluster)) {
            kfree(f);
            kfree(b);
            return -EINVAL;
        }
    }
    kfree(b);

    struct fnode *root = new_fnode(f->bits == 32 ? f->root_cluster : 0, ATTR_DIR, 0, 0);
    if (!root) {
        kfree(f);
        return -ENOMEM;
    }
    root->is_root = true;
    root->fixed_root = f->bits != 32;
    m->fs = f;
    m->ops = &fat_ops;
    m->case_insensitive = true;
    ksnprintf(m->fs_name, sizeof(m->fs_name), "FAT%d", f->bits);

    /* The label: the root's volume entry wins over the boot sector's copy (Windows renames that one). */
    for (uint32_t slot = 0; slot < 4096; slot++) {
        uint8_t e[ENTRY];
        bool end;
        if (read_slot(f, root, slot, e, &end) || end || e[0] == 0) {
            break;
        }
        if (e[0] != DELETED && e[11] != ATTR_LFN && (e[11] & ATTR_VOLUME) && !(e[11] & ATTR_DIR)) {
            memcpy(label, e, 11);
            label[11] = '\0';
            break;
        }
    }
    int ll = 11;
    while (ll > 0 && (label[ll - 1] == ' ' || label[ll - 1] == '\0')) {
        ll--;
    }
    label[ll] = '\0';
    if (strcmp(label, "NO NAME") == 0) {
        label[0] = '\0';
    }
    for (int i = 0; label[i]; i++) {
        if ((unsigned char)label[i] >= 0x80) {
            label[i] = '_';
        }
    }
    strlcpy(m->label, label, sizeof(m->label));

    m->root = diskfs_new(m, "", VNODE_DIR, 0, root);
    if (!m->root) {
        kfree(root);
        kfree(f);
        return -ENOMEM;
    }
    if (!m->read_only && !m->dev->read_only) {
        set_state(f, true);
        bio_sync(m->dev);
    }
    return 0;
}

static void fat_unmount(struct mount *m) {
    struct fat *f = m->fs;
    if (!m->dead && !m->read_only) {
        if (f->free_count != UINT32_MAX) {
            write_fsinfo(f);
        }
        set_state(f, false);
    }
    kfree(f);
    m->fs = NULL;
}

static int fat_statfs(struct mount *m, uint64_t *total, uint64_t *free) {
    struct fat *f = m->fs;
    if (f->free_count == UINT32_MAX) {
        count_free(f);
    }
    *total = (uint64_t)f->clusters * f->cluster_size;
    *free = f->free_count == UINT32_MAX ? 0 : (uint64_t)f->free_count * f->cluster_size;
    return 0;
}

const struct fs_driver fat_driver = {
    .name = "FAT",
    .mount = fat_mount,
    .unmount = fat_unmount,
    .statfs = fat_statfs,
};
