/*
 * exFAT, read-write. After Linux's fs/exfat:
 *
 *   super.c     the boot sector checks, the volume-dirty flag (exfat_set_vol_flags)
 *   nls.c       the compressed upcase table (exfat_load_upcase_table), name hashes
 *   misc.c      the entry-set and boot checksums (exfat_calc_chksum16/32)
 *   balloc.c    the allocation bitmap, which is what says a cluster is in use
 *   fatent.c    cluster chains in the FAT, for files that aren't contiguous
 *   dir.c       entry sets: a File entry, a Stream extension, then the name in
 *               15-character Name entries, all covered by one checksum
 *
 * Files start out contiguous ("NoFatChain": no FAT entries at all); one that
 * can't grow in place gets a FAT chain written for it (exfat_chain_cont_cluster).
 * Valid data length is always kept equal to the size (gaps are zero-filled).
 * Times are written as local time without a time zone, like the OS clock.
 */

#include "fs/filesystems.h"
#include "fs/diskfs.h"
#include "block/block.h"
#include "drivers/rtc.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"

#define T_UNUSED   0x00
#define T_BITMAP   0x81
#define T_UPCASE   0x82
#define T_VOLUME   0x83
#define T_FILE     0x85
#define T_STREAM   0xC0
#define T_NAME     0xC1
#define IN_USE     0x80

#define A_RO       0x0001
#define A_DIR      0x0010
#define A_ARCH     0x0020

#define F_ALLOC    0x01
#define F_NOCHAIN  0x02

#define EOC        0xFFFFFFFFu
#define VOLUME_DIRTY 0x0002
#define ENTRY      32
#define MAX_SET    (2 + 17)   /* file + stream + up to 17 name entries (255 characters) */

struct exfat {
    struct mount *m;
    struct block_device *dev;
    uint32_t cluster_size;
    uint64_t fat_start;          /* bytes */
    uint64_t heap_start;         /* cluster 2, bytes */
    uint32_t clusters;
    uint32_t root_cluster;
    uint16_t vol_flags;
    bool was_dirty;
    uint16_t *upcase;            /* 65536 entries */
    uint8_t *bitmap;             /* in memory, written through */
    uint32_t bitmap_cluster;     /* first cluster of the bitmap (it's contiguous in practice; chained is followed) */
    uint32_t free_count;
    uint32_t next_free;
};

struct xnode {
    uint32_t cluster;            /* first cluster, 0 if none */
    bool contiguous;             /* NoFatChain */
    bool is_root;
    uint16_t attr;
    uint32_t slot;               /* the File entry's index in the parent */
    int set_len;                 /* entries in its set */
    uint32_t alloc_clusters;     /* clusters allocated (known for contiguous ones, else counted) */
    uint32_t cursor_index, cursor_cluster;
};

static struct exfat *X(struct dnode *n) {
    return n->m->fs;
}

static struct xnode *XN(struct dnode *n) {
    return n->priv;
}

static bool valid_cluster(struct exfat *x, uint32_t c) {
    return c >= 2 && c < x->clusters + 2;
}

static uint64_t cluster_pos(struct exfat *x, uint32_t c) {
    return x->heap_start + (uint64_t)(c - 2) * x->cluster_size;
}

static uint16_t upper(struct exfat *x, uint16_t c) {
    return x->upcase[c] ? x->upcase[c] : c;
}

/* misc.c exfat_calc_chksum16 (CS_DIR_ENTRY skips the checksum field itself) */
static uint16_t checksum16(const uint8_t *data, int len, uint16_t sum, bool skip_field) {
    for (int i = 0; i < len; i++) {
        if (skip_field && (i == 2 || i == 3)) {
            continue;
        }
        sum = (uint16_t)(((sum << 15) | (sum >> 1)) + data[i]);
    }
    return sum;
}

static uint32_t checksum32(const uint8_t *data, int len, uint32_t sum) {
    for (int i = 0; i < len; i++) {
        sum = ((sum << 31) | (sum >> 1)) + data[i];
    }
    return sum;
}

/* --- FAT and bitmap ---------------------------------------------------------------------------------------- */

static int fat_get(struct exfat *x, uint32_t c, uint32_t *out) {
    uint8_t b[4];
    int err = bio_read(x->dev, x->fat_start + (uint64_t)c * 4, b, 4);
    *out = get32(b);
    return err;
}

static int fat_set(struct exfat *x, uint32_t c, uint32_t v) {
    uint8_t b[4];
    put32(b, v);
    return bio_write(x->dev, x->fat_start + (uint64_t)c * 4, b, 4);
}

static bool in_use(struct exfat *x, uint32_t c) {
    uint32_t i = c - 2;
    return x->bitmap[i / 8] & (1u << (i % 8));
}

