/*
 * The block cache: 4 KiB pieces of a disk, keyed by their byte position on
 * the whole disk (so partitions that start at odd sectors share it too).
 *
 * Reads fill the cache, except big runs of uncached data, which go straight
 * from the disk into the caller's buffer (a file being copied shouldn't push
 * all the filesystem metadata out). Writes work the same way: small ones
 * land in cached pieces and are marked dirty, big sector-aligned ones go to
 * the disk directly. bio_sync writes the dirty pieces out, in disk order and
 * neighbours merged into big requests; the filesystems call it at the end of
 * every change (except journaled ones), so a drive pulled out between two
 * operations is consistent.
 */

#include "block/block.h"
#include "lib/errno.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/sync.h"

#define PIECE        4096
#define MAX_PIECES   4096          /* 16 MiB */
#define HASH_SIZE    1024
#define DIRECT_MIN   (32 * 1024)   /* runs at least this long bypass the cache */
#define DIRECT_MAX   (256 * 1024)  /* per disk request */

struct piece {
    struct block_device *disk;
    uint64_t index;                /* disk byte offset / PIECE */
    uint32_t bytes;                /* valid bytes (less than PIECE only at the end of the disk) */
    bool dirty;
    uint8_t *data;
    struct piece *hash_next;
    struct piece *lru_prev, *lru_next;   /* most recently used first */
};

static struct mutex lock = MUTEX_INIT;
static struct piece *table[HASH_SIZE];
static struct piece *lru_head, *lru_tail;
static int piece_count, dirty_count;

static uint32_t hash(struct block_device *disk, uint64_t index) {
    return (uint32_t)(((uint64_t)(uintptr_t)disk >> 4) * 31 + index * 2654435761u) % HASH_SIZE;
}

static void lru_unlink(struct piece *p) {
    if (p->lru_prev) {
        p->lru_prev->lru_next = p->lru_next;
    } else {
        lru_head = p->lru_next;
    }
    if (p->lru_next) {
        p->lru_next->lru_prev = p->lru_prev;
    } else {
        lru_tail = p->lru_prev;
    }
    p->lru_prev = p->lru_next = NULL;
}

static void lru_push_front(struct piece *p) {
    p->lru_next = lru_head;
    p->lru_prev = NULL;
    if (lru_head) {
        lru_head->lru_prev = p;
    }
    lru_head = p;
    if (!lru_tail) {
        lru_tail = p;
    }
}

static struct piece *find(struct block_device *disk, uint64_t index) {
    for (struct piece *p = table[hash(disk, index)]; p; p = p->hash_next) {
        if (p->disk == disk && p->index == index) {
            return p;
        }
    }
    return NULL;
}

static void remove_piece(struct piece *p) {
    struct piece **link = &table[hash(p->disk, p->index)];
    while (*link != p) {
        link = &(*link)->hash_next;
    }
    *link = p->hash_next;
    lru_unlink(p);
    if (p->dirty) {
        dirty_count--;
    }
    piece_count--;
    kfree(p->data);
    kfree(p);
}

/* Disk sector range covered by a piece. */
static int piece_io(struct piece *p, bool write) {
    struct block_device *disk = p->disk;
    if (disk->gone) {
        return -EIO;
    }
    uint64_t lba = p->index * PIECE / disk->sector_size;
    uint32_t count = p->bytes / disk->sector_size;
    return write ? disk->write(disk, lba, count, p->data) : disk->read(disk, lba, count, p->data);
}

static int write_back(struct piece *p) {
    if (!p->dirty) {
        return 0;
    }
    int err = piece_io(p, true);
    if (!err) {
        p->dirty = false;
        dirty_count--;
    }
    return err;
}

/* Make room: drop the least recently used piece (writing it first if needed). */
static void evict_one(void) {
    for (struct piece *p = lru_tail; p; p = p->lru_prev) {
        if (!p->dirty || write_back(p) == 0 || p->disk->gone) {
            remove_piece(p);
            return;
        }
    }
}

