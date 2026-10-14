/*
 * Disks and partitions. After Linux's block/partitions (msdos.c, efi.c):
 *
 *   - GPT if the MBR has a protective 0xEE entry (header at LBA 1);
 *   - otherwise MBR primary partitions, and logical ones in the chain of
 *     EBRs inside an extended partition (numbered from 5, like Linux);
 *   - no usable table: a "superfloppy", the filesystem starts at sector 0
 *     (a FAT boot sector carries 55 AA too, so that is checked first).
 *
 * Partition scanning and mounting happen in the "disks" thread, so the USB
 * thread that found the disk isn't held up by it.
 */

#include "block/block.h"
#include "fs/devfs.h"
#include "fs/diskfs.h"
#include "fs/mount.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "sched/sync.h"
#include "arch/x86_64/cpu.h"
#include "drivers/ahci.h"
#include "drivers/nvme.h"

static struct mutex registry_lock = MUTEX_INIT;
static struct block_device *devices;

/* Work for the disks thread. */
#define QUEUE_SIZE 16
static struct {
    struct block_device *disk;
    bool gone;
} queue[QUEUE_SIZE];
static int queue_head, queue_tail;
static struct wait_queue work = WAIT_QUEUE_INIT;

static void enqueue(struct block_device *disk, bool gone) {
    uint64_t irq = irq_save();
    int next = (queue_tail + 1) % QUEUE_SIZE;
    if (next != queue_head) {
        queue[queue_tail].disk = disk;
        queue[queue_tail].gone = gone;
        queue_tail = next;
    }
    wait_queue_wake_all(&work);
    irq_restore(irq);
}

static void add_device(struct block_device *d) {
    mutex_lock(&registry_lock);
    struct block_device **link = &devices;
    while (*link) {
        link = &(*link)->next;
    }
    d->next = NULL;
    *link = d;
    mutex_unlock(&registry_lock);
}

struct block_device *block_find(const char *name) {
    mutex_lock(&registry_lock);
    struct block_device *d = devices;
    while (d && (d->gone || d->disk->gone || strcmp(d->name, name) != 0)) {
        d = d->next;
    }
    mutex_unlock(&registry_lock);
    return d;
}

struct block_device *block_at(int n) {
    mutex_lock(&registry_lock);
    struct block_device *d = devices;
    while (d && n-- > 0) {
        d = d->next;
    }
    mutex_unlock(&registry_lock);
    return d;
}

/* --- sector I/O ------------------------------------------------------------------------------------- */

int block_read(struct block_device *d, uint64_t lba, uint32_t count, void *buf) {
    struct block_device *disk = d->disk;
    if (d->gone || disk->gone) {
        return -EIO;
    }
    if (lba > d->sectors || count > d->sectors - lba) {
        return -EINVAL;
    }
    return disk->read(disk, d->start + lba, count, buf);
}

int block_write(struct block_device *d, uint64_t lba, uint32_t count, const void *buf) {
    struct block_device *disk = d->disk;
    if (d->gone || disk->gone) {
        return -EIO;
    }
    if (d->read_only || disk->read_only) {
        return -EROFS;
    }
    if (lba > d->sectors || count > d->sectors - lba) {
        return -EINVAL;
    }
    return disk->write(disk, d->start + lba, count, buf);
}

