/*
 * NTFS, read-only. After Linux's fs/ntfs (the newer driver, layout.h for the
 * on-disk structures):
 *
 *   super.c     the boot sector: cluster, MFT record and index block sizes
 *   mst.c       multi-sector transfer fixups: every 512 bytes of an MFT record
 *               or index block ends in a sequence number, the real bytes are
 *               kept in the update sequence array (post_read_mst_fixup)
 *   runlist.c   mapping pairs: (length, signed offset) runs, none = sparse
 *   attrib.c    finding attributes in a record, attribute lists that spread a
 *               big file's runs over several records
 *   index.c     directory B-trees: $INDEX_ROOT in the record, $INDEX_ALLOCATION
 *               blocks below it, walked from the root through subnode pointers
 *   compress.c  LZNT1: 16-cluster compression units of 4 KiB sub-blocks
 *               (ntfs_decompress)
 *
 * Why read-only: NTFS keeps a journal ($LogFile) and Windows leaves a disk
 * "hibernated" with Fast Startup; writing without honouring either can
 * damage a Windows drive. Reading can't.
 * Not readable: encrypted (EFS) files, and Windows' own CompactOS-compressed
 * system files (WOF reparse points); both read as an error.
 */

#include "fs/filesystems.h"
#include "fs/diskfs.h"
#include "block/block.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"

#define AT_STANDARD_INFO   0x10
#define AT_ATTRIBUTE_LIST  0x20
#define AT_FILE_NAME       0x30
#define AT_VOLUME_NAME     0x60
#define AT_DATA            0x80
#define AT_INDEX_ROOT      0x90
#define AT_INDEX_ALLOC     0xA0
#define AT_REPARSE         0xC0
#define AT_END             0xFFFFFFFFu

#define ATTR_COMPRESSED    0x0001
#define ATTR_ENCRYPTED     0x4000
#define ATTR_SPARSE        0x8000

#define REC_IN_USE         0x0001
#define REC_DIRECTORY      0x0002

#define FILE_ATTR_HIDDEN   0x0002
#define FILE_ATTR_SYSTEM   0x0004
#define FILE_ATTR_REPARSE  0x0400

#define NS_DOS             2
#define IE_SUBNODE         0x01
#define IE_LAST            0x02

#define FIRST_USER_RECORD  24   /* 0..23 are the filesystem's own ($MFT, $Bitmap...) */
#define ROOT_RECORD        5
#define MAX_RECORD         4096

struct run {
    uint64_t vcn, lcn, len;      /* lcn = UINT64_MAX: sparse */
};

/* An attribute's value: resident bytes, or runs on disk. */
struct nattr {
    bool present;
    bool resident;
    uint16_t flags;
    uint8_t *value;              /* resident */
    uint32_t value_len;
    struct run *runs;
    int run_count;
    uint64_t data_size, init_size;
    uint32_t unit_clusters;      /* compression unit, in clusters (compressed only) */
};

struct ntfs {
    struct mount *m;
    struct block_device *dev;
    uint32_t cluster_size;
    uint32_t record_size;
    uint32_t index_size;         /* default index block size (each $INDEX_ROOT says its own) */
    uint64_t clusters;
    struct nattr mft;            /* $MFT's $DATA: where every record is */
    uint64_t free_clusters;
    bool free_known;
};

struct nnode {
    uint64_t record;
    struct nattr data;           /* files: unnamed $DATA, loaded on the first read */
    bool data_loaded;
    /* The last decompressed compression unit, for sequential reads of compressed files. */
    uint8_t *unit_buf;
    uint64_t unit_vcn;
    bool unit_valid;
};

static struct ntfs *N(struct dnode *n) {
    return n->m->fs;
}

static void free_attr(struct nattr *a) {
    kfree(a->value);
    kfree(a->runs);
    memset(a, 0, sizeof(*a));
}

static uint64_t nt_time(uint64_t t) {
    const uint64_t epoch = 116444736000000000ull;   /* 1601 -> 1970 in 100 ns */
    return t > epoch ? (t - epoch) / 10000000ull : 0;
}

/* --- reading runs ------------------------------------------------------------------------------------ */