/* Where byte `i` of the bitmap is on disk (the bitmap is a chain of clusters starting at bitmap_cluster). */
static int bitmap_pos(struct exfat *x, uint32_t byte, uint64_t *pos) {
    uint32_t c = x->bitmap_cluster;
    for (uint32_t k = byte / x->cluster_size; k > 0; k--) {
        uint32_t next;
        int err = fat_get(x, c, &next);
        if (err) {
            return err;
        }
        /* (A bitmap with no FAT chain is contiguous: FAT entries are then 0.) */
        c = valid_cluster(x, next) ? next : c + 1;
    }
    *pos = cluster_pos(x, c) + byte % x->cluster_size;
    return 0;
}

static int set_used(struct exfat *x, uint32_t c, bool used) {
    uint32_t i = c - 2;
    uint8_t bit = (uint8_t)(1u << (i % 8));
    if (used) {
        x->bitmap[i / 8] |= bit;
    } else {
        x->bitmap[i / 8] &= (uint8_t)~bit;
    }
    uint64_t pos;
    int err = bitmap_pos(x, i / 8, &pos);
    return err ? err : bio_write(x->dev, pos, &x->bitmap[i / 8], 1);
}

static int alloc_one(struct exfat *x, uint32_t want, uint32_t *out) {
    if (!x->free_count) {
        return -ENOSPC;
    }
    if (valid_cluster(x, want) && !in_use(x, want)) {
        *out = want;
    } else {
        uint32_t c = valid_cluster(x, x->next_free) ? x->next_free : 2;
        uint32_t tries;
        for (tries = 0; tries < x->clusters && in_use(x, c); tries++) {
            if (++c >= x->clusters + 2) {
                c = 2;
            }
        }
        if (tries == x->clusters) {
            x->free_count = 0;
            return -ENOSPC;
        }
        *out = c;
    }
    int err = set_used(x, *out, true);
    if (err) {
        return err;
    }
    x->free_count--;
    x->next_free = *out + 1;
    return 0;
}

static int zero_cluster(struct exfat *x, uint32_t c) {
    static uint8_t zeros[4096];
    for (uint32_t at = 0; at < x->cluster_size; at += sizeof(zeros)) {
        uint32_t n = x->cluster_size - at < sizeof(zeros) ? x->cluster_size - at : sizeof(zeros);
        int err = bio_write(x->dev, cluster_pos(x, c) + at, zeros, n);
        if (err) {
            return err;
        }
    }
    return 0;
}

/* --- cluster chains ---------------------------------------------------------------------------------------- */

static int nth_cluster(struct exfat *x, struct xnode *xn, uint32_t index, uint32_t *out) {
    *out = 0;
    if (!xn->cluster) {
        return 0;
    }
    if (xn->contiguous) {
        if (index < xn->alloc_clusters) {
            *out = xn->cluster + index;
        }
        return 0;
    }
    uint32_t c = xn->cluster, i = 0;
    if (xn->cursor_cluster && xn->cursor_index <= index) {
        c = xn->cursor_cluster;
        i = xn->cursor_index;
    }
    while (i < index) {
        uint32_t next;
        int err = fat_get(x, c, &next);
        if (err) {
            return err;
        }
        if (!valid_cluster(x, next)) {
            return 0;
        }
        c = next;
        i++;
    }
    xn->cursor_index = index;
    xn->cursor_cluster = c;
    *out = c;
    return 0;
}

static int count_chain(struct exfat *x, struct xnode *xn) {
    uint32_t n = 0;
    for (uint32_t c = xn->cluster; valid_cluster(x, c) && n <= x->clusters; n++) {
        uint32_t next;
        int err = fat_get(x, c, &next);
        if (err) {
            return err;
        }
        if (next == EOC || !valid_cluster(x, next)) {
            n++;
            break;
        }
        c = next;
    }
    xn->alloc_clusters = n;
    return 0;
}

/* exfat_chain_cont_cluster: a contiguous file is about to stop being contiguous: write its FAT chain. */
static int make_chained(struct exfat *x, struct xnode *xn) {
    for (uint32_t i = 0; i < xn->alloc_clusters; i++) {
        int err = fat_set(x, xn->cluster + i, i + 1 < xn->alloc_clusters ? xn->cluster + i + 1 : EOC);
        if (err) {
            return err;
        }
    }
    xn->contiguous = false;
    xn->cursor_cluster = xn->cursor_index = 0;
    return 0;
}

