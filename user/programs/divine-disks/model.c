/* What's there: disks, partitions, free space, and the filesystems on them (like blkid, reading
   the start of each /dev file). */

#include "disks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <paths.h>

struct disk disks[MAX_DISKS];
int disk_count;

const struct part_type part_types[] = {
    { "Microsoft Basic Data", "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", 0x07 },
    { "Linux Filesystem", "0FC63DAF-8483-4772-8E79-3D69D8477DE4", 0x83 },
    { "EFI System", "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", 0xEF },
    { "Linux Swap", "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", 0x82 },
    { "Microsoft Reserved", "E3C9E316-0B5C-4DB8-817D-F92DF00215AE", 0x00 },
    { "Windows Recovery", "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", 0x27 },
    { "W95 FAT32 (LBA)", NULL, 0x0C },
    { "W95 FAT16 (LBA)", NULL, 0x0E },
};
const int part_type_count = sizeof(part_types) / sizeof(part_types[0]);

static const struct {
    const char *guid, *name;
} gpt_names[] = {
    { "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI System" },
    { "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "Microsoft Basic Data" },
    { "E3C9E316-0B5C-4DB8-817D-F92DF00215AE", "Microsoft Reserved" },
    { "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", "Windows Recovery Environment" },
    { "0FC63DAF-8483-4772-8E79-3D69D8477DE4", "Linux Filesystem" },
    { "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", "Linux Swap" },
    { "E6D6D379-F507-44C2-A23C-238F2A3DF928", "Linux LVM" },
    { "933AC7E1-2EB4-4F13-B844-0E14E2AEF915", "Linux Home" },
    { "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709", "Linux Root (x86-64)" },
    { "21686148-6449-6E6F-744E-656564454649", "BIOS Boot" },
    { "48465300-0000-11AA-AA11-00306543ECAC", "Apple HFS/HFS+" },
    { "7C3457EF-0000-11AA-AA11-00306543ECAC", "Apple APFS" },
};

static const struct {
    uint8_t type;
    const char *name;
} mbr_names[] = {
    { 0x01, "FAT12" }, { 0x04, "FAT16 <32M" }, { 0x05, "Extended" }, { 0x06, "FAT16" },
    { 0x07, "HPFS/NTFS/exFAT" }, { 0x0B, "W95 FAT32" }, { 0x0C, "W95 FAT32 (LBA)" }, { 0x0E, "W95 FAT16 (LBA)" },
    { 0x0F, "W95 Extended (LBA)" }, { 0x27, "Hidden NTFS WinRE" }, { 0x82, "Linux Swap" }, { 0x83, "Linux" },
    { 0x85, "Linux Extended" }, { 0x8E, "Linux LVM" }, { 0xEE, "GPT Protective" }, { 0xEF, "EFI System" },
};

/* --- small helpers ------------------------------------------------------------------------------------------ */

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p) {
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

void guid_string(const uint8_t *g, char *out) {
    snprintf(out, 37, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", le32(g), le16(g + 4), le16(g + 6), g[8],
             g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

bool guid_parse(const char *s, uint8_t *g) {
    uint8_t raw[16];
    int n = 0;
    for (const char *c = s; *c && n < 32; c++) {
        if (*c == '-') {
            continue;
        }
        int v = hexval(*c);
        if (v < 0) {
            return false;
        }
        if (n % 2 == 0) {
            raw[n / 2] = (uint8_t)(v << 4);
        } else {
            raw[n / 2] |= (uint8_t)v;
        }
        n++;
    }
    if (n != 32) {
        return false;
    }
    /* The first three fields are stored little-endian. */
    g[0] = raw[3], g[1] = raw[2], g[2] = raw[1], g[3] = raw[0];
    g[4] = raw[5], g[5] = raw[4];
    g[6] = raw[7], g[7] = raw[6];
    memcpy(g + 8, raw + 8, 8);
    return true;
}

static uint64_t rng;

void guid_random(uint8_t *g) {
    if (!rng) {
        rng = __builtin_ia32_rdtsc() ^ (wall_time() << 32) ^ uptime_ms() ^ 0x9E3779B97F4A7C15ull;
    }
    for (int i = 0; i < 16; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        g[i] = (uint8_t)(rng >> 24) ^ (uint8_t)__builtin_ia32_rdtsc();
    }
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);   /* version 4 (in the little-endian third field) */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);   /* variant */
}

void human_size(uint64_t bytes, char *out, size_t size) {
    static const char *const units[] = { "bytes", "kB", "MB", "GB", "TB", "PB" };
    if (bytes < 1000) {
        snprintf(out, size, "%lu bytes", (unsigned long)bytes);
        return;
    }
    int u = 0;
    uint64_t v = bytes * 10;
    while (v >= 10000 && u < 5) {
        v /= 1000;
        u++;
    }
    if (v >= 1000) {
        snprintf(out, size, "%lu %s", (unsigned long)((v + 5) / 10), units[u]);
    } else {
        snprintf(out, size, "%lu.%lu %s", (unsigned long)(v / 10), (unsigned long)(v % 10), units[u]);
    }
}

uint64_t disk_bytes(const struct disk *d) {
    return d->sectors * d->sector_size;
}

uint64_t vol_bytes(const struct disk *d, const struct volume *v) {
    return v->sectors * d->sector_size;
}

void disk_title(const struct disk *d, char *out, size_t size) {
    char s[32];
    human_size(disk_bytes(d), s, sizeof(s));
    snprintf(out, size, "%s %s", s, d->flags & OS_BLOCK_REMOVABLE ? "Drive" : "Disk");
}

const char *part_type_name(const struct disk *d, const struct volume *v) {
    static char buf[64];
    if (v->kind != VOL_PARTITION) {
        return "";
    }
    if (strcmp(d->table, "gpt") == 0) {
        char g[40];
        guid_string(v->type_guid, g);
        for (size_t i = 0; i < sizeof(gpt_names) / sizeof(gpt_names[0]); i++) {
            if (strcmp(g, gpt_names[i].guid) == 0) {
                return gpt_names[i].name;
            }
        }
        snprintf(buf, sizeof(buf), "%s", g);
        return buf;
    }
    for (size_t i = 0; i < sizeof(mbr_names) / sizeof(mbr_names[0]); i++) {
        if (mbr_names[i].type == v->mbr_type) {
            snprintf(buf, sizeof(buf), "%s (0x%02x)", mbr_names[i].name, v->mbr_type);
            return buf;
        }
    }
    snprintf(buf, sizeof(buf), "0x%02x", v->mbr_type);
    return buf;
}

const char *fs_short(const struct volume *v) {
    if (strcmp(v->fs, "vfat") == 0) {
        return "FAT";
    }
    if (strcmp(v->fs, "exfat") == 0) {
        return "exFAT";
    }
    if (strcmp(v->fs, "ntfs") == 0) {
        return "NTFS";
    }
    if (strcmp(v->fs, "ext4") == 0) {
        return "Ext4";
    }
    if (strcmp(v->fs, "ext3") == 0) {
        return "Ext3";
    }
    if (strcmp(v->fs, "ext2") == 0) {
        return "Ext2";
    }
    if (strcmp(v->fs, "swap") == 0) {
        return "Swap";
    }
    return v->extended ? "Extended" : "Unknown";
}

/* --- probing (what blkid does) -------------------------------------------------------------------------- */

static bool read_at(int fd, uint64_t offset, void *buf, size_t len) {
    if (seek(fd, (long)offset, OS_SEEK_SET) < 0) {
        return false;
    }
    size_t done = 0;
    while (done < len) {
        long n = read(fd, (char *)buf + done, len - done);
        if (n <= 0) {
            return false;
        }
        done += (size_t)n;
    }
    return true;
}

static void utf16_label(const uint8_t *p, int chars, char *out, size_t size) {
    size_t o = 0;
    for (int i = 0; i < chars && o + 4 < size; i++) {
        uint16_t c = le16(p + 2 * i);
        if (!c) {
            break;
        }
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | c >> 12);
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = '\0';
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\0')) {
        s[--n] = '\0';
    }
}