/* --- partitions ------------------------------------------------------------------------------------- */

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p) {
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

/* A partition as the table says it, before it's a device. */
struct pentry {
    int number;
    uint64_t start, sectors;
    uint8_t type;                /* MBR type byte (0 for GPT) */
    bool gpt;
    uint8_t entry[128];          /* GPT: the entry (type GUID, GUID, name) */
    bool mountable;
};

#define MAX_PARTITIONS 64

static struct mutex scan_lock = MUTEX_INIT;   /* one table read/apply at a time */

static void fill_partition(struct block_device *p, const struct pentry *e) {
    p->part_type = e->type;
    if (e->gpt) {
        memcpy(p->type_guid, e->entry, 16);
        memcpy(p->part_guid, e->entry + 16, 16);
        uint16_t u[36];
        for (int i = 0; i < 36; i++) {
            u[i] = (uint16_t)(e->entry[56 + 2 * i] | e->entry[57 + 2 * i] << 8);
        }
        utf16_to_utf8(u, 36, p->part_name, sizeof(p->part_name));
    } else {
        memset(p->type_guid, 0, 16);
        memset(p->part_guid, 0, 16);
        p->part_name[0] = '\0';
    }
}

static struct block_device *add_partition(struct block_device *disk, const struct pentry *e) {
    struct block_device *p = kzalloc(sizeof(*p));
    if (!p) {
        return NULL;
    }
    ksnprintf(p->name, sizeof(p->name), "%sp%d", disk->name, e->number);
    strlcpy(p->model, disk->model, sizeof(p->model));
    p->sector_size = disk->sector_size;
    p->sectors = e->sectors;
    p->read_only = disk->read_only;
    p->removable = disk->removable;
    p->external = disk->external;
    p->partition = e->number;
    p->disk = disk;
    p->start = e->start;
    fill_partition(p, e);
    devfs_add(p);
    add_device(p);
    kprintf("       Disk: %s: partition %d, %lu MiB (type %02x)\n", disk->name, e->number,
            e->sectors * disk->sector_size >> 20, e->type);
    return p;
}

/* Note a partition from the table (clipped to the disk). */
static void note(struct block_device *disk, struct pentry *out, int *n, int number, uint64_t start,
                 uint64_t sectors, uint8_t type, const uint8_t *gpt_entry, bool mountable) {
    if (*n >= MAX_PARTITIONS || !sectors || start >= disk->sectors) {
        return;
    }
    if (sectors > disk->sectors - start) {
        sectors = disk->sectors - start;   /* (a table that overshoots the disk: trust the disk) */
    }
    struct pentry *e = &out[(*n)++];
    memset(e, 0, sizeof(*e));
    e->number = number;
    e->start = start;
    e->sectors = sectors;
    e->type = type;
    e->gpt = gpt_entry != NULL;
    if (gpt_entry) {
        memcpy(e->entry, gpt_entry, 128);
    }
    e->mountable = mountable;
}

/* Does this look like a filesystem's boot sector rather than an MBR? (msdos.c fat_valid_media and friends) */
static bool looks_like_boot_sector(const uint8_t *s) {
    if (memcmp(s + 3, "NTFS    ", 8) == 0 || memcmp(s + 3, "EXFAT   ", 8) == 0) {
        return true;
    }
    if (s[0] != 0xEB && s[0] != 0xE9) {
        return false;
    }
    uint16_t bps = (uint16_t)(s[11] | s[12] << 8);
    uint8_t spc = s[13];
    return bps >= 512 && bps <= 4096 && !(bps & (bps - 1)) && spc && !(spc & (spc - 1)) && s[16] >= 1 &&
           s[16] <= 2 && (s[21] == 0xF0 || s[21] >= 0xF8);
}

/* Partition type GUIDs (little-endian layout as stored) that hold nothing to mount. */
static const uint8_t GUID_MS_RESERVED[16] = { 0x16, 0xE3, 0xC9, 0xE3, 0x5C, 0x0B, 0xB8, 0x4D,
                                              0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE };
static const uint8_t GUID_EFI_SYSTEM[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                                             0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
static const uint8_t GUID_MS_RECOVERY[16] = { 0xA4, 0xBB, 0x94, 0xDE, 0xD1, 0x06, 0x40, 0x4D,
                                              0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC };
static const uint8_t GUID_LINUX_SWAP[16] = { 0x6D, 0xFD, 0x57, 0x06, 0xAB, 0xA4, 0xC4, 0x43,
                                             0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F };

/* efi.c: the primary header and its entries (no CRC checks beyond sanity; the backup isn't used). */
static bool read_gpt(struct block_device *disk, struct pentry *out, int *n) {
    uint32_t ss = disk->sector_size;
    uint8_t *hdr = kmalloc(ss);
    if (!hdr || block_read(disk, 1, 1, hdr) || memcmp(hdr, "EFI PART", 8) != 0) {
        kfree(hdr);
        return false;
    }
    uint64_t entries_lba = le64(hdr + 72);
    uint32_t count = le32(hdr + 80), size = le32(hdr + 84);
    kfree(hdr);
    if (size < 128 || size > 4096 || count > 1024) {
        return false;
    }
    uint32_t bytes = count * size, sectors = (bytes + ss - 1) / ss;
    uint8_t *table = kmalloc((size_t)sectors * ss);
    if (!table || block_read(disk, entries_lba, sectors, table)) {
        kfree(table);
        return false;
    }
    static const uint8_t zero[16];
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = table + (size_t)i * size;
        if (memcmp(e, zero, 16) == 0) {
            continue;
        }
        uint64_t first = le64(e + 32), last = le64(e + 40);
        if (last < first) {
            continue;
        }
        bool mountable = memcmp(e, GUID_MS_RESERVED, 16) != 0 && memcmp(e, GUID_EFI_SYSTEM, 16) != 0 &&
                         memcmp(e, GUID_MS_RECOVERY, 16) != 0 && memcmp(e, GUID_LINUX_SWAP, 16) != 0;
        note(disk, out, n, (int)i + 1, first, last - first + 1, 0, e, mountable);
    }
    kfree(table);
    return true;
}

static bool is_extended(uint8_t type) {
    return type == 0x05 || type == 0x0F || type == 0x85;
}

/* Types not worth trying to mount: empty, extended, swap, hidden recovery/hibernation, EFI system. */
static bool skip_type(uint8_t type) {
    return type == 0 || is_extended(type) || type == 0x82 || type == 0x27 || type == 0x84 || type == 0xA0 ||
           type == 0xEF;
}

/* msdos.c parse_extended: the chain of EBRs; each holds one logical partition and a link to the next. */
static void read_extended(struct block_device *disk, uint64_t ext_start, uint64_t ext_size, struct pentry *out,
                          int *n) {
    uint8_t *s = kmalloc(disk->sector_size);
    if (!s) {
        return;
    }
    int number = 5;
    uint64_t ebr = ext_start;
    for (int guard = 0; guard < 64; guard++) {
        if (block_read(disk, ebr, 1, s) || s[510] != 0x55 || s[511] != 0xAA) {
            break;
        }
        const uint8_t *e0 = s + 446, *e1 = s + 462;
        uint32_t start = le32(e0 + 8), size = le32(e0 + 12);
        if (e0[4] && size) {
            note(disk, out, n, number, ebr + start, size, e0[4], NULL, !skip_type(e0[4]));
        }
        number++;
        if (!is_extended(e1[4]) || !le32(e1 + 8)) {
            break;
        }
        ebr = ext_start + le32(e1 + 8);   /* the link is relative to the extended partition's start */
        if (ebr >= ext_start + ext_size) {
            break;
        }
    }
    kfree(s);
}

/* Read the partition table: the entries, and the table type ("" = none: the filesystem, if any, starts at
   sector 0, a "superfloppy"). */
static int read_table(struct block_device *disk, struct pentry *out, char table[8]) {
    int n = 0;
    table[0] = '\0';
    uint8_t *s = kmalloc(disk->sector_size);
    if (!s || block_read(disk, 0, 1, s)) {
        kfree(s);
        return 0;
    }
    if (s[510] == 0x55 && s[511] == 0xAA) {
        bool valid = true, protective = false;
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = s + 446 + 16 * i;
            if (e[0] != 0 && e[0] != 0x80) {
                valid = false;
            }
            if (e[4] == 0xEE) {
                protective = true;
            }
        }
        if (protective && read_gpt(disk, out, &n)) {
            strlcpy(table, "gpt", 8);
        } else if (valid && !looks_like_boot_sector(s)) {   /* (even empty, as on Linux) */
            strlcpy(table, "mbr", 8);
            n = 0;
            for (int i = 0; i < 4; i++) {
                const uint8_t *e = s + 446 + 16 * i;
                uint32_t start = le32(e + 8), size = le32(e + 12);
                if (e[4] && size) {
                    note(disk, out, &n, i + 1, start, size, e[4], NULL, !skip_type(e[4]));   /* (extended: shown) */
                }
                if (is_extended(e[4])) {
                    read_extended(disk, start, size, out, &n);
                }
            }
        }
    }
    kfree(s);
    return n;
}