/* Grow to `count` clusters (zeroed if `zero`), staying contiguous while the next cluster is free. */
static int grow(struct exfat *x, struct xnode *xn, uint32_t count, bool zero) {
    while (xn->alloc_clusters < count) {
        uint32_t last = 0;
        if (xn->cluster) {
            int err = nth_cluster(x, xn, xn->alloc_clusters - 1, &last);
            if (err) {
                return err;
            }
        }
        uint32_t c;
        int err = alloc_one(x, last ? last + 1 : 0, &c);
        if (err) {
            return err;
        }
        if (zero && (err = zero_cluster(x, c)) != 0) {
            return err;
        }
        if (!xn->cluster) {
            xn->cluster = c;
            xn->contiguous = true;
        } else if (xn->contiguous && c != last + 1) {
            err = make_chained(x, xn);
            if (!err) {
                err = fat_set(x, last, c);
            }
        } else if (!xn->contiguous) {
            err = fat_set(x, last, c);
        }
        if (!err && !xn->contiguous) {
            err = fat_set(x, c, EOC);
        }
        if (err) {
            return err;
        }
        xn->alloc_clusters++;
    }
    return 0;
}

/* Give back everything from cluster index `keep` on. */
static int shrink(struct exfat *x, struct xnode *xn, uint32_t keep) {
    if (keep >= xn->alloc_clusters) {
        return 0;
    }
    uint32_t c;
    int err = nth_cluster(x, xn, keep, &c);
    if (err) {
        return err;
    }
    if (keep && !xn->contiguous) {
        uint32_t last;
        err = nth_cluster(x, xn, keep - 1, &last);
        if (!err) {
            err = fat_set(x, last, EOC);
        }
        if (err) {
            return err;
        }
    }
    for (uint32_t i = keep; i < xn->alloc_clusters && valid_cluster(x, c); i++) {
        uint32_t next = c + 1;
        if (!xn->contiguous && (err = fat_get(x, c, &next)) != 0) {
            return err;
        }
        if ((err = set_used(x, c, false)) != 0) {
            return err;
        }
        x->free_count++;
        if (c < x->next_free) {
            x->next_free = c;
        }
        c = next;
    }
    xn->alloc_clusters = keep;
    xn->cursor_cluster = xn->cursor_index = 0;
    if (!keep) {
        xn->cluster = 0;
        xn->contiguous = false;
    }
    return 0;
}

static int64_t chain_io(struct exfat *x, struct xnode *xn, uint64_t offset, uint8_t *buf, size_t len, bool write) {
    size_t done = 0;
    while (done < len) {
        uint32_t index = (uint32_t)((offset + done) / x->cluster_size);
        uint32_t within = (uint32_t)((offset + done) % x->cluster_size);
        uint32_t c;
        int err = nth_cluster(x, xn, index, &c);
        if (err) {
            return err;
        }
        if (!c) {
            return -EIO;
        }
        size_t span = x->cluster_size - within;
        if (xn->contiguous) {
            /* All the rest is one run. */
            uint64_t left = (uint64_t)(xn->alloc_clusters - index) * x->cluster_size - within;
            span = left < (1u << 20) ? (size_t)left : (1u << 20);
        } else {
            uint32_t run_end = c;
            while (done + span < len && span < (1u << 20)) {
                uint32_t next;
                if (fat_get(x, run_end, &next) || next != run_end + 1) {
                    break;
                }
                run_end = next;
                span += x->cluster_size;
                xn->cursor_index++;
                xn->cursor_cluster = run_end;
            }
        }
        size_t n = len - done < span ? len - done : span;
        uint64_t pos = cluster_pos(x, c) + within;
        err = write ? bio_write(x->dev, pos, buf + done, n) : bio_read(x->dev, pos, buf + done, n);
        if (err) {
            return err;
        }
        done += n;
    }
    return (int64_t)done;
}

/* --- directory entries --------------------------------------------------------------------------------------- */

static int slot_pos(struct exfat *x, struct xnode *dir, uint32_t slot, uint64_t *pos) {
    uint64_t byte = (uint64_t)slot * ENTRY;
    uint32_t c;
    int err = nth_cluster(x, dir, (uint32_t)(byte / x->cluster_size), &c);
    *pos = c ? cluster_pos(x, c) + byte % x->cluster_size : 0;
    return err;
}

static int read_slot(struct exfat *x, struct xnode *dir, uint32_t slot, uint8_t *e, bool *end) {
    uint64_t pos;
    int err = slot_pos(x, dir, slot, &pos);
    if (err) {
        return err;
    }
    *end = !pos;
    return pos ? bio_read(x->dev, pos, e, ENTRY) : 0;
}

static int write_slot(struct exfat *x, struct xnode *dir, uint32_t slot, const uint8_t *e) {
    uint64_t pos;
    int err = slot_pos(x, dir, slot, &pos);
    if (err) {
        return err;
    }
    return pos ? bio_write(x->dev, pos, e, ENTRY) : -EIO;
}

static uint64_t ex_time(uint16_t time, uint16_t date, uint8_t cs) {
    if (!date) {
        return 0;
    }
    struct dtime t = { 1980 + (date >> 9), (date >> 5) & 15, date & 31, time >> 11, (time >> 5) & 63,
                       (time & 31) * 2 + cs / 100 };
    return dtime_to_unix(&t);
}