/* runlist.c ntfs_mapping_pairs_decompress: append the runs of a non-resident attribute. */
static int decode_runs(const uint8_t *attr, uint32_t attr_len, struct nattr *a) {
    uint64_t vcn = get64(attr + 16);
    uint32_t at = get16(attr + 32);
    int64_t lcn = 0;
    while (at < attr_len && attr[at]) {
        uint8_t h = attr[at];
        int lb = h & 0x0F, ob = h >> 4;
        if (lb == 0 || lb > 8 || ob > 8 || at + 1 + lb + ob > attr_len) {
            return -EIO;
        }
        uint64_t len = 0;
        for (int i = lb - 1; i >= 0; i--) {
            len = len << 8 | attr[at + 1 + i];
        }
        int64_t delta = 0;
        if (ob) {
            for (int i = ob - 1; i >= 0; i--) {
                delta = (int64_t)((uint64_t)delta << 8 | attr[at + 1 + lb + i]);
            }
            if (attr[at + lb + ob] & 0x80) {   /* sign-extend */
                delta -= (int64_t)1 << (8 * ob);
            }
            lcn += delta;
        }
        struct run *grown = krealloc(a->runs, (size_t)(a->run_count + 1) * sizeof(struct run));
        if (!grown) {
            return -ENOMEM;
        }
        a->runs = grown;
        a->runs[a->run_count].vcn = vcn;
        a->runs[a->run_count].lcn = ob ? (uint64_t)lcn : UINT64_MAX;
        a->runs[a->run_count].len = len;
        a->run_count++;
        vcn += len;
        at += 1 + (uint32_t)lb + (uint32_t)ob;
    }
    return 0;
}

/* Where virtual cluster `vcn` is: its LCN (UINT64_MAX if sparse) and how many clusters follow in that run. */
static bool map_vcn(const struct nattr *a, uint64_t vcn, uint64_t *lcn, uint64_t *left) {
    for (int i = 0; i < a->run_count; i++) {
        const struct run *r = &a->runs[i];
        if (vcn >= r->vcn && vcn < r->vcn + r->len) {
            *lcn = r->lcn == UINT64_MAX ? UINT64_MAX : r->lcn + (vcn - r->vcn);
            *left = r->vcn + r->len - vcn;
            return true;
        }
    }
    return false;
}

/* Raw (not decompressed) bytes of a non-resident attribute. Sparse and unmapped parts read as zeros. */
static int read_runs(struct ntfs *nt, const struct nattr *a, uint64_t offset, uint8_t *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        uint64_t pos = offset + done;
        uint64_t vcn = pos / nt->cluster_size;
        uint32_t within = (uint32_t)(pos % nt->cluster_size);
        uint64_t lcn, left;
        size_t n;
        if (!map_vcn(a, vcn, &lcn, &left)) {
            left = 1;
            lcn = UINT64_MAX;
        }
        uint64_t span = left * nt->cluster_size - within;
        n = len - done < span ? len - done : (size_t)span;
        if (lcn == UINT64_MAX) {
            memset(buf + done, 0, n);
        } else {
            if (lcn + left > nt->clusters) {
                return -EIO;
            }
            int err = bio_read(nt->dev, lcn * nt->cluster_size + within, buf + done, n);
            if (err) {
                return err;
            }
        }
        done += n;
    }
    return 0;
}

/* --- records ------------------------------------------------------------------------------------------- */

/* mst.c post_read_mst_fixup */
static int fixup(uint8_t *buf, uint32_t size, const char *magic) {
    if (memcmp(buf, magic, 4) != 0) {
        return -EIO;
    }
    uint16_t usa_ofs = get16(buf + 4), usa_count = get16(buf + 6);
    if (usa_count < 2 || (uint32_t)(usa_count - 1) * 512 > size || usa_ofs + usa_count * 2u > size) {
        return -EIO;
    }
    uint16_t usn = get16(buf + usa_ofs);
    for (uint32_t i = 1; i < usa_count; i++) {
        uint8_t *end = buf + i * 512 - 2;
        if (get16(end) != usn) {
            return -EIO;   /* a torn write */
        }
        memcpy(end, buf + usa_ofs + 2 * i, 2);
    }
    return 0;
}