/* The piece at `index`, read from the disk unless `fill` is false (the caller overwrites all of it). */
static int get_piece(struct block_device *disk, uint64_t index, bool fill, struct piece **out) {
    struct piece *p = find(disk, index);
    if (p) {
        lru_unlink(p);
        lru_push_front(p);
        *out = p;
        return 0;
    }
    if (piece_count >= MAX_PIECES) {
        evict_one();
    }
    p = kzalloc(sizeof(*p));
    uint8_t *data = kmalloc(PIECE);
    if (!p || !data) {
        kfree(p);
        kfree(data);
        return -ENOMEM;
    }
    uint64_t disk_bytes = disk->sectors * disk->sector_size;
    p->disk = disk;
    p->index = index;
    p->data = data;
    p->bytes = disk_bytes - index * PIECE < PIECE ? (uint32_t)(disk_bytes - index * PIECE) : PIECE;
    if (fill) {
        int err = piece_io(p, false);
        if (err) {
            kfree(data);
            kfree(p);
            return err;
        }
    }
    uint32_t h = hash(disk, index);
    p->hash_next = table[h];
    table[h] = p;
    lru_push_front(p);
    piece_count++;
    *out = p;
    return 0;
}

/* How many bytes from `pos` on (at most `len`) are in pieces that aren't cached. */
static uint64_t uncached_run(struct block_device *disk, uint64_t pos, uint64_t len) {
    uint64_t run = 0;
    while (run < len && run < DIRECT_MAX) {
        uint64_t index = (pos + run) / PIECE;
        if (find(disk, index)) {
            break;
        }
        run += PIECE - (pos + run) % PIECE;
    }
    return run < len ? run : len;
}

/* Turn a partition-relative range into a disk byte position, checking the bounds. */
static int locate(struct block_device *d, uint64_t offset, size_t len, uint64_t *pos) {
    if (d->gone || d->disk->gone) {
        return -EIO;
    }
    uint64_t size = d->sectors * d->sector_size;
    if (offset > size || len > size - offset) {
        return -EINVAL;
    }
    *pos = d->start * d->sector_size + offset;
    return 0;
}

int bio_read(struct block_device *d, uint64_t offset, void *buf, size_t len) {
    uint64_t pos;
    int err = locate(d, offset, len, &pos);
    if (err) {
        return err;
    }
    struct block_device *disk = d->disk;
    uint32_t ss = disk->sector_size;
    uint8_t *out = buf;
    mutex_lock(&lock);
    while (len && !err) {
        /* A long sector-aligned stretch nobody cached: read it in one go. */
        if (pos % ss == 0 && len >= DIRECT_MIN) {
            uint64_t run = uncached_run(disk, pos, len) / ss * ss;
            if (run >= DIRECT_MIN) {
                err = disk->gone ? -EIO : disk->read(disk, pos / ss, (uint32_t)(run / ss), out);
                pos += run;
                out += run;
                len -= run;
                continue;
            }
        }
        struct piece *p;
        err = get_piece(disk, pos / PIECE, true, &p);
        if (err) {
            break;
        }
        uint32_t at = (uint32_t)(pos % PIECE);
        size_t n = PIECE - at < len ? PIECE - at : len;
        memcpy(out, p->data + at, n);
        pos += n;
        out += n;
        len -= n;
    }
    mutex_unlock(&lock);
    return err;
}

int bio_write(struct block_device *d, uint64_t offset, const void *buf, size_t len) {
    if (d->read_only || d->disk->read_only) {
        return -EROFS;
    }
    uint64_t pos;
    int err = locate(d, offset, len, &pos);
    if (err) {
        return err;
    }
    struct block_device *disk = d->disk;
    uint32_t ss = disk->sector_size;
    const uint8_t *in = buf;
    mutex_lock(&lock);
    while (len && !err) {
        if (pos % ss == 0 && len >= DIRECT_MIN) {
            uint64_t run = uncached_run(disk, pos, len) / ss * ss;
            if (run >= DIRECT_MIN) {
                err = disk->gone ? -EIO : disk->write(disk, pos / ss, (uint32_t)(run / ss), in);
                pos += run;
                in += run;
                len -= run;
                continue;
            }
        }
        uint32_t at = (uint32_t)(pos % PIECE);
        size_t n = PIECE - at < len ? PIECE - at : len;
        struct piece *p;
        /* No need to read what's about to be overwritten whole. */
        bool whole = at == 0 && n == PIECE;
        err = get_piece(disk, pos / PIECE, !whole, &p);
        if (err) {
            break;
        }
        if (at + n > p->bytes) {
            err = -EINVAL;
            break;
        }
        memcpy(p->data + at, in, n);
        if (!p->dirty) {
            p->dirty = true;
            dirty_count++;
        }
        pos += n;
        in += n;
        len -= n;
    }
    mutex_unlock(&lock);
    return err;
}

