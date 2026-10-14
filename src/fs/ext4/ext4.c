/*
 * ext2/ext3/ext4, read-only. After Linux's fs/ext4:
 *
 *   super.c     the superblock and which features we can read (ext4_fill_super)
 *   balloc.c    group descriptors, META_BG placement (ext4_bg_has_super,
 *               descriptor_loc)
 *   inode.c     finding an inode in its group's table (__ext4_get_inode_loc),
 *               inline data, the old indirect block map (indirect.c)
 *   extents.c   extent trees: index nodes down to leaves (ext4_find_extent)
 *   dir.c       directory blocks read linearly, which works for hashed
 *               (htree) folders too: their index blocks look like empty entries
 *
 * Why read-only: writing needs the journal (jbd2) and metadata checksums kept
 * right; a mistake there damages a Linux system's disk. Reading can't.
 * Symbolic links show as small files holding the target path; device files,
 * pipes and sockets aren't shown.
 */

#include "fs/filesystems.h"
#include "fs/diskfs.h"
#include "block/block.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"

#define MAGIC              0xEF53
#define INC_FILETYPE       0x0002
#define INC_RECOVER        0x0004
#define INC_JOURNAL_DEV    0x0008
#define INC_META_BG        0x0010
#define INC_EXTENTS        0x0040
#define INC_64BIT          0x0080
#define INC_MMP            0x0100
#define INC_FLEX_BG        0x0200
#define INC_EA_INODE       0x0400
#define INC_DIRDATA        0x1000
#define INC_CSUM_SEED      0x2000
#define INC_LARGEDIR       0x4000
#define INC_INLINE_DATA    0x8000
#define INC_ENCRYPT        0x10000
#define INC_CASEFOLD       0x20000
#define INC_KNOWN (INC_FILETYPE | INC_RECOVER | INC_META_BG | INC_EXTENTS | INC_64BIT | INC_MMP | INC_FLEX_BG | \
                   INC_EA_INODE | INC_CSUM_SEED | INC_LARGEDIR | INC_INLINE_DATA | INC_ENCRYPT | INC_CASEFOLD)
#define RO_SPARSE_SUPER    0x0001

#define FL_INDEX           0x00001000
#define FL_EXTENTS         0x00080000
#define FL_INLINE_DATA     0x10000000
#define FL_ENCRYPT         0x00000800

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000

#define ROOT_INO 2

struct ext4 {
    struct mount *m;
    struct block_device *dev;
    uint32_t block_size;
    uint64_t blocks;
    uint64_t free_blocks;
    uint32_t first_data_block;
    uint32_t blocks_per_group, inodes_per_group;
    uint32_t inode_size, desc_size;
    uint32_t groups;
    uint32_t incompat, ro_compat, first_meta_bg;
    uint64_t *inode_tables;      /* per group: the inode table's first block */
};

struct enode {
    uint32_t ino;
    uint32_t flags;
    uint8_t iblock[60];          /* i_block: extent root, block map, inline data or a short link */
    /* The extent last found, for sequential reads. */
    uint64_t ext_first, ext_len, ext_start;
    bool ext_zero;
};

static struct ext4 *E(struct dnode *n) {
    return n->m->fs;
}

static struct enode *EN(struct dnode *n) {
    return n->priv;
}

static uint64_t block_pos(struct ext4 *e, uint64_t block) {
    return block * e->block_size;
}

/* --- inodes ----------------------------------------------------------------------------------------------- */

static int read_inode(struct ext4 *e, uint32_t ino, uint8_t *raw /* inode_size bytes */) {
    if (ino < 1) {
        return -EIO;
    }
    uint32_t group = (ino - 1) / e->inodes_per_group, index = (ino - 1) % e->inodes_per_group;
    if (group >= e->groups) {
        return -EIO;
    }
    uint64_t pos = block_pos(e, e->inode_tables[group]) + (uint64_t)index * e->inode_size;
    return bio_read(e->dev, pos, raw, e->inode_size);
}

static uint64_t inode_size_of(const uint8_t *raw) {
    return get32(raw + 4) | (uint64_t)get32(raw + 108) << 32;
}

/* --- mapping file blocks to disk blocks ---------------------------------------------------------------------- */

/* ext4_find_extent: walk the tree from the inode down to the leaf that covers `lblock`.
   Fills the extent (first logical block, length, first physical block); length 0: a hole. */