static int read_record(struct ntfs *nt, uint64_t record, uint8_t *buf) {
    int err = read_runs(nt, &nt->mft, record * nt->record_size, buf, nt->record_size);
    if (!err) {
        err = fixup(buf, nt->record_size, "FILE");
    }
    if (!err && !(get16(buf + 22) & REC_IN_USE)) {
        err = -ENOENT;
    }
    return err;
}

/* The next attribute after `at` in a record (0: start); returns its offset, 0 at the end. */
static uint32_t next_attr(const uint8_t *rec, uint32_t size, uint32_t at) {
    at = at ? at + get32(rec + at + 4) : get16(rec + 20);
    if (at + 8 > size || get32(rec + at) == AT_END) {
        return 0;
    }
    uint32_t len = get32(rec + at + 4);
    if (len < 16 || at + len > size) {
        return 0;
    }
    return at;
}

static bool attr_name_is(const uint8_t *attr, const uint16_t *name, int name_len) {
    if (attr[9] != name_len) {
        return false;
    }
    const uint8_t *p = attr + get16(attr + 10);
    for (int i = 0; i < name_len; i++) {
        if (get16(p + 2 * i) != name[i]) {
            return false;
        }
    }
    return true;
}

/* Take one attribute instance into `a` (resident value, or more runs). */
static int take_attr(const uint8_t *attr, struct nattr *a) {
    uint32_t len = get32(attr + 4);
    bool first = !a->present;
    a->present = true;
    a->flags = get16(attr + 12);
    if (!attr[8]) {
        uint32_t vlen = get32(attr + 16), voff = get16(attr + 20);
        if (voff + vlen > len) {
            return -EIO;
        }
        a->resident = true;
        a->value = kmalloc(vlen ? vlen : 1);
        if (!a->value) {
            return -ENOMEM;
        }
        memcpy(a->value, attr + voff, vlen);
        a->value_len = vlen;
        a->data_size = a->init_size = vlen;
        return 0;
    }
    if (first || get64(attr + 16) == 0) {
        /* The first instance (lowest VCN 0) has the sizes. */
        a->data_size = get64(attr + 48);
        a->init_size = get64(attr + 56);
        a->unit_clusters = attr[34] ? 1u << attr[34] : 0;
    }
    return decode_runs(attr, len, a);
}

/* attrib.c: find attribute `type` named `name` of a record, following an attribute list into other
   records. */
static int load_attr(struct ntfs *nt, uint64_t record, uint32_t type, const uint16_t *name, int name_len,
                     struct nattr *a) {
    memset(a, 0, sizeof(*a));
    uint8_t *rec = kmalloc(nt->record_size);
    if (!rec) {
        return -ENOMEM;
    }
    int err = read_record(nt, record, rec);
    if (err) {
        kfree(rec);
        return err;
    }
    struct nattr list = { 0 };
    for (uint32_t at = 0; (at = next_attr(rec, nt->record_size, at)) != 0;) {
        uint32_t t = get32(rec + at);
        if (t == AT_ATTRIBUTE_LIST && !list.present) {
            err = take_attr(rec + at, &list);
        } else if (t == type && attr_name_is(rec + at, name, name_len) && !a->present) {
            err = take_attr(rec + at, a);
        }
        if (err) {
            break;
        }
    }
    if (!err && list.present && !(a->present && a->resident)) {
        /* The attribute list: entries (type, length, name, lowest VCN, record) for every attribute. */
        uint8_t *lv = list.value;
        uint32_t llen = list.value_len;
        uint8_t *heap_list = NULL;
        if (!list.resident) {
            llen = (uint32_t)(list.data_size < (1u << 20) ? list.data_size : (1u << 20));
            heap_list = kmalloc(llen ? llen : 1);
            if (!heap_list || read_runs(nt, &list, 0, heap_list, llen)) {
                err = heap_list ? -EIO : -ENOMEM;
            }
            lv = heap_list;
        }
        uint8_t *other = err ? NULL : kmalloc(nt->record_size);
        bool fresh = !a->present;
        for (uint32_t at = 0; !err && other && at + 26 <= llen;) {
            uint32_t t = get32(lv + at);
            uint16_t elen = get16(lv + at + 4);
            if (elen < 26) {
                break;
            }
            uint64_t ref = get64(lv + at + 16) & 0xFFFFFFFFFFFFull;
            bool named_ok = lv[at + 6] == name_len;
            for (int i = 0; named_ok && i < name_len; i++) {
                named_ok = get16(lv + at + lv[at + 7] + 2 * i) == name[i];
            }
            if (t == type && named_ok && ref != record) {
                /* An extension record holds (part of) it. */
                uint64_t lowest = get64(lv + at + 8);
                if (read_record(nt, ref, other) == 0) {
                    for (uint32_t x = 0; (x = next_attr(other, nt->record_size, x)) != 0;) {
                        if (get32(other + x) == type && attr_name_is(other + x, name, name_len) && other[x + 8] &&
                            get64(other + x + 16) == lowest) {
                            if (fresh || lowest != 0) {
                                err = take_attr(other + x, a);
                            }
                            break;
                        }
                    }
                }
            }
            at += elen;
        }
        kfree(other);
        kfree(heap_list);
    }
    free_attr(&list);
    kfree(rec);
    return err;
}