/* NTFS: the label is the $VOLUME_NAME attribute of MFT record 3 ($Volume). */
static void ntfs_label(int fd, const uint8_t *b, char *out, size_t size) {
    uint32_t bps = le16(b + 11), spc = b[13] <= 0x80 ? b[13] : 1u << (256 - b[13]);
    uint32_t cluster = bps * spc;
    int8_t rec = (int8_t)b[64];
    uint32_t rsize = rec > 0 ? (uint32_t)rec * cluster : 1u << (-rec);
    if (rsize < 512 || rsize > 4096) {
        return;
    }
    uint8_t r[4096];
    if (!read_at(fd, le64(b + 48) * cluster + 3 * (uint64_t)rsize, r, rsize) || memcmp(r, "FILE", 4) != 0) {
        return;
    }
    uint16_t usa = le16(r + 4), count = le16(r + 6);
    for (int i = 1; i < count && (uint32_t)i * 512 <= rsize; i++) {
        memcpy(r + i * 512 - 2, r + usa + 2 * i, 2);
    }
    for (uint32_t at = le16(r + 20); at + 24 < rsize;) {
        uint32_t type = le32(r + at), len = le32(r + at + 4);
        if (type == 0xFFFFFFFF || len < 16 || at + len > rsize) {
            break;
        }
        if (type == 0x60 && !r[at + 8]) {
            uint32_t vlen = le32(r + at + 16), voff = le16(r + at + 20);
            utf16_label(r + at + voff, (int)(vlen / 2), out, size);
            return;
        }
        at += len;
    }
}