static int find_extent(struct ext4 *e, struct enode *en, uint64_t lblock) {
    uint8_t *node = en->iblock;
    uint8_t *buf = NULL;
    int err = 0;
    for (int level = 0; level < 8; level++) {
        if (get16(node) != 0xF30A) {
            err = -EIO;
            break;
        }
        int entries = get16(node + 2), depth = get16(node + 6);
        if (depth == 0) {
            /* A leaf: the last extent starting at or before lblock. */
            en->ext_len = 0;
            en->ext_first = lblock;
            uint64_t next_start = ~0ull;
            for (int i = 0; i < entries; i++) {
                const uint8_t *x = node + 12 + 12 * i;
                uint32_t first = get32(x);
                uint16_t len = get16(x + 4);
                bool unwritten = len > 32768;
                if (unwritten) {
                    len = (uint16_t)(len - 32768);
                }
                if (first <= lblock && lblock < (uint64_t)first + len) {
                    en->ext_first = first;
                    en->ext_len = len;
                    en->ext_start = get32(x + 8) | (uint64_t)get16(x + 6) << 32;
                    en->ext_zero = unwritten;   /* allocated but never written: reads as zeros */
                    break;
                }
                if (first > lblock && first < next_start) {
                    next_start = first;
                }
            }
            if (!en->ext_len) {
                /* A hole up to the next extent. */
                en->ext_len = next_start == ~0ull ? 1 : next_start - lblock;
                en->ext_start = 0;
                en->ext_zero = true;
            }
            break;
        }
        /* An index node: the last index whose first block is <= lblock. */
        int pick = -1;
        for (int i = 0; i < entries; i++) {
            if (get32(node + 12 + 12 * i) <= lblock) {
                pick = i;
            }
        }
        if (pick < 0) {
            pick = 0;
        }
        const uint8_t *ix = node + 12 + 12 * pick;
        uint64_t child = get32(ix + 4) | (uint64_t)get16(ix + 8) << 32;
        if (!buf && !(buf = kmalloc(e->block_size))) {
            err = -ENOMEM;
            break;
        }
        if ((err = bio_read(e->dev, block_pos(e, child), buf, e->block_size)) != 0) {
            break;
        }
        node = buf;
    }
    kfree(buf);
    return err;
}

/* The old ext2/3 map: 12 direct blocks, then single, double and triple indirect. */
static int map_indirect(struct ext4 *e, struct enode *en, uint64_t lblock, uint64_t *out) {
    uint32_t per = e->block_size / 4;
    if (lblock < 12) {
        *out = get32(en->iblock + 4 * lblock);
        return 0;
    }
    lblock -= 12;
    int levels;
    uint32_t slot;
    if (lblock < per) {
        levels = 1;
        slot = 12;
    } else if ((lblock -= per) < (uint64_t)per * per) {
        levels = 2;
        slot = 13;
    } else {
        lblock -= (uint64_t)per * per;
        levels = 3;
        slot = 14;
    }
    uint64_t block = get32(en->iblock + 4 * slot);
    for (int l = levels - 1; l >= 0 && block; l--) {
        uint64_t div = 1;
        for (int k = 0; k < l; k++) {
            div *= per;
        }
        uint32_t index = (uint32_t)(lblock / div % per);
        uint8_t b[4];
        int err = bio_read(e->dev, block_pos(e, block) + (uint64_t)index * 4, b, 4);
        if (err) {
            return err;
        }
        block = get32(b);
    }
    *out = block;
    return 0;
}