/* --- LZNT1 (compress.c) ---------------------------------------------------------------------------------- */

/* Decompress one compression unit into `out` (out_size bytes, zero-filled past what decompresses). */
static void lznt1(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
    memset(out, 0, out_size);
    size_t ip = 0, op = 0;
    while (ip + 2 <= in_size && op < out_size) {
        uint16_t header = get16(in + ip);
        if (!header) {
            break;
        }
        size_t chunk_end = ip + (header & 0x0FFF) + 3;
        if (chunk_end > in_size) {
            break;
        }
        ip += 2;
        size_t sb_start = op, sb_end = op + 4096 < out_size ? op + 4096 : out_size;
        if (!(header & 0x8000)) {
            /* Stored as is. */
            size_t n = chunk_end - ip < sb_end - op ? chunk_end - ip : sb_end - op;
            memcpy(out + op, in + ip, n);
            op = sb_end;
            ip = chunk_end;
            continue;
        }
        while (ip < chunk_end && op < sb_end) {
            uint8_t tag = in[ip++];
            for (int t = 0; t < 8 && ip < chunk_end && op < sb_end; t++, tag >>= 1) {
                if (!(tag & 1)) {
                    out[op++] = in[ip++];
                    continue;
                }
                if (ip + 2 > chunk_end || op == sb_start) {
                    ip = chunk_end;
                    break;
                }
                int lg = 0;
                for (size_t i = op - sb_start - 1; i >= 0x10; i >>= 1) {
                    lg++;
                }
                uint16_t pt = get16(in + ip);
                ip += 2;
                size_t back = (size_t)(pt >> (12 - lg)) + 1;
                size_t length = (size_t)(pt & (0x0FFF >> lg)) + 3;
                if (back > op - sb_start) {
                    ip = chunk_end;
                    break;
                }
                for (size_t k = 0; k < length && op < sb_end; k++, op++) {
                    out[op] = out[op - back];   /* (overlapping copies repeat, as intended) */
                }
            }
        }
        op = sb_end;   /* a short sub-block is padded with zeros */
        ip = chunk_end;
    }
}