/*
 * Make the registered partitions match the table. With `keep_mounted`, the disk was already known:
 * partitions whose entry is unchanged stay as they are (mounted or not); changed or removed ones go,
 * which fails with -EBUSY if one of them is mounted (then nothing changes). New ones are mounted.
 */
static int apply_table(struct block_device *disk) {
    struct pentry *entries = kmalloc(MAX_PARTITIONS * sizeof(struct pentry));
    if (!entries) {
        return -ENOMEM;
    }
    mutex_lock(&scan_lock);
    char table[8];
    int n = read_table(disk, entries, table);
    bool used[MAX_PARTITIONS] = { false };

    /* First check: nothing mounted may change. */
    struct block_device *mount_new[MAX_PARTITIONS + 1];
    int to_mount = 0;
    struct block_device *root_only[MAX_PARTITIONS];   /* not mounted, unless one is the installed system */
    int to_root = 0;
    int err = 0;
    mutex_lock(&registry_lock);
    for (struct block_device *d = devices; d && !err; d = d->next) {
        if (d->disk != disk || d == disk || d->gone) {
            continue;
        }
        bool same = false;
        for (int i = 0; i < n && !same; i++) {
            same = !used[i] && entries[i].number == d->partition && entries[i].start == d->start &&
                   entries[i].sectors == d->sectors;
        }
        if (!same && mount_of(d)) {
            err = -EBUSY;
        }
    }
    /* A filesystem on the whole disk can only stay if there's still no table. */
    if (!err && table[0] && mount_of(disk)) {
        err = -EBUSY;
    }
    if (!err) {
        for (struct block_device *d = devices; d; d = d->next) {
            if (d->disk != disk || d == disk || d->gone) {
                continue;
            }
            int match = -1;
            for (int i = 0; i < n && match < 0; i++) {
                if (!used[i] && entries[i].number == d->partition && entries[i].start == d->start &&
                    entries[i].sectors == d->sectors) {
                    match = i;
                }
            }
            if (match >= 0) {
                used[match] = true;
                fill_partition(d, &entries[match]);   /* (its type or name may have changed) */
            } else {
                d->gone = true;
            }
        }
        strlcpy(disk->table, table, sizeof(disk->table));
    }
    mutex_unlock(&registry_lock);
    if (!err) {
        for (int i = 0; i < n; i++) {
            if (!used[i]) {
                struct block_device *p = add_partition(disk, &entries[i]);
                if (p && entries[i].mountable) {
                    mount_new[to_mount++] = p;
                } else if (p && mount_wants_root()) {
                    root_only[to_root++] = p;   /* (an EFI System Partition may be the installed system) */
                }
            }
        }
        if (!table[0] && !mount_of(disk)) {
            mount_new[to_mount++] = disk;   /* superfloppy (or empty: then nothing mounts) */
        }
    }
    mutex_unlock(&scan_lock);
    kfree(entries);
    for (int i = 0; i < to_mount; i++) {
        mount_auto(mount_new[i]);
    }
    for (int i = 0; i < to_root; i++) {
        mount_try_root(root_only[i]);
    }
    return err;
}