static void to_ex_time(uint64_t unix_time, uint16_t *time, uint16_t *date, uint8_t *cs) {
    struct dtime t;
    dtime_from_unix(unix_time, &t);
    if (t.year < 1980) {
        t.year = 1980;
        t.month = t.day = 1;
        t.hour = t.minute = t.second = 0;
    }
    *date = (uint16_t)((t.year - 1980) << 9 | t.month << 5 | t.day);
    *time = (uint16_t)(t.hour << 11 | t.minute << 5 | t.second / 2);
    *cs = (uint8_t)(t.second % 2 * 100);
}

static void set_checksum(uint8_t *set, int count) {
    uint16_t sum = checksum16(set, ENTRY, 0, true);
    for (int i = 1; i < count; i++) {
        sum = checksum16(set + i * ENTRY, ENTRY, sum, false);
    }
    put16(set + 2, sum);
}

static int read_set(struct exfat *x, struct xnode *dir, uint32_t slot, int count, uint8_t *set) {
    for (int i = 0; i < count; i++) {
        bool end;
        int err = read_slot(x, dir, slot + (uint32_t)i, set + i * ENTRY, &end);
        if (err || end) {
            return err ? err : -EIO;
        }
    }
    return 0;
}

static int write_set(struct exfat *x, struct xnode *dir, uint32_t slot, int count, const uint8_t *set) {
    for (int i = 0; i < count; i++) {
        int err = write_slot(x, dir, slot + (uint32_t)i, set + i * ENTRY);
        if (err) {
            return err;
        }
    }
    return 0;
}

static struct xnode *new_xnode(void) {
    return kzalloc(sizeof(struct xnode));
}

static int ex_list(struct dnode *dir) {
    struct exfat *x = X(dir);
    struct xnode *d = XN(dir);
    uint8_t set[MAX_SET * ENTRY];
    for (uint32_t slot = 0; slot < (1u << 23);) {
        bool end;
        int err = read_slot(x, d, slot, set, &end);
        if (err) {
            return err;
        }
        if (end || set[0] == T_UNUSED) {
            break;
        }
        if (set[0] != T_FILE) {
            slot++;
            continue;
        }
        int count = 1 + set[1];
        if (count < 3 || count > MAX_SET) {
            slot++;
            continue;
        }
        err = read_set(x, d, slot, count, set);
        if (err) {
            return err;
        }
        const uint8_t *st = set + ENTRY;
        if (st[0] != T_STREAM) {
            slot++;
            continue;
        }
        uint16_t sum = get16(set + 2);
        uint8_t copy[MAX_SET * ENTRY];
        memcpy(copy, set, (size_t)count * ENTRY);
        set_checksum(copy, count);
        if (get16(copy + 2) != sum) {
            slot += (uint32_t)count;   /* a damaged set: skip it */
            continue;
        }
        int name_len = st[3];
        uint16_t name[256];
        int got = 0;
        for (int i = 2; i < count && got < name_len; i++) {
            const uint8_t *ne = set + i * ENTRY;
            if (ne[0] != T_NAME) {
                break;
            }
            for (int k = 0; k < 15 && got < name_len; k++) {
                name[got++] = get16(ne + 2 + 2 * k);
            }
        }
        name[got] = 0;
        char utf8[VFS_NAME_MAX];
        utf16_to_utf8(name, (size_t)got, utf8, sizeof(utf8));
        uint16_t attr = get16(set + 4);
        struct xnode *xn = new_xnode();
        if (!xn) {
            return -ENOMEM;
        }
        xn->attr = attr;
        xn->slot = slot;
        xn->set_len = count;
        xn->cluster = get32(st + 20);
        xn->contiguous = st[1] & F_NOCHAIN;
        uint64_t size = get64(st + 24);
        if (!valid_cluster(x, xn->cluster)) {
            xn->cluster = 0;
        }
        if (xn->cluster) {
            if (xn->contiguous) {
                xn->alloc_clusters = (uint32_t)((size + x->cluster_size - 1) / x->cluster_size);
            } else if ((err = count_chain(x, xn)) != 0) {
                kfree(xn);
                return err;
            }
        }
        bool is_dir = attr & A_DIR;
        /* (Valid data length can be less than the size: past it reads as zeros. We read what's there,
           which for a file written by us is the same.) */
        struct dnode *n = diskfs_new(dir->m, utf8, is_dir ? VNODE_DIR : VNODE_FILE, size, xn);
        if (!n) {
            kfree(xn);
            return -ENOMEM;
        }
        n->v.ctime = ex_time(get16(set + 8), get16(set + 10), set[20]);
        n->v.mtime = ex_time(get16(set + 12), get16(set + 14), set[21]);
        if ((attr & A_RO) && !dir->m->read_only) {
            n->v.mode &= ~0222u;
        }
        diskfs_add(dir, n);
        slot += (uint32_t)count;
    }
    return 0;
}