/* Read a compressed attribute: per compression unit, stored raw, compressed, or all sparse. */
static int read_compressed(struct ntfs *nt, struct nnode *nn, uint64_t offset, uint8_t *buf, size_t len) {
    struct nattr *a = &nn->data;
    uint32_t unit_bytes = a->unit_clusters * nt->cluster_size;
    if (!nn->unit_buf && !(nn->unit_buf = kmalloc(unit_bytes))) {
        return -ENOMEM;
    }
    size_t done = 0;
    while (done < len) {
        uint64_t pos = offset + done;
        uint64_t unit_vcn = pos / unit_bytes * a->unit_clusters;
        uint32_t within = (uint32_t)(pos % unit_bytes);
        if (!nn->unit_valid || nn->unit_vcn != unit_vcn) {
            /* How many clusters of this unit are really on disk? All: stored raw. None: zeros. */
            uint64_t on_disk = 0;
            for (uint64_t v = unit_vcn; v < unit_vcn + a->unit_clusters;) {
                uint64_t lcn, left;
                if (!map_vcn(a, v, &lcn, &left)) {
                    break;
                }
                uint64_t take = left < unit_vcn + a->unit_clusters - v ? left : unit_vcn + a->unit_clusters - v;
                if (lcn != UINT64_MAX) {
                    on_disk += take;
                }
                v += take;
            }
            int err;
            if (on_disk == a->unit_clusters || on_disk == 0) {
                err = read_runs(nt, a, unit_vcn * nt->cluster_size, nn->unit_buf, unit_bytes);
            } else {
                uint8_t *raw = kmalloc((size_t)on_disk * nt->cluster_size);
                err = raw ? read_runs(nt, a, unit_vcn * nt->cluster_size, raw, (size_t)on_disk * nt->cluster_size)
                          : -ENOMEM;
                if (!err) {
                    lznt1(raw, (size_t)on_disk * nt->cluster_size, nn->unit_buf, unit_bytes);
                }
                kfree(raw);
            }
            if (err) {
                return err;
            }
            nn->unit_vcn = unit_vcn;
            nn->unit_valid = true;
        }
        size_t n = unit_bytes - within < len - done ? unit_bytes - within : len - done;
        memcpy(buf + done, nn->unit_buf + within, n);
        done += n;
    }
    return 0;
}

/* --- files -------------------------------------------------------------------------------------------------- */

static int load_data(struct ntfs *nt, struct dnode *n) {
    struct nnode *nn = n->priv;
    if (nn->data_loaded) {
        return 0;
    }
    int err = load_attr(nt, nn->record, AT_DATA, NULL, 0, &nn->data);
    if (err) {
        return err;
    }
    nn->data_loaded = true;
    if (nn->data.present) {
        n->v.size = nn->data.data_size;
    }
    return 0;
}

static int64_t nt_read(struct dnode *n, uint64_t offset, void *buf, size_t len) {
    struct ntfs *nt = N(n);
    struct nnode *nn = n->priv;
    int err = load_data(nt, n);
    if (err) {
        return err;
    }
    struct nattr *a = &nn->data;
    if (!a->present) {
        return -EIO;   /* e.g. a CompactOS-compressed system file: its data is elsewhere */
    }
    if (a->flags & ATTR_ENCRYPTED) {
        return -EACCES;
    }
    if (offset >= a->data_size) {
        return 0;
    }
    if (len > a->data_size - offset) {
        len = (size_t)(a->data_size - offset);
    }
    uint8_t *out = buf;
    if (a->resident) {
        size_t have = offset < a->value_len ? a->value_len - (size_t)offset : 0;
        size_t c = len < have ? len : have;
        memcpy(out, a->value + offset, c);
        memset(out + c, 0, len - c);
        return (int64_t)len;
    }
    /* Past the initialized size there's nothing on disk yet: zeros. */
    size_t real = offset < a->init_size ? (size_t)(a->init_size - offset < len ? a->init_size - offset : len) : 0;
    if (real) {
        err = (a->flags & ATTR_COMPRESSED) && a->unit_clusters ? read_compressed(nt, nn, offset, out, real)
                                                              : read_runs(nt, a, offset, out, real);
        if (err) {
            return err;
        }
    }
    memset(out + real, 0, len - real);
    return (int64_t)len;
}

/* --- folders (index.c) ---------------------------------------------------------------------------------- */

struct walk {
    struct ntfs *nt;
    struct dnode *dir;
    struct nattr alloc;
    uint32_t block_size;
    uint8_t *block;
    int err;
};