/* exFAT: the label is an entry (type 0x83) in the root directory. */
static void exfat_label(int fd, const uint8_t *b, char *out, size_t size) {
    uint32_t ss = 1u << b[108], cluster = ss << b[109];
    uint64_t heap = (uint64_t)le32(b + 88) * ss;
    uint32_t root = le32(b + 96);
    uint8_t buf[4096];
    uint32_t n = cluster < sizeof(buf) ? cluster : sizeof(buf);
    if (root < 2 || !read_at(fd, heap + (uint64_t)(root - 2) * cluster, buf, n)) {
        return;
    }
    for (uint32_t at = 0; at + 32 <= n; at += 32) {
        if (buf[at] == 0) {
            break;
        }
        if (buf[at] == 0x83) {
            utf16_label(buf + at + 2, buf[at + 1] > 11 ? 11 : buf[at + 1], out, size);
            return;
        }
    }
}

static void probe(struct volume *v) {
    v->fs[0] = v->label[0] = v->uuid[0] = '\0';
    snprintf(v->fs_desc, sizeof(v->fs_desc), "%s", v->extended ? "Extended Partition" : "Unknown");
    if (v->extended) {
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), _PATH_DEV "/%s", v->dev);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return;
    }
    static uint8_t b[4096];
    if (!read_at(fd, 0, b, sizeof(b))) {
        close(fd);
        return;
    }
    if (memcmp(b + 3, "NTFS    ", 8) == 0) {
        strcpy(v->fs, "ntfs");
        strcpy(v->fs_desc, "NTFS");
        snprintf(v->uuid, sizeof(v->uuid), "%08X%08X", le32(b + 76), le32(b + 72));
        ntfs_label(fd, b, v->label, sizeof(v->label));
    } else if (memcmp(b + 3, "EXFAT   ", 8) == 0) {
        strcpy(v->fs, "exfat");
        snprintf(v->fs_desc, sizeof(v->fs_desc), "exFAT (version %u.%u)", b[105], b[104]);
        uint32_t serial = le32(b + 100);
        snprintf(v->uuid, sizeof(v->uuid), "%04X-%04X", serial >> 16, serial & 0xFFFF);
        exfat_label(fd, b, v->label, sizeof(v->label));
    } else if (le16(b + 1024 + 56) == 0xEF53) {
        const uint8_t *sb = b + 1024;
        uint32_t compat = le32(sb + 92), incompat = le32(sb + 96);
        const char *kind = incompat & 0x40 ? "ext4" : compat & 0x4 ? "ext3" : "ext2";
        strcpy(v->fs, kind);
        snprintf(v->fs_desc, sizeof(v->fs_desc), "%s (version 1.0)", kind[3] == '4' ? "Ext4" : kind[3] == '3' ? "Ext3" : "Ext2");
        memcpy(v->label, sb + 120, 16);
        v->label[16] = '\0';
        const uint8_t *u = sb + 0x68;
        snprintf(v->uuid, sizeof(v->uuid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0],
                 u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    } else if (memcmp(b + 4086, "SWAPSPACE2", 10) == 0) {
        strcpy(v->fs, "swap");
        strcpy(v->fs_desc, "Swap");
    } else if ((b[0] == 0xEB || b[0] == 0xE9) && b[510] == 0x55 && b[511] == 0xAA && le16(b + 11) >= 512 && b[13] &&
               b[16] >= 1 && b[16] <= 2) {
        uint32_t bps = le16(b + 11), spc = b[13], reserved = le16(b + 14), fats = b[16], root = le16(b + 17);
        uint32_t fatsz = le16(b + 22) ? le16(b + 22) : le32(b + 36);
        uint64_t total = le16(b + 19) ? le16(b + 19) : le32(b + 32);
        uint64_t data = reserved + (uint64_t)fats * fatsz + (root * 32 + bps - 1) / bps;
        uint64_t clusters = total > data ? (total - data) / spc : 0;
        int bits = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
        strcpy(v->fs, "vfat");
        snprintf(v->fs_desc, sizeof(v->fs_desc), "FAT (%d-bit version)", bits);
        const uint8_t *ext = bits == 32 ? b + 64 : b + 36;
        if (ext[2] == 0x29 || ext[2] == 0x28) {
            uint32_t serial = le32(ext + 3);
            snprintf(v->uuid, sizeof(v->uuid), "%04X-%04X", serial >> 16, serial & 0xFFFF);
        }
        if (ext[2] == 0x29) {
            memcpy(v->label, ext + 7, 11);
            v->label[11] = '\0';
            trim(v->label);
            if (strcmp(v->label, "NO NAME") == 0) {
                v->label[0] = '\0';
            }
        }
    }
    trim(v->label);
    close(fd);
}