/* Rewrite a node's entry set from what we know (cluster, size, flags, time). */
static int update_set(struct dnode *n) {
    struct exfat *x = X(n);
    struct xnode *xn = XN(n);
    if (xn->is_root) {
        return 0;
    }
    struct xnode *dir = XN((struct dnode *)n->v.parent);
    uint8_t set[MAX_SET * ENTRY];
    int err = read_set(x, dir, xn->slot, xn->set_len, set);
    if (err) {
        return err;
    }
    uint8_t *st = set + ENTRY;
    uint64_t size = n->v.type == VNODE_DIR ? (uint64_t)xn->alloc_clusters * x->cluster_size : n->v.size;
    st[1] = (uint8_t)((xn->cluster ? F_ALLOC : 0) | (xn->cluster && xn->contiguous ? F_NOCHAIN : 0));
    put32(st + 20, xn->cluster);
    put64(st + 8, size);    /* valid data length */
    put64(st + 24, size);
    uint16_t t, d;
    uint8_t cs;
    to_ex_time(n->v.mtime ? n->v.mtime : rtc_time(), &t, &d, &cs);
    put16(set + 12, t);
    put16(set + 14, d);
    set[21] = cs;
    set[23] = 0;   /* modify time zone: not given */
    put16(set + 16, t);
    put16(set + 18, d);
    set[24] = 0;
    if (n->v.type == VNODE_FILE) {
        put16(set + 4, (uint16_t)(get16(set + 4) | A_ARCH));
    }
    set_checksum(set, xn->set_len);
    return write_set(x, dir, xn->slot, xn->set_len, set);
}

/* --- names ----------------------------------------------------------------------------------------------- */