/* Read `len` bytes at `offset` of an inode's data (a run of blocks at a time). */
static int64_t read_data(struct ext4 *e, struct enode *en, uint64_t size, uint64_t offset, uint8_t *buf, size_t len) {
    if (offset >= size) {
        return 0;
    }
    if (len > size - offset) {
        len = (size_t)(size - offset);
    }
    if (en->flags & FL_INLINE_DATA) {
        /* Small files live in i_block (the rest, past 60 bytes, in an xattr we don't read). */
        size_t have = offset < 60 ? 60 - (size_t)offset : 0;
        size_t n = len < have ? len : have;
        memcpy(buf, en->iblock + offset, n);
        memset(buf + n, 0, len - n);
        return (int64_t)len;
    }
    size_t done = 0;
    while (done < len) {
        uint64_t pos = offset + done;
        uint64_t lblock = pos / e->block_size;
        uint32_t within = (uint32_t)(pos % e->block_size);
        uint64_t phys;
        uint64_t run;   /* blocks from lblock on that are contiguous on disk */
        bool zero;
        if (en->flags & FL_EXTENTS) {
            if (!(en->ext_len && lblock >= en->ext_first && lblock < en->ext_first + en->ext_len)) {
                int err = find_extent(e, en, lblock);
                if (err) {
                    return err;
                }
            }
            run = en->ext_first + en->ext_len - lblock;
            zero = en->ext_zero;
            phys = zero ? 0 : en->ext_start + (lblock - en->ext_first);
        } else {
            int err = map_indirect(e, en, lblock, &phys);
            if (err) {
                return err;
            }
            zero = phys == 0;
            run = 1;
        }
        uint64_t span = run * e->block_size - within;
        size_t n = len - done < span ? len - done : (size_t)span;
        if (zero) {
            memset(buf + done, 0, n);
        } else {
            if (phys + run > e->blocks) {
                return -EIO;
            }
            int err = bio_read(e->dev, block_pos(e, phys) + within, buf + done, n);
            if (err) {
                return err;
            }
        }
        done += n;
    }
    return (int64_t)done;
}

/* --- nodes ------------------------------------------------------------------------------------------------- */

static struct dnode *make_node(struct mount *m, struct ext4 *e, uint32_t ino, const char *name, int *err) {
    uint8_t *raw = kmalloc(e->inode_size);
    struct enode *en = kzalloc(sizeof(*en));
    if (!raw || !en) {
        kfree(raw);
        kfree(en);
        *err = -ENOMEM;
        return NULL;
    }
    *err = read_inode(e, ino, raw);
    if (*err) {
        kfree(raw);
        kfree(en);
        return NULL;
    }
    uint16_t mode = get16(raw);
    uint16_t type = mode & S_IFMT;
    if (type != S_IFREG && type != S_IFDIR && type != S_IFLNK) {
        kfree(raw);
        kfree(en);
        *err = 0;   /* not shown */
        return NULL;
    }
    en->ino = ino;
    en->flags = get32(raw + 32);
    memcpy(en->iblock, raw + 40, 60);
    uint64_t size = inode_size_of(raw);
    struct dnode *n = diskfs_new(m, name, type == S_IFDIR ? VNODE_DIR : VNODE_FILE, size, en);
    if (!n) {
        kfree(raw);
        kfree(en);
        *err = -ENOMEM;
        return NULL;
    }
    n->v.mode = mode & 0555;   /* (read-only: no write bits) */
    n->v.mtime = get32(raw + 16);
    n->v.ctime = e->inode_size > 128 && get16(raw + 128) >= 24 ? get32(raw + 0x90) : get32(raw + 12);
    kfree(raw);
    return n;
}

static int ext_list(struct dnode *dir) {
    struct ext4 *e = E(dir);
    struct enode *d = EN(dir);
    uint64_t size = dir->v.size;
    if (d->flags & FL_ENCRYPT) {
        return 0;   /* names are encrypted: nothing we can show */
    }
    uint8_t raw[160];
    if (!(d->flags & FL_INLINE_DATA)) {
        /* (A directory's size is in i_size, like a file's.) */
        uint8_t *ino_raw = kmalloc(e->inode_size);
        if (ino_raw && read_inode(e, d->ino, ino_raw) == 0) {
            size = inode_size_of(ino_raw);
        }
        kfree(ino_raw);
    }
    uint8_t *block = kmalloc(e->block_size);
    if (!block) {
        return -ENOMEM;
    }
    uint32_t bs = d->flags & FL_INLINE_DATA ? 60 : e->block_size;
    int err = 0;
    for (uint64_t at = 0; at < size && !err; at += bs) {
        int64_t r;
        if (d->flags & FL_INLINE_DATA) {
            /* Inline folder: the parent's inode number (4 bytes), then entries. */
            memcpy(raw, d->iblock, 60);
            r = 60;
        } else {
            r = read_data(e, d, size, at, block, bs);
        }
        if (r < 0) {
            err = (int)r;
            break;
        }
        const uint8_t *b = d->flags & FL_INLINE_DATA ? raw : block;
        uint32_t start = d->flags & FL_INLINE_DATA ? 4 : 0;
        for (uint32_t off = start; off + 8 <= (uint32_t)r;) {
            uint32_t ino = get32(b + off);
            uint16_t rec_len = get16(b + off + 4);
            uint8_t name_len = b[off + 6];
            if (rec_len < 8 || off + rec_len > (uint32_t)r || 8u + name_len > rec_len) {
                break;   /* damaged block: skip the rest of it */
            }
            if (ino && name_len && !(name_len == 1 && b[off + 8] == '.') &&
                !(name_len == 2 && b[off + 8] == '.' && b[off + 9] == '.')) {
                char name[VFS_NAME_MAX];
                memcpy(name, b + off + 8, name_len);
                name[name_len] = '\0';
                int e2 = 0;
                struct dnode *n = make_node(dir->m, e, ino, name, &e2);
                if (n) {
                    diskfs_add(dir, n);
                } else if (e2) {
                    err = e2;
                    break;
                }
            }
            off += rec_len;
        }
        if (d->flags & FL_INLINE_DATA) {
            break;
        }
    }
    kfree(block);
    return err;
}