/* Sort pieces by their place on the disk (a heap sort: no recursion, no extra memory). */
static void sift_down(struct piece **a, int root, int end) {
    for (;;) {
        int child = root * 2 + 1, top = root;
        if (child <= end && a[child]->index > a[top]->index) {
            top = child;
        }
        if (child + 1 <= end && a[child + 1]->index > a[top]->index) {
            top = child + 1;
        }
        if (top == root) {
            return;
        }
        struct piece *t = a[root];
        a[root] = a[top];
        a[top] = t;
        root = top;
    }
}

static void sort_pieces(struct piece **a, int n) {
    for (int i = n / 2 - 1; i >= 0; i--) {
        sift_down(a, i, n - 1);
    }
    for (int end = n - 1; end > 0; end--) {
        struct piece *t = a[0];
        a[0] = a[end];
        a[end] = t;
        sift_down(a, 0, end - 1);
    }
}

/* Write the disk's dirty pieces in order, neighbours together as one request (up to DIRECT_MAX):
   a few big writes instead of many small ones in no particular order. */
static int write_back_all(struct block_device *disk) {
    int n = 0;
    struct piece **list = dirty_count ? kmalloc(sizeof(struct piece *) * (size_t)dirty_count) : NULL;
    uint8_t *run = list ? kmalloc(DIRECT_MAX) : NULL;
    if (!run) {
        kfree(list);
        int err = 0;   /* (no memory for that: one at a time) */
        for (struct piece *p = lru_head; p && dirty_count; p = p->lru_next) {
            if (p->disk == disk && p->dirty) {
                int e = write_back(p);
                err = err ? err : e;
            }
        }
        return err;
    }
    for (struct piece *p = lru_head; p && n < dirty_count; p = p->lru_next) {
        if (p->disk == disk && p->dirty) {
            list[n++] = p;
        }
    }
    sort_pieces(list, n);
    int err = 0;
    for (int i = 0; i < n;) {
        /* A run of whole, consecutive pieces. */
        int j = i + 1;
        while (j < n && list[j]->index == list[j - 1]->index + 1 && list[j - 1]->bytes == PIECE &&
               (uint64_t)(j - i + 1) * PIECE <= DIRECT_MAX) {
            j++;
        }
        int e;
        if (j - i == 1) {
            e = write_back(list[i]);
        } else if (disk->gone) {
            e = -EIO;
        } else {
            size_t bytes = 0;
            for (int k = i; k < j; k++) {
                memcpy(run + bytes, list[k]->data, list[k]->bytes);
                bytes += list[k]->bytes;
            }
            e = disk->write(disk, list[i]->index * PIECE / disk->sector_size, (uint32_t)(bytes / disk->sector_size), run);
            for (int k = i; !e && k < j; k++) {
                list[k]->dirty = false;
                dirty_count--;
            }
        }
        err = err ? err : e;
        i = j;
    }
    kfree(run);
    kfree(list);
    return err;
}

int bio_sync(struct block_device *d) {
    struct block_device *disk = d->disk;
    mutex_lock(&lock);
    int err = write_back_all(disk);
    mutex_unlock(&lock);
    if (!err && disk->flush && !disk->gone) {
        err = disk->flush(disk);
    }
    return err;
}

void bio_forget(struct block_device *disk) {
    mutex_lock(&lock);
    for (int h = 0; h < HASH_SIZE; h++) {
        struct piece *p = table[h];
        while (p) {
            struct piece *next = p->hash_next;
            if (p->disk == disk) {
                remove_piece(p);
            }
            p = next;
        }
    }
    mutex_unlock(&lock);
}

void bio_stats(uint64_t *cached_bytes, uint64_t *dirty_bytes) {
    *cached_bytes = (uint64_t)piece_count * PIECE;
    *dirty_bytes = (uint64_t)dirty_count * PIECE;
}