/* Probing reads every volume, which over USB takes a while, so results are kept until something might
   have changed them: the same device at the same place with the same size reads the same until a job runs. */
#define CACHE_MAX (MAX_DISKS * MAX_VOLUMES)
static struct cached {
    char dev[16];
    uint64_t start, sectors;
    char fs[16], fs_desc[48], label[OS_NAME_MAX], uuid[40];
} cache[CACHE_MAX];
static int cached_count;

void model_forget(void) {
    cached_count = 0;
}

static void probe_cached(struct volume *v) {
    for (int i = 0; i < cached_count; i++) {
        struct cached *c = &cache[i];
        if (strcmp(c->dev, v->dev) == 0 && c->start == v->start && c->sectors == v->sectors) {
            strcpy(v->fs, c->fs);
            strcpy(v->fs_desc, c->fs_desc);
            strcpy(v->label, c->label);
            strcpy(v->uuid, c->uuid);
            return;
        }
    }
    probe(v);
    if (cached_count < CACHE_MAX) {
        struct cached *c = &cache[cached_count++];
        strcpy(c->dev, v->dev);
        c->start = v->start;
        c->sectors = v->sectors;
        strcpy(c->fs, v->fs);
        strcpy(c->fs_desc, v->fs_desc);
        strcpy(c->label, v->label);
        strcpy(c->uuid, v->uuid);
    }
}

/* --- the list --------------------------------------------------------------------------------------- */