static int64_t ext_read(struct dnode *n, uint64_t offset, void *buf, size_t len) {
    struct ext4 *e = E(n);
    struct enode *en = EN(n);
    if (en->flags & FL_ENCRYPT) {
        return -EACCES;
    }
    /* A short symbolic link keeps its target in i_block itself. */
    if (n->v.size < 60 && !(en->flags & (FL_EXTENTS | FL_INLINE_DATA))) {
        uint8_t *raw = kmalloc(e->inode_size);
        bool link = raw && read_inode(e, en->ino, raw) == 0 && (get16(raw) & S_IFMT) == S_IFLNK &&
                    get32(raw + 28) == 0;   /* no blocks: a fast link */
        kfree(raw);
        if (link) {
            size_t have = offset < n->v.size ? (size_t)(n->v.size - offset) : 0;
            size_t c = len < have ? len : have;
            memcpy(buf, en->iblock + offset, c);
            return (int64_t)c;
        }
    }
    return read_data(e, en, n->v.size, offset, buf, len);
}

static void ext_release(struct dnode *n) {
    kfree(n->priv);
}

static const struct diskfs_ops ext4_ops = {
    .list = ext_list,
    .read = ext_read,
    .release = ext_release,
};

/* --- mounting ------------------------------------------------------------------------------------------- */

static bool power_of(uint32_t n, uint32_t base) {
    while (n > 1 && n % base == 0) {
        n /= base;
    }
    return n == 1;
}

/* ext4_bg_has_super: with sparse_super, only groups 0, 1 and powers of 3, 5, 7 keep a superblock copy. */
static bool has_super(struct ext4 *e, uint32_t g) {
    if (!(e->ro_compat & RO_SPARSE_SUPER) || g <= 1) {
        return true;
    }
    return power_of(g, 3) || power_of(g, 5) || power_of(g, 7);
}

/* balloc.c descriptor_loc: where the block holding group g's descriptor is. */
static uint64_t desc_block(struct ext4 *e, uint32_t g) {
    uint32_t per = e->block_size / e->desc_size;
    uint32_t meta = g / per;
    if (!(e->incompat & INC_META_BG) || meta < e->first_meta_bg) {
        return e->first_data_block + 1 + meta;
    }
    uint32_t first = meta * per;
    return e->first_data_block + (uint64_t)first * e->blocks_per_group + (has_super(e, first) ? 1 : 0);
}