static bool name_ok(const char *name) {
    static const char bad[] = "\"*/:<>?\\|";
    size_t len = strlen(name);
    if (!len || name[len - 1] == ' ') {
        return false;
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

/* A new entry set for `name`: File + Stream + Name entries, with `template` giving the File and Stream
   fields to keep (or NULL for fresh ones). */
static int build_set(struct exfat *x, const char *name, uint16_t attr, const uint8_t *template, uint8_t *set,
                     int *count) {
    uint16_t u[256];
    int len = utf8_to_utf16(name, u, 255);
    if (len <= 0) {
        return len == 0 ? -EINVAL : -ENAMETOOLONG;
    }
    int names = (len + 14) / 15;
    *count = 2 + names;
    memset(set, 0, (size_t)*count * ENTRY);
    if (template) {
        memcpy(set, template, 2 * ENTRY);
    } else {
        uint16_t t, d;
        uint8_t cs;
        to_ex_time(rtc_time(), &t, &d, &cs);
        put16(set + 4, attr);
        put16(set + 8, t);
        put16(set + 10, d);
        put16(set + 12, t);
        put16(set + 14, d);
        put16(set + 16, t);
        put16(set + 18, d);
        set[20] = set[21] = cs;
        set[ENTRY] = T_STREAM;
    }
    set[0] = T_FILE;
    set[1] = (uint8_t)(*count - 1);
    uint8_t *st = set + ENTRY;
    st[0] = T_STREAM;
    st[3] = (uint8_t)len;
    uint8_t up[512];
    for (int i = 0; i < len; i++) {
        put16(up + 2 * i, upper(x, u[i]));
    }
    put16(st + 4, checksum16(up, len * 2, 0, false));
    for (int k = 0; k < names; k++) {
        uint8_t *ne = set + (2 + k) * ENTRY;
        ne[0] = T_NAME;
        for (int i = 0; i < 15; i++) {
            int at = k * 15 + i;
            put16(ne + 2 + 2 * i, at < len ? u[at] : 0);
        }
    }
    set_checksum(set, *count);
    return 0;
}

/* `count` free entries in a row (growing the folder if needed); the first one's index. */
static int find_free(struct exfat *x, struct dnode *dirnode, int count, uint32_t *first) {
    struct xnode *dir = XN(dirnode);
    uint32_t total = dir->alloc_clusters * (x->cluster_size / ENTRY);
    int run = 0;
    uint32_t slot;
    for (slot = 0; slot < total; slot++) {
        uint8_t e[ENTRY];
        bool end;
        int err = read_slot(x, dir, slot, e, &end);
        if (err) {
            return err;
        }
        if (end) {
            break;
        }
        if (e[0] == T_UNUSED) {
            /* Everything from here on is free. */
            if (total - (slot - (uint32_t)run) >= (uint32_t)count) {
                *first = slot - (uint32_t)run;
                return 0;
            }
            break;
        }
        if (!(e[0] & IN_USE)) {
            if (++run == count) {
                *first = slot - (uint32_t)count + 1;
                return 0;
            }
        } else {
            run = 0;
        }
    }
    uint32_t start = slot - (uint32_t)run;
    uint32_t per = x->cluster_size / ENTRY;
    uint32_t need = (start + (uint32_t)count + per - 1) / per;
    int err = grow(x, dir, need, true);
    if (err) {
        return err;
    }
    /* The folder's own size changed (the root has none). */
    if (!dir->is_root && (err = update_set(dirnode)) != 0) {
        return err;
    }
    *first = start;
    return 0;
}

static int erase_set(struct exfat *x, struct xnode *dir, struct xnode *xn) {
    for (int i = 0; i < xn->set_len; i++) {
        uint8_t e[ENTRY];
        bool end;
        int err = read_slot(x, dir, xn->slot + (uint32_t)i, e, &end);
        if (err || end) {
            return err ? err : -EIO;
        }
        e[0] &= (uint8_t)~IN_USE;
        if ((err = write_slot(x, dir, xn->slot + (uint32_t)i, e)) != 0) {
            return err;
        }
    }
    return 0;
}

/* --- operations ----------------------------------------------------------------------------------------- */

static int64_t ex_read(struct dnode *n, uint64_t offset, void *buf, size_t len) {
    return chain_io(X(n), XN(n), offset, buf, len, false);
}

static int64_t ex_write(struct dnode *n, uint64_t offset, const void *buf, size_t len) {
    struct exfat *x = X(n);
    struct xnode *xn = XN(n);
    if (!len) {
        return 0;
    }
    uint64_t end = offset + len;
    uint64_t need = (end + x->cluster_size - 1) / x->cluster_size;
    if (need > x->clusters) {
        return -ENOSPC;
    }
    bool gap = offset > n->v.size;
    int err = grow(x, xn, (uint32_t)need, gap);
    if (err) {
        update_set(n);
        return err;
    }
    if (gap) {
        uint64_t from = n->v.size, to = offset;
        uint64_t boundary = (from + x->cluster_size - 1) / x->cluster_size * x->cluster_size;
        if (to > boundary) {
            to = boundary;
        }
        static uint8_t zeros[4096];
        while (from < to) {
            size_t z = to - from < sizeof(zeros) ? (size_t)(to - from) : sizeof(zeros);
            int64_t r = chain_io(x, xn, from, zeros, z, true);
            if (r < 0) {
                return r;
            }
            from += z;
        }
    }
    int64_t r = chain_io(x, xn, offset, (uint8_t *)buf, len, true);
    if (r < 0) {
        return r;
    }
    if (end > n->v.size) {
        n->v.size = end;
    }
    n->v.mtime = rtc_time();
    err = update_set(n);
    return err ? err : r;
}

static int ex_truncate(struct dnode *n, uint64_t size) {
    struct exfat *x = X(n);
    struct xnode *xn = XN(n);
    if (size > n->v.size) {
        uint8_t zero = 0;
        int64_t r = ex_write(n, size - 1, &zero, 1);
        return r < 0 ? (int)r : 0;
    }
    uint32_t keep = (uint32_t)((size + x->cluster_size - 1) / x->cluster_size);
    n->v.size = size;
    n->v.mtime = rtc_time();
    if (keep == 0) {
        /* Detach first, then free: never a live entry pointing at free clusters. */
        struct xnode old = *xn;
        xn->cluster = 0;
        xn->alloc_clusters = 0;
        int err = update_set(n);
        if (err) {
            return err;
        }
        return shrink(x, &old, 0);
    }
    int err = shrink(x, xn, keep);
    return err ? err : update_set(n);
}

static int ex_create(struct dnode *dirnode, struct dnode *node) {
    struct exfat *x = X(dirnode);
    if (!name_ok(node->v.name)) {
        return -EINVAL;
    }
    struct xnode *xn = new_xnode();
    if (!xn) {
        return -ENOMEM;
    }
    node->priv = xn;
    bool dir = node->v.type == VNODE_DIR;
    xn->attr = dir ? A_DIR : A_ARCH;
    uint8_t set[MAX_SET * ENTRY];
    int count;
    int err = build_set(x, node->v.name, xn->attr, NULL, set, &count);
    if (err) {
        return err;
    }
    if (dir) {
        /* A folder gets one zeroed cluster (all entries unused). */
        if ((err = grow(x, xn, 1, true)) != 0) {
            return err;
        }
        uint8_t *st = set + ENTRY;
        st[1] = F_ALLOC | F_NOCHAIN;
        put32(st + 20, xn->cluster);
        put64(st + 8, x->cluster_size);
        put64(st + 24, x->cluster_size);
        set_checksum(set, count);
    }
    uint32_t first;
    err = find_free(x, dirnode, count, &first);
    if (!err) {
        err = write_set(x, XN(dirnode), first, count, set);
    }
    if (err) {
        shrink(x, xn, 0);
        return err;
    }
    xn->slot = first;
    xn->set_len = count;
    return 0;
}

static int ex_unlink(struct dnode *dirnode, struct dnode *child) {
    struct exfat *x = X(dirnode);
    struct xnode *xn = XN(child);
    int err = erase_set(x, XN(dirnode), xn);
    return err ? err : shrink(x, xn, 0);
}

static int ex_rename(struct dnode *dirnode, struct dnode *child, struct dnode *new_dir, const char *new_name) {
    struct exfat *x = X(dirnode);
    struct xnode *xn = XN(child);
    if (!name_ok(new_name)) {
        return -EINVAL;
    }
    uint8_t old_set[MAX_SET * ENTRY], set[MAX_SET * ENTRY];
    int err = read_set(x, XN(dirnode), xn->slot, xn->set_len, old_set);
    if (err) {
        return err;
    }
    int count;
    err = build_set(x, new_name, 0, old_set, set, &count);
    if (err) {
        return err;
    }
    /* Free the old set first, so the new name can reuse its place. */
    err = erase_set(x, XN(dirnode), xn);
    uint32_t first = 0;
    if (!err) {
        err = find_free(x, new_dir, count, &first);
    }
    if (!err) {
        err = write_set(x, XN(new_dir), first, count, set);
    }
    if (err) {
        write_set(x, XN(dirnode), xn->slot, xn->set_len, old_set);   /* put it back */
        return err;
    }
    xn->slot = first;
    xn->set_len = count;
    return 0;
}

static void ex_release(struct dnode *n) {
    kfree(n->priv);
}

static const struct diskfs_ops exfat_ops = {
    .list = ex_list,
    .read = ex_read,
    .write = ex_write,
    .truncate = ex_truncate,
    .create = ex_create,
    .unlink = ex_unlink,
    .rename = ex_rename,
    .release = ex_release,
};

/* --- mounting (super.c) ---------------------------------------------------------------------------------- */

static void set_dirty(struct exfat *x, bool dirty) {
    if (x->was_dirty || x->m->read_only) {
        return;
    }
    uint16_t flags = dirty ? (uint16_t)(x->vol_flags | VOLUME_DIRTY) : (uint16_t)(x->vol_flags & ~VOLUME_DIRTY);
    uint8_t b[2];
    put16(b, flags);
    /* (VolumeFlags is left out of the boot checksum, so the checksum sector needn't change.) */
    if (bio_write(x->dev, 106, b, 2) == 0) {
        x->vol_flags = flags;
    }
}

/* Read the folder's system entries: bitmap, upcase table, label. */
static int read_root_entries(struct exfat *x, struct xnode *root, char *label, size_t label_size) {
    uint32_t upcase_cluster = 0, upcase_sum = 0;
    uint64_t upcase_size = 0, bitmap_size = 0;
    for (uint32_t slot = 0; slot < 1024; slot++) {
        uint8_t e[ENTRY];
        bool end;
        int err = read_slot(x, root, slot, e, &end);
        if (err) {
            return err;
        }
        if (end || e[0] == T_UNUSED) {
            break;
        }
        if (e[0] == T_BITMAP && !(e[1] & 1)) {   /* the first bitmap (two only with two FATs) */
            x->bitmap_cluster = get32(e + 20);
            bitmap_size = get64(e + 24);
        } else if (e[0] == T_UPCASE) {
            upcase_sum = get32(e + 4);
            upcase_cluster = get32(e + 20);
            upcase_size = get64(e + 24);
        } else if (e[0] == T_VOLUME) {
            uint16_t u[11];
            int n = e[1] > 11 ? 11 : e[1];
            for (int i = 0; i < n; i++) {
                u[i] = get16(e + 2 + 2 * i);
            }
            utf16_to_utf8(u, (size_t)n, label, label_size);
        }
    }
    if (!valid_cluster(x, x->bitmap_cluster) || bitmap_size < (x->clusters + 7) / 8 ||
        !valid_cluster(x, upcase_cluster) || !upcase_size || upcase_size > 131072) {
        return -EINVAL;
    }

    /* The bitmap, whole, in memory. */
    uint32_t bytes = (x->clusters + 7) / 8;
    x->bitmap = kmalloc(bytes);
    if (!x->bitmap) {
        return -ENOMEM;
    }
    for (uint32_t at = 0; at < bytes;) {
        uint64_t pos;
        int err = bitmap_pos(x, at, &pos);
        uint32_t n = x->cluster_size - at % x->cluster_size;
        n = n > bytes - at ? bytes - at : n;
        if (!err) {
            err = bio_read(x->dev, pos, x->bitmap + at, n);
        }
        if (err) {
            return err;
        }
        at += n;
    }
    uint32_t used = 0;
    for (uint32_t c = 2; c < x->clusters + 2; c++) {
        used += in_use(x, c);
    }
    x->free_count = x->clusters - used;

    /* The upcase table (exfat_load_upcase_table): runs of identity mappings are compressed as
       0xFFFF, count. */
    uint8_t *raw = kmalloc((size_t)upcase_size);
    x->upcase = kzalloc(65536 * sizeof(uint16_t));
    if (!raw || !x->upcase) {
        kfree(raw);
        return -ENOMEM;
    }
    struct xnode up = { .cluster = upcase_cluster };
    int err = count_chain(x, &up);
    /* (It's usually written contiguous with no FAT chain: then the chain looks one cluster long.) */
    if (!err && (uint64_t)up.alloc_clusters * x->cluster_size < upcase_size) {
        up.contiguous = true;
        up.alloc_clusters = (uint32_t)((upcase_size + x->cluster_size - 1) / x->cluster_size);
    }
    int64_t r = err ? err : chain_io(x, &up, 0, raw, (size_t)upcase_size, false);
    if (r < 0) {
        kfree(raw);
        return (int)r;
    }
    if (checksum32(raw, (int)upcase_size, 0) != upcase_sum) {
        kprintf("[WARN] exFAT: upcase table checksum mismatch\n");
    }
    uint32_t index = 0;
    bool skip = false;
    for (uint64_t i = 0; i + 1 < upcase_size && index <= 0xFFFF; i += 2) {
        uint16_t u = get16(raw + i);
        if (skip) {
            index += u;
            skip = false;
        } else if (u == index) {
            index++;
        } else if (u == 0xFFFF) {
            skip = true;
        } else {
            x->upcase[index++] = u;
        }
    }
    kfree(raw);
    return 0;
}

static int ex_mount(struct mount *m) {
    uint8_t *b = kmalloc(512);
    if (!b) {
        return -ENOMEM;
    }
    int err = bio_read(m->dev, 0, b, 512);
    if (err || memcmp(b + 3, "EXFAT   ", 8) != 0 || b[510] != 0x55 || b[511] != 0xAA) {
        kfree(b);
        return err ? err : -EINVAL;
    }
    uint8_t ss_bits = b[108], spc_bits = b[109];
    uint64_t vol_length = get64(b + 72);
    struct exfat *x = kzalloc(sizeof(*x));
    if (!x || ss_bits < 9 || ss_bits > 12 || spc_bits > 25 - ss_bits || b[110] < 1 || b[110] > 2) {
        kfree(x);
        kfree(b);
        return x ? -EINVAL : -ENOMEM;
    }
    uint32_t ss = 1u << ss_bits;
    x->m = m;
    x->dev = m->dev;
    x->cluster_size = ss << spc_bits;
    x->fat_start = (uint64_t)get32(b + 80) * ss;
    x->heap_start = (uint64_t)get32(b + 88) * ss;
    x->clusters = get32(b + 92);
    x->root_cluster = get32(b + 96);
    x->vol_flags = get16(b + 106);
    uint32_t serial = get32(b + 100);
    ksnprintf(m->uuid, sizeof(m->uuid), "%04X-%04X", serial >> 16, serial & 0xFFFF);
    x->was_dirty = x->vol_flags & VOLUME_DIRTY;
    kfree(b);
    if (vol_length * ss > m->dev->sectors * m->dev->sector_size + (uint64_t)ss ||
        x->heap_start + (uint64_t)x->clusters * x->cluster_size > vol_length * ss || !valid_cluster(x, x->root_cluster)) {
        kfree(x);
        return -EINVAL;
    }
    m->fs = x;
    m->ops = &exfat_ops;
    m->case_insensitive = true;
    strlcpy(m->fs_name, "exFAT", sizeof(m->fs_name));

    struct xnode *root = new_xnode();
    if (!root) {
        kfree(x);
        return -ENOMEM;
    }
    root->is_root = true;
    root->cluster = x->root_cluster;
    if ((err = count_chain(x, root)) == 0) {
        err = read_root_entries(x, root, m->label, sizeof(m->label));
    }
    if (err) {
        kfree(x->bitmap);
        kfree(x->upcase);
        kfree(root);
        kfree(x);
        m->fs = NULL;
        return err;
    }
    x->next_free = 2;
    m->root = diskfs_new(m, "", VNODE_DIR, 0, root);
    if (!m->root) {
        return -ENOMEM;
    }
    if (!m->read_only && !m->dev->read_only) {
        set_dirty(x, true);
        bio_sync(m->dev);
    }
    return 0;
}

static void ex_unmount(struct mount *m) {
    struct exfat *x = m->fs;
    if (!m->dead && !m->read_only) {
        set_dirty(x, false);
    }
    kfree(x->bitmap);
    kfree(x->upcase);
    kfree(x);
    m->fs = NULL;
}

static int ex_statfs(struct mount *m, uint64_t *total, uint64_t *free) {
    struct exfat *x = m->fs;
    *total = (uint64_t)x->clusters * x->cluster_size;
    *free = (uint64_t)x->free_count * x->cluster_size;
    return 0;
}

const struct fs_driver exfat_driver = {
    .name = "exFAT",
    .mount = ex_mount,
    .unmount = ex_unmount,
    .statfs = ex_statfs,
};