static struct dnode *make_node(struct ntfs *nt, struct mount *m, uint64_t record, const char *name, int *err) {
    uint8_t *rec = kmalloc(nt->record_size);
    struct nnode *nn = kzalloc(sizeof(*nn));
    if (!rec || !nn) {
        kfree(rec);
        kfree(nn);
        *err = -ENOMEM;
        return NULL;
    }
    *err = read_record(nt, record, rec);
    if (*err) {
        kfree(rec);
        kfree(nn);
        if (*err == -ENOENT || *err == -EIO) {
            *err = 0;   /* a stale index entry or a damaged record: skip it */
        }
        return NULL;
    }
    bool dir = get16(rec + 22) & REC_DIRECTORY;
    bool have_data = false;
    uint64_t size = 0, mtime = 0, ctime = 0;
    for (uint32_t at = 0; (at = next_attr(rec, nt->record_size, at)) != 0;) {
        uint32_t t = get32(rec + at);
        if (t == AT_STANDARD_INFO && !rec[at + 8]) {
            const uint8_t *v = rec + at + get16(rec + at + 20);
            ctime = nt_time(get64(v));
            mtime = nt_time(get64(v + 8));
        } else if (t == AT_DATA && rec[at + 9] == 0) {
            if (!rec[at + 8]) {
                size = get32(rec + at + 16);
                have_data = true;
            } else if (get64(rec + at + 16) == 0) {
                size = get64(rec + at + 48);
                have_data = true;
            }
        }
    }
    kfree(rec);
    nn->record = record;
    if (!dir && !have_data) {
        /* $DATA is in an extension record (a very fragmented file): load it through the attribute list. */
        if (load_attr(nt, record, AT_DATA, NULL, 0, &nn->data) == 0 && nn->data.present) {
            size = nn->data.data_size;
            nn->data_loaded = true;
        } else {
            free_attr(&nn->data);
        }
    }
    struct dnode *n = diskfs_new(m, name, dir ? VNODE_DIR : VNODE_FILE, size, nn);
    if (!n) {
        kfree(nn);
        *err = -ENOMEM;
        return NULL;
    }
    n->v.mtime = mtime;
    n->v.ctime = ctime;
    return n;
}

/* One index entry list (in the root or a block): add the files, descend into subnodes. */
static void walk_entries(struct walk *w, const uint8_t *hdr, uint32_t avail, int depth);

static void walk_block(struct walk *w, uint64_t vcn, int depth) {
    if (depth > 16 || w->err) {
        return;
    }
    uint8_t *block = kmalloc(w->block_size);
    if (!block) {
        w->err = -ENOMEM;
        return;
    }
    /* The block's position: VCN in clusters, or in 512-byte units when blocks are smaller than a cluster. */
    uint64_t offset = w->block_size >= w->nt->cluster_size ? vcn * w->nt->cluster_size : vcn * 512;
    int err = read_runs(w->nt, &w->alloc, offset, block, w->block_size);
    if (!err) {
        err = fixup(block, w->block_size, "INDX");
    }
    if (!err) {
        walk_entries(w, block + 24, w->block_size - 24, depth + 1);
    }
    kfree(block);
}

static void walk_entries(struct walk *w, const uint8_t *hdr, uint32_t avail, int depth) {
    uint32_t first = get32(hdr), used = get32(hdr + 4);
    if (used > avail) {
        used = avail;
    }
    for (uint32_t at = first; at + 16 <= used && !w->err;) {
        const uint8_t *e = hdr + at;
        uint16_t len = get16(e + 8), key_len = get16(e + 10), flags = get16(e + 12);
        if (len < 16 || at + len > used) {
            break;
        }
        if (flags & IE_SUBNODE) {
            walk_block(w, get64(e + len - 8), depth);   /* smaller names first: keeps the order */
        }
        if (flags & IE_LAST) {
            break;
        }
        if (key_len >= 66) {
            const uint8_t *fn = e + 16;
            uint64_t record = get64(e) & 0xFFFFFFFFFFFFull;
            uint8_t name_len = fn[64], ns = fn[65];
            uint32_t attrs = get32(fn + 56);
            bool hidden_system = (attrs & FILE_ATTR_HIDDEN) && (attrs & FILE_ATTR_SYSTEM);
            if (ns != NS_DOS && record >= FIRST_USER_RECORD && 66u + name_len * 2u <= key_len && !hidden_system) {
                uint16_t u[256];
                for (int i = 0; i < name_len; i++) {
                    u[i] = get16(fn + 66 + 2 * i);
                }
                char name[VFS_NAME_MAX];
                utf16_to_utf8(u, name_len, name, sizeof(name));
                int err = 0;
                struct dnode *n = name[0] ? make_node(w->nt, w->dir->m, record, name, &err) : NULL;
                if (n) {
                    diskfs_add(w->dir, n);
                } else if (err) {
                    w->err = err;
                }
            }
        }
        at += len;
    }
}