static int by_start(const void *a, const void *b) {
    const struct volume *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static void add_free(struct disk *d, uint64_t from, uint64_t to) {
    uint64_t align = MIB / d->sector_size;
    uint64_t start = (from + align - 1) / align * align;
    if (to <= start || to - start < align || d->nvols >= MAX_VOLUMES) {
        return;
    }
    struct volume *v = &d->vols[d->nvols++];
    memset(v, 0, sizeof(*v));
    v->kind = VOL_FREE;
    v->start = start;
    v->sectors = to - start;
    strcpy(v->fs_desc, "Unallocated Space");
}

void model_refresh(void) {
    static struct os_block_info info;
    static struct os_drive_info drive;
    disk_count = 0;
    /* Disks first, then their partitions. */
    for (int i = 0; blockdev(i, &info) == 0 && disk_count < MAX_DISKS; i++) {
        if (!(info.flags & OS_BLOCK_DISK)) {
            continue;
        }
        struct disk *d = &disks[disk_count++];
        memset(d, 0, sizeof(*d));
        strcpy(d->name, info.name);
        strcpy(d->model, info.model);
        strcpy(d->serial, info.serial);
        d->sectors = info.sectors;
        d->sector_size = info.sector_size;
        d->flags = info.flags;
        strcpy(d->table, info.table);
        if (!d->table[0]) {
            struct volume *v = &d->vols[d->nvols++];
            memset(v, 0, sizeof(*v));
            v->kind = VOL_WHOLE;
            strcpy(v->dev, d->name);
            v->sectors = d->sectors;
            strcpy(v->mounted_at, info.mounted_at);
        }
    }
    for (int i = 0; blockdev(i, &info) == 0; i++) {
        if (info.flags & OS_BLOCK_DISK) {
            continue;
        }
        for (int k = 0; k < disk_count; k++) {
            struct disk *d = &disks[k];
            if (strcmp(d->name, info.disk) != 0 || d->nvols >= MAX_VOLUMES || !d->table[0]) {
                continue;
            }
            struct volume *v = &d->vols[d->nvols++];
            memset(v, 0, sizeof(*v));
            v->kind = VOL_PARTITION;
            strcpy(v->dev, info.name);
            v->number = info.partition;
            v->start = info.start;
            v->sectors = info.sectors;
            v->mbr_type = info.mbr_type;
            memcpy(v->type_guid, info.type_guid, 16);
            strcpy(v->part_name, info.part_name);
            v->extended = strcmp(d->table, "mbr") == 0 && (info.mbr_type == 0x05 || info.mbr_type == 0x0F ||
                                                           info.mbr_type == 0x85);
            v->logical = strcmp(d->table, "mbr") == 0 && info.partition > 4;
            strcpy(v->mounted_at, info.mounted_at);
        }
    }

    for (int k = 0; k < disk_count; k++) {
        struct disk *d = &disks[k];
        for (int i = 0; i < d->nvols; i++) {
            probe_cached(&d->vols[i]);
        }
        /* Size and free space of what's mounted (from the drive list). */
        for (int j = 0; drives(j, &drive) == 0; j++) {
            for (int i = 0; i < d->nvols; i++) {
                struct volume *v = &d->vols[i];
                if (v->mounted_at[0] && strcmp(v->mounted_at, drive.path) == 0) {
                    v->fs_size = drive.total;
                    v->fs_free = drive.free;
                    v->mounted_ro = drive.flags & OS_DRIVE_READ_ONLY;
                    if (drive.label[0]) {
                        strcpy(v->label, drive.label);
                    }
                }
            }
        }
        if (!d->table[0]) {
            continue;
        }
        /* The volume bar: partitions in disk order (an extended one is hidden behind its logical ones),
           and the free space between them. */
        qsort(d->vols, (size_t)d->nvols, sizeof(d->vols[0]), by_start);
        int parts = d->nvols;
        uint64_t first = MIB / d->sector_size;
        uint64_t end = strcmp(d->table, "gpt") == 0 ? d->sectors - 34 : d->sectors;
        uint64_t at = first;
        for (int i = 0; i < parts; i++) {
            struct volume *v = &d->vols[i];
            if (v->extended) {
                continue;
            }
            if (v->start > at && !v->logical) {
                add_free(d, at, v->start);
            }
            if (v->start + v->sectors > at) {
                at = v->start + v->sectors;
            }
        }
        add_free(d, at, end);
        qsort(d->vols, (size_t)d->nvols, sizeof(d->vols[0]), by_start);
    }
}