/* --- the disks thread --------------------------------------------------------------------------------- */

static void disks_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t irq = irq_save();
        while (queue_head == queue_tail) {
            wait_queue_sleep(&work);
        }
        struct block_device *disk = queue[queue_head].disk;
        bool gone = queue[queue_head].gone;
        queue_head = (queue_head + 1) % QUEUE_SIZE;
        irq_restore(irq);

        if (gone) {
            mount_disk_gone(disk);
            bio_forget(disk);
        } else {
            apply_table(disk);
        }
    }
}

/* Built-in disks: their controllers can take seconds to start, so they're found in a thread. */
static void internal_disks_thread(void *arg) {
    (void)arg;
    nvme_init();
    ahci_init();
}

void block_init(void) {
    thread_create("disks", disks_thread, NULL);
    mount_init();
    thread_create("storage", internal_disks_thread, NULL);
}

void block_shutdown(void) {
    mount_unmount_all();
    for (struct block_device *d = devices; d; d = d->next) {
        if (d->disk == d && !d->gone) {
            bio_sync(d);
        }
    }
    ahci_shutdown();
    nvme_shutdown();
}

void block_add_disk(struct block_device *disk) {
    disk->disk = disk;
    disk->partition = 0;
    disk->start = 0;
    devfs_add(disk);
    add_device(disk);
    kprintf("       Disk: %s: %s, %lu MiB, %u-byte sectors%s\n", disk->name, disk->model,
            disk->sectors * disk->sector_size >> 20, disk->sector_size, disk->read_only ? ", read-only" : "");
    enqueue(disk, false);
}

int block_rescan(struct block_device *disk) {
    disk = disk->disk;
    if (disk->gone) {
        return -ENODEV;
    }
    kprintf("       Disk: %s: reading the partition table again\n", disk->name);
    return apply_table(disk);
}

void block_disk_gone(struct block_device *disk) {
    mutex_lock(&registry_lock);
    for (struct block_device *d = devices; d; d = d->next) {
        if (d->disk == disk) {
            d->gone = true;
        }
    }
    mutex_unlock(&registry_lock);
    enqueue(disk, true);
}