static const uint16_t I30[4] = { '$', 'I', '3', '0' };

static int nt_list(struct dnode *dir) {
    struct ntfs *nt = N(dir);
    struct nnode *nn = dir->priv;
    struct nattr root;
    int err = load_attr(nt, nn->record, AT_INDEX_ROOT, I30, 4, &root);
    if (err || !root.present || !root.resident || root.value_len < 32) {
        free_attr(&root);
        return err ? err : 0;   /* (a folder without an index: a junction or a broken one; show it empty) */
    }
    struct walk w = { .nt = nt, .dir = dir };
    w.block_size = get32(root.value + 8);
    if (get32(root.value + 16 + 12) & 1) {   /* "large index": there are blocks below */
        err = load_attr(nt, nn->record, AT_INDEX_ALLOC, I30, 4, &w.alloc);
        if (err || w.block_size < 512 || w.block_size > 65536) {
            free_attr(&root);
            free_attr(&w.alloc);
            return err ? err : -EIO;
        }
    }
    walk_entries(&w, root.value + 16, root.value_len - 16, 0);
    free_attr(&root);
    free_attr(&w.alloc);
    return w.err;
}

static void nt_release(struct dnode *n) {
    struct nnode *nn = n->priv;
    free_attr(&nn->data);
    kfree(nn->unit_buf);
    kfree(nn);
}

static const struct diskfs_ops ntfs_ops = {
    .list = nt_list,
    .read = nt_read,
    .release = nt_release,
};

/* --- mounting (super.c) ---------------------------------------------------------------------------------- */

static int count_free(struct ntfs *nt) {
    struct nattr bitmap;
    int err = load_attr(nt, 6 /* $Bitmap */, AT_DATA, NULL, 0, &bitmap);
    if (err || !bitmap.present) {
        free_attr(&bitmap);
        return err ? err : -EIO;
    }
    static uint8_t chunk[65536];
    uint64_t used = 0, bytes = (nt->clusters + 7) / 8;
    for (uint64_t at = 0; at < bytes && !err; at += sizeof(chunk)) {
        size_t n = bytes - at < sizeof(chunk) ? (size_t)(bytes - at) : sizeof(chunk);
        if (bitmap.resident) {
            memcpy(chunk, bitmap.value + at, n < bitmap.value_len - at ? n : bitmap.value_len - at);
        } else {
            err = read_runs(nt, &bitmap, at, chunk, n);
        }
        for (size_t i = 0; i < n && !err; i++) {
            for (uint8_t v = chunk[i]; v; v &= (uint8_t)(v - 1)) {
                used++;   /* (no libgcc for __builtin_popcount) */
            }
        }
    }
    free_attr(&bitmap);
    if (!err) {
        nt->free_clusters = nt->clusters > used ? nt->clusters - used : 0;
        nt->free_known = true;
    }
    return err;
}

static uint32_t size_from_byte(int8_t v, uint32_t cluster_size) {
    /* Positive: clusters. Negative: 2^-v bytes. */
    return v > 0 ? (uint32_t)v * cluster_size : 1u << (-v);
}

