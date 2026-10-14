/*
 * Partition tables, written straight to /dev/<disk>:
 *
 *   GPT (UEFI spec 5.3): a protective MBR (one 0xEE entry over the disk),
 *   the header at LBA 1 and 128 entries of 128 bytes from LBA 2, and a
 *   backup of both at the end (entries, then the header in the last LBA).
 *   Headers and the entry array carry CRC32s.
 *
 *   MBR: four 16-byte entries at byte 446, 0x55AA at 510; boot code and
 *   the disk signature (440) are kept. Logical partitions (in an extended
 *   one) are shown but not edited here.
 *
 * Partitions are aligned to 1 MiB, like every partitioning tool since Vista.
 */

#include "disks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <paths.h>

#define GPT_ENTRIES     128
#define GPT_ENTRY_SIZE  128

static uint32_t crc_table[256];

static uint32_t crc32(const uint8_t *p, size_t n) {
    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            crc_table[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd64(const uint8_t *p) {
    return rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

static int open_disk(const struct disk *d, int flags) {
    char path[48];
    snprintf(path, sizeof(path), _PATH_DEV "/%s", d->name);
    return open(path, flags);
}

static int io_at(int fd, uint64_t offset, void *buf, size_t len, bool write_it) {
    if (seek(fd, (long)offset, OS_SEEK_SET) < 0) {
        return -EIO;
    }
    size_t done = 0;
    while (done < len) {
        long n = write_it ? write(fd, (const char *)buf + done, len - done) : read(fd, (char *)buf + done, len - done);
        if (n <= 0) {
            return n < 0 ? (int)n : -EIO;
        }
        done += (size_t)n;
    }
    return 0;
}

uint64_t pt_align(const struct disk *d) {
    return MIB / d->sector_size;
}

static uint32_t gpt_entry_sectors(const struct disk *d) {
    return (GPT_ENTRIES * GPT_ENTRY_SIZE + d->sector_size - 1) / d->sector_size;
}

void pt_new(const struct disk *d, struct ptable *t, const char *type) {
    memset(t, 0, sizeof(*t));
    strcpy(t->type, type);
    guid_random(t->disk_guid);
    uint8_t sig[16];
    guid_random(sig);
    t->mbr_signature = rd32(sig);
    if (strcmp(type, "gpt") == 0) {
        t->first_usable = 2 + gpt_entry_sectors(d);
        t->last_usable = d->sectors - 2 - gpt_entry_sectors(d);
    } else {
        t->first_usable = 1;
        t->last_usable = d->sectors - 1;
    }
}

int pt_read(const struct disk *d, struct ptable *t) {
    memset(t, 0, sizeof(*t));
    int fd = open_disk(d, O_RDONLY);
    if (fd < 0) {
        return fd;
    }
    uint32_t ss = d->sector_size;
    uint8_t *buf = malloc(GPT_ENTRIES * GPT_ENTRY_SIZE + ss);
    int err = buf ? io_at(fd, 0, buf, ss, false) : -ENOMEM;
    if (!err && strcmp(d->table, "gpt") == 0) {
        strcpy(t->type, "gpt");
        err = io_at(fd, ss, buf, ss, false);
        if (!err && memcmp(buf, "EFI PART", 8) != 0) {
            err = -EINVAL;
        }
        if (!err) {
            memcpy(t->disk_guid, buf + 56, 16);
            t->first_usable = rd64(buf + 40);
            t->last_usable = rd64(buf + 48);
            uint64_t entries_lba = rd64(buf + 72);
            uint32_t count = rd32(buf + 80), size = rd32(buf + 84);
            if (size != GPT_ENTRY_SIZE || count > GPT_ENTRIES) {
                err = -EINVAL;   /* unusual layouts: don't touch */
            } else {
                err = io_at(fd, entries_lba * ss, buf, (size_t)count * size, false);
            }
            for (uint32_t i = 0; !err && i < count && t->count < PT_MAX; i++) {
                const uint8_t *e = buf + (size_t)i * size;
                static const uint8_t zero[16];
                if (memcmp(e, zero, 16) == 0) {
                    continue;
                }
                struct pt_entry *p = &t->e[t->count++];
                p->number = (int)i + 1;
                memcpy(p->type_guid, e, 16);
                memcpy(p->guid, e + 16, 16);
                p->start = rd64(e + 32);
                p->sectors = rd64(e + 40) - p->start + 1;
                p->attrs = rd64(e + 48);
                /* UTF-16 name -> UTF-8 (ASCII is enough to keep and show) */
                int o = 0;
                for (int k = 0; k < 36 && o < 70; k++) {
                    uint16_t c = (uint16_t)(e[56 + 2 * k] | e[57 + 2 * k] << 8);
                    if (!c) {
                        break;
                    }
                    p->name[o++] = c < 0x80 ? (char)c : '?';
                }
                p->name[o] = '\0';
            }
        }
    } else if (!err && strcmp(d->table, "mbr") == 0) {
        strcpy(t->type, "mbr");
        memcpy(t->mbr_head, buf, 446);
        t->mbr_signature = rd32(buf + 440);
        t->first_usable = 1;
        t->last_usable = d->sectors - 1;
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = buf + 446 + 16 * i;
            if (!e[4] || !rd32(e + 12)) {
                continue;
            }
            struct pt_entry *p = &t->e[t->count++];
            p->number = i + 1;
            p->mbr_type = e[4];
            p->boot = e[0] == 0x80;
            p->start = rd32(e + 8);
            p->sectors = rd32(e + 12);
            if (e[4] == 0x05 || e[4] == 0x0F || e[4] == 0x85) {
                t->has_logical = true;
            }
        }
    } else if (!err) {
        err = -EINVAL;   /* no table */
    }
    free(buf);
    close(fd);
    return err;
}

/* CHS for an MBR entry: past what CHS can say, the conventional "too big" value. */
static void chs(uint8_t *out, uint64_t lba) {
    uint64_t c = lba / (255 * 63), h = lba / 63 % 255, s = lba % 63 + 1;
    if (c > 1023) {
        c = 1023, h = 254, s = 63;
    }
    out[0] = (uint8_t)h;
    out[1] = (uint8_t)(s | (c >> 2 & 0xC0));
    out[2] = (uint8_t)c;
}

static void mbr_entry(uint8_t *e, bool boot, uint8_t type, uint64_t start, uint64_t sectors) {
    memset(e, 0, 16);
    if (!type) {
        return;
    }
    e[0] = boot ? 0x80 : 0;
    chs(e + 1, start);
    e[4] = type;
    chs(e + 5, start + sectors - 1);
    wr32(e + 8, start > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)start);
    wr32(e + 12, sectors > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sectors);
}

int pt_write(const struct disk *d, const struct ptable *t) {
    uint32_t ss = d->sector_size;
    int fd = open_disk(d, O_RDWR);
    if (fd < 0) {
        return fd;
    }
    uint8_t *mbr = calloc(1, ss);
    int err = mbr ? 0 : -ENOMEM;
    if (!err && strcmp(t->type, "mbr") == 0) {
        memcpy(mbr, t->mbr_head, 446);
        wr32(mbr + 440, t->mbr_signature);
        for (int i = 0; i < t->count; i++) {
            const struct pt_entry *p = &t->e[i];
            if (p->number >= 1 && p->number <= 4) {
                mbr_entry(mbr + 446 + 16 * (p->number - 1), p->boot, p->mbr_type, p->start, p->sectors);
            }
        }
        mbr[510] = 0x55;
        mbr[511] = 0xAA;
        err = io_at(fd, 0, mbr, ss, true);
    } else if (!err) {
        /* Protective MBR */
        wr32(mbr + 440, t->mbr_signature);
        mbr_entry(mbr + 446, false, 0xEE, 1, d->sectors - 1);
        mbr[510] = 0x55;
        mbr[511] = 0xAA;
        uint32_t es = gpt_entry_sectors(d);
        size_t ebytes = (size_t)es * ss;
        uint8_t *entries = calloc(1, ebytes), *hdr = calloc(1, ss);
        if (!entries || !hdr) {
            err = -ENOMEM;
        }
        for (int i = 0; !err && i < t->count; i++) {
            const struct pt_entry *p = &t->e[i];
            if (p->number < 1 || p->number > GPT_ENTRIES) {
                continue;
            }
            uint8_t *e = entries + (size_t)(p->number - 1) * GPT_ENTRY_SIZE;
            memcpy(e, p->type_guid, 16);
            memcpy(e + 16, p->guid, 16);
            wr64(e + 32, p->start);
            wr64(e + 40, p->start + p->sectors - 1);
            wr64(e + 48, p->attrs);
            for (int k = 0; k < 36 && p->name[k]; k++) {
                e[56 + 2 * k] = (uint8_t)p->name[k];
            }
        }
        uint64_t last = d->sectors - 1;
        uint32_t entries_crc = err ? 0 : crc32(entries, GPT_ENTRIES * GPT_ENTRY_SIZE);
        for (int copy = 0; copy < 2 && !err; copy++) {
            /* copy 0: the primary (LBA 1, entries from 2); copy 1: the backup (last LBA, entries before it) */
            uint64_t here = copy ? last : 1, other = copy ? 1 : last;
            uint64_t entries_lba = copy ? last - es : 2;
            memset(hdr, 0, ss);
            memcpy(hdr, "EFI PART", 8);
            wr32(hdr + 8, 0x00010000);
            wr32(hdr + 12, 92);
            wr64(hdr + 24, here);
            wr64(hdr + 32, other);
            wr64(hdr + 40, t->first_usable);
            wr64(hdr + 48, t->last_usable);
            memcpy(hdr + 56, t->disk_guid, 16);
            wr64(hdr + 72, entries_lba);
            wr32(hdr + 80, GPT_ENTRIES);
            wr32(hdr + 84, GPT_ENTRY_SIZE);
            wr32(hdr + 88, entries_crc);
            wr32(hdr + 16, crc32(hdr, 92));
            err = io_at(fd, entries_lba * ss, entries, ebytes, true);
            if (!err) {
                err = io_at(fd, here * ss, hdr, ss, true);
            }
        }
        if (!err) {
            err = io_at(fd, 0, mbr, ss, true);
        }
        free(entries);
        free(hdr);
    }
    if (!err) {
        err = fsync(fd);
    }
    free(mbr);
    close(fd);
    return err;
}

int pt_wipe(const struct disk *d) {
    int fd = open_disk(d, O_RDWR);
    if (fd < 0) {
        return fd;
    }
    size_t len = MIB;
    uint8_t *zero = calloc(1, len);
    int err = zero ? io_at(fd, 0, zero, len, true) : -ENOMEM;
    uint64_t bytes = disk_bytes(d);
    if (!err && bytes > 2 * len) {
        err = io_at(fd, bytes - len, zero, len, true);   /* a GPT backup lives at the end */
    }
    if (!err) {
        err = fsync(fd);
    }
    free(zero);
    close(fd);
    return err;
}

struct pt_entry *pt_find(struct ptable *t, int number) {
    for (int i = 0; i < t->count; i++) {
        if (t->e[i].number == number) {
            return &t->e[i];
        }
    }
    return NULL;
}

int pt_add(struct ptable *t, uint64_t start, uint64_t sectors, const char *type_guid, uint8_t mbr_type,
           const char *name) {
    if (t->count >= PT_MAX || !sectors || start < t->first_usable || start + sectors - 1 > t->last_usable) {
        return -EINVAL;
    }
    for (int i = 0; i < t->count; i++) {
        const struct pt_entry *p = &t->e[i];
        if (start < p->start + p->sectors && p->start < start + sectors) {
            return -EINVAL;   /* overlaps */
        }
    }
    /* The lowest free number. */
    int max = strcmp(t->type, "gpt") == 0 ? GPT_ENTRIES : 4;
    int number = 0;
    for (int n = 1; n <= max && !number; n++) {
        if (!pt_find(t, n)) {
            number = n;
        }
    }
    if (!number) {
        return -ENOSPC;
    }
    struct pt_entry *p = &t->e[t->count++];
    memset(p, 0, sizeof(*p));
    p->number = number;
    p->start = start;
    p->sectors = sectors;
    p->mbr_type = mbr_type;
    if (type_guid) {
        guid_parse(type_guid, p->type_guid);
    }
    guid_random(p->guid);
    if (name) {
        strlcpy(p->name, name, sizeof(p->name));
    }
    return number;
}

int pt_remove(struct ptable *t, int number) {
    for (int i = 0; i < t->count; i++) {
        if (t->e[i].number == number) {
            t->e[i] = t->e[--t->count];
            return 0;
        }
    }
    return -ENOENT;
}

uint64_t pt_free_after(const struct disk *d, const struct ptable *t, uint64_t from, int except_number) {
    (void)d;
    uint64_t end = t->last_usable + 1;
    for (int i = 0; i < t->count; i++) {
        const struct pt_entry *p = &t->e[i];
        if (p->number != except_number && p->start >= from && p->start < end) {
            end = p->start;
        }
    }
    return end > from ? end - from : 0;
}