static int ext_mount(struct mount *m) {
    uint8_t *sb = kmalloc(1024);
    if (!sb) {
        return -ENOMEM;
    }
    int err = bio_read(m->dev, 1024, sb, 1024);
    if (err || get16(sb + 56) != MAGIC) {
        kfree(sb);
        return err ? err : -EINVAL;
    }
    struct ext4 *e = kzalloc(sizeof(*e));
    if (!e) {
        kfree(sb);
        return -ENOMEM;
    }
    e->m = m;
    e->dev = m->dev;
    uint32_t log = get32(sb + 24);
    e->block_size = log <= 6 ? 1024u << log : 0;
    e->incompat = get32(sb + 96);
    e->ro_compat = get32(sb + 100);
    e->first_data_block = get32(sb + 20);
    e->blocks_per_group = get32(sb + 32);
    e->inodes_per_group = get32(sb + 40);
    e->inode_size = get32(sb + 76) >= 1 ? get16(sb + 88) : 128;   /* rev 0 has 128-byte inodes */
    e->desc_size = (e->incompat & INC_64BIT) && get16(sb + 0xFE) >= 64 ? get16(sb + 0xFE) : 32;
    e->first_meta_bg = get32(sb + 0x104);
    e->blocks = get32(sb + 4) | ((e->incompat & INC_64BIT) ? (uint64_t)get32(sb + 0x150) << 32 : 0);
    e->free_blocks = get32(sb + 12) | ((e->incompat & INC_64BIT) ? (uint64_t)get32(sb + 0x158) << 32 : 0);
    bool has_journal = get32(sb + 92) & 0x0004;   /* COMPAT_HAS_JOURNAL */
    const uint8_t *u = sb + 0x68;
    ksnprintf(m->uuid, sizeof(m->uuid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0],
              u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    char label[17];
    memcpy(label, sb + 120, 16);
    label[16] = '\0';
    kfree(sb);

    if (!e->block_size || !e->blocks_per_group || !e->inodes_per_group || e->inode_size < 128 ||
        e->inode_size > e->block_size || (e->inode_size & (e->inode_size - 1))) {
        kfree(e);
        return -EINVAL;
    }
    if (e->incompat & ~INC_KNOWN) {
        kprintf("[WARN] ext4: %s uses features this reader doesn't know (%x)\n", m->dev->name,
                e->incompat & ~INC_KNOWN);
        kfree(e);
        return -ENOSYS;
    }
    if (e->incompat & INC_RECOVER) {
        kprintf("[WARN] ext4: %s wasn't unmounted cleanly; the newest changes may be missing\n", m->dev->name);
    }
    if (e->blocks * e->block_size > m->dev->sectors * m->dev->sector_size) {
        kfree(e);
        return -EINVAL;
    }
    e->groups = (uint32_t)((e->blocks - e->first_data_block + e->blocks_per_group - 1) / e->blocks_per_group);
    e->inode_tables = kmalloc((size_t)e->groups * sizeof(uint64_t));
    if (!e->inode_tables) {
        kfree(e);
        return -ENOMEM;
    }
    for (uint32_t g = 0; g < e->groups; g++) {
        uint32_t per = e->block_size / e->desc_size;
        uint8_t d[64];
        err = bio_read(m->dev, block_pos(e, desc_block(e, g)) + (uint64_t)(g % per) * e->desc_size, d, e->desc_size);
        if (err) {
            kfree(e->inode_tables);
            kfree(e);
            return err;
        }
        e->inode_tables[g] = get32(d + 8) | (e->desc_size >= 64 ? (uint64_t)get32(d + 0x28) << 32 : 0);
    }

    m->fs = e;
    m->ops = &ext4_ops;
    m->read_only = true;
    strlcpy(m->fs_name, e->incompat & INC_EXTENTS ? "ext4" : has_journal ? "ext3" : "ext2", sizeof(m->fs_name));
    for (int i = 0; label[i]; i++) {
        if ((unsigned char)label[i] < 0x20) {
            label[i] = '_';
        }
    }
    strlcpy(m->label, label, sizeof(m->label));
    int e2 = 0;
    m->root = make_node(m, e, ROOT_INO, "", &e2);
    if (!m->root || m->root->v.type != VNODE_DIR) {
        kfree(e->inode_tables);
        kfree(e);
        m->fs = NULL;
        return e2 ? e2 : -EIO;
    }
    return 0;
}

static void ext_unmount(struct mount *m) {
    struct ext4 *e = m->fs;
    kfree(e->inode_tables);
    kfree(e);
    m->fs = NULL;
}

static int ext_statfs(struct mount *m, uint64_t *total, uint64_t *free) {
    struct ext4 *e = m->fs;
    *total = e->blocks * e->block_size;
    *free = e->free_blocks * e->block_size;
    return 0;
}

const struct fs_driver ext4_driver = {
    .name = "ext4",
    .mount = ext_mount,
    .unmount = ext_unmount,
    .statfs = ext_statfs,
};