static int nt_mount(struct mount *m) {
    uint8_t *b = kmalloc(512);
    if (!b) {
        return -ENOMEM;
    }
    int err = bio_read(m->dev, 0, b, 512);
    if (err || memcmp(b + 3, "NTFS    ", 8) != 0 || b[510] != 0x55 || b[511] != 0xAA) {
        kfree(b);
        return err ? err : -EINVAL;
    }
    uint32_t bps = get16(b + 11);
    uint32_t spc = b[13] <= 0x80 ? b[13] : 1u << (256 - b[13]);
    uint64_t total_sectors = get64(b + 40), mft_lcn = get64(b + 48);
    uint64_t serial = get64(b + 72);
    ksnprintf(m->uuid, sizeof(m->uuid), "%08X%08X", (uint32_t)(serial >> 32), (uint32_t)serial);
    struct ntfs *nt = kzalloc(sizeof(*nt));
    if (!nt || bps < 256 || bps > 4096 || (bps & (bps - 1)) || !spc) {
        kfree(nt);
        kfree(b);
        return nt ? -EINVAL : -ENOMEM;
    }
    nt->m = m;
    nt->dev = m->dev;
    nt->cluster_size = bps * spc;
    nt->record_size = size_from_byte((int8_t)b[64], nt->cluster_size);
    nt->index_size = size_from_byte((int8_t)b[68], nt->cluster_size);
    nt->clusters = total_sectors / spc;
    kfree(b);
    if (nt->record_size < 256 || nt->record_size > MAX_RECORD || mft_lcn >= nt->clusters ||
        total_sectors * bps > m->dev->sectors * m->dev->sector_size + bps) {
        kfree(nt);
        return -EINVAL;
    }

    /* Record 0 is $MFT itself: start with a run covering just it, then read its real runs. */
    struct run first = { 0, mft_lcn, (nt->record_size + nt->cluster_size - 1) / nt->cluster_size };
    nt->mft.present = true;
    nt->mft.runs = &first;
    nt->mft.run_count = 1;
    uint8_t *rec = kmalloc(nt->record_size);
    err = rec ? read_record(nt, 0, rec) : -ENOMEM;
    struct nattr mft = { 0 };
    for (uint32_t at = 0; !err && (at = next_attr(rec, nt->record_size, at)) != 0;) {
        if (get32(rec + at) == AT_DATA && rec[at + 8] && rec[at + 9] == 0 && get64(rec + at + 16) == 0) {
            err = take_attr(rec + at, &mft);
            break;
        }
    }
    kfree(rec);
    nt->mft.runs = NULL;
    if (err || !mft.present || mft.resident) {
        free_attr(&mft);
        kfree(nt);
        return err ? err : -EIO;
    }
    nt->mft = mft;
    /* A big MFT can have more runs in extension records (an attribute list): now that record 0 can be
       read, load $DATA properly. */
    struct nattr full;
    if (load_attr(nt, 0, AT_DATA, NULL, 0, &full) == 0 && full.present && !full.resident &&
        full.run_count > nt->mft.run_count) {
        free_attr(&nt->mft);
        nt->mft = full;
    } else {
        free_attr(&full);
    }

    m->fs = nt;
    m->ops = &ntfs_ops;
    m->read_only = true;
    m->case_insensitive = true;
    strlcpy(m->fs_name, "NTFS", sizeof(m->fs_name));

    struct nattr vol;
    if (load_attr(nt, 3 /* $Volume */, AT_VOLUME_NAME, NULL, 0, &vol) == 0 && vol.present && vol.resident) {
        uint16_t u[128];
        uint32_t n = vol.value_len / 2 > 127 ? 127 : vol.value_len / 2;
        for (uint32_t i = 0; i < n; i++) {
            u[i] = get16(vol.value + 2 * i);
        }
        utf16_to_utf8(u, n, m->label, sizeof(m->label));
    }
    free_attr(&vol);

    struct nnode *rn = kzalloc(sizeof(*rn));
    if (!rn) {
        free_attr(&nt->mft);
        kfree(nt);
        m->fs = NULL;
        return -ENOMEM;
    }
    rn->record = ROOT_RECORD;
    m->root = diskfs_new(m, "", VNODE_DIR, 0, rn);
    if (!m->root) {
        kfree(rn);
        free_attr(&nt->mft);
        kfree(nt);
        m->fs = NULL;
        return -ENOMEM;
    }
    return 0;
}

static void nt_unmount(struct mount *m) {
    struct ntfs *nt = m->fs;
    free_attr(&nt->mft);
    kfree(nt);
    m->fs = NULL;
}

static int nt_statfs(struct mount *m, uint64_t *total, uint64_t *free) {
    struct ntfs *nt = m->fs;
    if (!nt->free_known) {
        count_free(nt);
    }
    *total = nt->clusters * nt->cluster_size;
    *free = nt->free_known ? nt->free_clusters * nt->cluster_size : 0;
    return 0;
}

const struct fs_driver ntfs_driver = {
    .name = "NTFS",
    .mount = nt_mount,
    .unmount = nt_unmount,
    .statfs = nt_statfs,
};
