/* The installation itself, a step at a time (see installer.h). */

#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <users.h>

#include "installer.h"

#define LOG_DIR     _PATH_TMP   /* each tool's output: /tmp/installer-<tool>.log */
#define COPY_CHUNK  (256 * 1024)        /* bytes copied per step */
#define MAX_ENTRIES 4096
#define BOOT_MIB    256                 /* the boot partition */

enum stage {
    ST_UNMOUNT, ST_PARTITION, ST_WAIT_PARTITIONS, ST_FORMAT_BOOT, ST_FORMATTING_BOOT, ST_FORMAT_ROOT,
    ST_FORMATTING_ROOT, ST_MOUNT, ST_SCAN, ST_COPY, ST_CONFIGURE, ST_BOOT_FILES, ST_UNMOUNT_TARGET,
    ST_BIOS_INSTALL, ST_BIOS_INSTALLING, ST_DONE,
};

struct entry {
    char *from, *to;
    bool dir;
    uint32_t mode;
    uint64_t size;
};

static struct {
    struct choices c;
    enum stage stage;
    enum install_state state;
    char status[256];
    char part_boot[16], part_root[16];   /* "sata0p1", "sata0p2" */
    uint64_t started_ms;
    long pid;                    /* mkfs.fat / mke2fs / limine running */
    char log[64];                /* its output */

    struct entry *entries;
    int count, next;
    uint64_t total_bytes, copied_bytes;
    int copy_fd_in, copy_fd_out; /* a file being copied, a piece at a time */
} in;

static void fail(const char *fmt, const char *a, int err) {
    char what[200];
    snprintf(what, sizeof(what), fmt, a ? a : "");
    if (err) {
        snprintf(in.status, sizeof(in.status), "%s: %s.", what, strerror(err));
    } else {
        snprintf(in.status, sizeof(in.status), "%s.", what);
    }
    in.state = INSTALL_FAILED;
}

static void status(const char *text) {
    strlcpy(in.status, text, sizeof(in.status));
}

/* --- random numbers (GUIDs, the disk signature) -------------------------------------------------- */

static uint64_t rng;

static uint64_t random64(void) {
    if (!rng) {
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        rng = ((uint64_t)hi << 32 | lo) ^ uptime_us() ^ 0x9E3779B97F4A7C15ull;
    }
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void random_guid(uint8_t g[16]) {
    uint64_t a = random64(), b = random64();
    memcpy(g, &a, 8);
    memcpy(g + 8, &b, 8);
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);   /* version 4 (the third field is little-endian) */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);
}

/* --- the partition table ---------------------------------------------------------------------------- */

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

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void wr64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static int write_at(int fd, uint64_t offset, const void *buf, size_t len) {
    if (seek(fd, (long)offset, OS_SEEK_SET) < 0) {
        return -EIO;
    }
    long w = write(fd, buf, len);
    return w == (long)len ? 0 : w < 0 ? (int)w : -EIO;
}

/* An MBR partition entry: CHS left at "past the end" (every system since the 90s uses the LBAs). */
static void mbr_entry(uint8_t *e, bool active, uint8_t type, uint64_t start, uint64_t sectors) {
    e[0] = active ? 0x80 : 0;
    e[1] = 0xFE, e[2] = 0xFF, e[3] = 0xFF;
    e[4] = type;
    e[5] = 0xFE, e[6] = 0xFF, e[7] = 0xFF;
    wr32(e + 8, (uint32_t)start);
    wr32(e + 12, (uint32_t)(sectors > 0xFFFFFFFFull ? 0xFFFFFFFFull : sectors));
}

static void gpt_entry(uint8_t *e, const uint8_t type[16], uint64_t first, uint64_t last, const char *name) {
    memcpy(e, type, 16);
    random_guid(e + 16);
    wr64(e + 32, first);
    wr64(e + 40, last);
    for (int i = 0; name[i]; i++) {
        wr16(e + 56 + i * 2, (uint16_t)name[i]);
    }
}

/*
 * Two partitions: the boot partition (FAT32, BOOT_MIB) from 1 MiB, then the system (ext4) to the
 * end. UEFI: GPT, the boot partition an EFI System Partition. BIOS: MBR, the boot partition active
 * and typed as an EFI system one too (0xEF: so it isn't shown as a drive; Limine doesn't mind).
 */
static int write_table(void) {
    char path[32];
    snprintf(path, sizeof(path), _PATH_DEV "/%s", in.c.disk);
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        return fd;
    }
    uint32_t ss = in.c.sector_size ? in.c.sector_size : 512;
    uint64_t sectors = in.c.disk_bytes / ss;
    uint64_t boot_start = (1024 * 1024) / ss, boot_count = (BOOT_MIB * 1024ull * 1024) / ss;
    uint64_t root_start = boot_start + boot_count;
    int err = 0;

    /* Wipe old tables, and old filesystems where the partitions will start. */
    static uint8_t zero[64 * 1024];
    for (uint64_t off = 0; off < 1024 * 1024 + sizeof(zero) && !err; off += sizeof(zero)) {
        err = write_at(fd, off, zero, sizeof(zero));
    }
    for (uint64_t off = 0; off < 1024 * 1024 && !err; off += sizeof(zero)) {
        err = write_at(fd, root_start * ss + off, zero, sizeof(zero));
    }

    uint8_t *sector = calloc(1, ss);
    uint8_t *entries = calloc(1, 128 * 128);
    if (!sector || !entries) {
        err = -ENOMEM;
    }
    if (!err && !in.c.uefi) {
        uint64_t root_count = sectors - root_start;
        root_count = root_count > 0xFFFFFFFFull - root_start ? 0xFFFFFFFFull - root_start : root_count;
        wr32(sector + 440, (uint32_t)random64());   /* disk signature */
        mbr_entry(sector + 446, true, 0xEF, boot_start, boot_count);
        mbr_entry(sector + 462, false, 0x83, root_start, root_count);   /* Linux filesystem */
        sector[510] = 0x55, sector[511] = 0xAA;
        err = write_at(fd, 0, sector, ss);
    } else if (!err) {
        uint64_t entry_sectors = (128 * 128 + ss - 1) / ss;
        uint64_t last_usable = sectors - 1 - entry_sectors - 1;

        /* The protective MBR. */
        mbr_entry(sector + 446, false, 0xEE, 1, sectors - 1);
        sector[510] = 0x55, sector[511] = 0xAA;
        err = write_at(fd, 0, sector, ss);

        static const uint8_t esp[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                                         0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
        static const uint8_t linux_fs[16] = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
                                              0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };
        gpt_entry(entries, esp, boot_start, root_start - 1, "viralOS boot");
        gpt_entry(entries + 128, linux_fs, root_start, last_usable, "viralOS");
        uint32_t entries_crc = crc32(entries, 128 * 128);
        uint8_t disk_guid[16];
        random_guid(disk_guid);

        /* The primary header (LBA 1) and the backup (last LBA), entries after the primary and
           before the backup. */
        for (int backup = 0; backup < 2 && !err; backup++) {
            uint64_t here = backup ? sectors - 1 : 1, other = backup ? 1 : sectors - 1;
            uint64_t entries_lba = backup ? sectors - 1 - entry_sectors : 2;
            memset(sector, 0, ss);
            memcpy(sector, "EFI PART", 8);
            wr32(sector + 8, 0x00010000);
            wr32(sector + 12, 92);
            wr64(sector + 24, here);
            wr64(sector + 32, other);
            wr64(sector + 40, 2 + entry_sectors);
            wr64(sector + 48, last_usable);
            memcpy(sector + 56, disk_guid, 16);
            wr64(sector + 72, entries_lba);
            wr32(sector + 80, 128);
            wr32(sector + 84, 128);
            wr32(sector + 88, entries_crc);
            wr32(sector + 16, crc32(sector, 92));
            err = write_at(fd, entries_lba * ss, entries, 128 * 128);
            if (!err) {
                err = write_at(fd, here * ss, sector, ss);
            }
        }
    }
    if (!err) {
        err = fsync(fd);
    }
    free(sector);
    free(entries);
    close(fd);
    return err;
}

/* --- running a tool ------------------------------------------------------------------------------- */

static long run(const char *const *argv) {
    char path[OS_PATH_MAX];
    snprintf(path, sizeof(path), _PATH_BIN "/%s", argv[0]);
    snprintf(in.log, sizeof(in.log), LOG_DIR "/installer-%s.log", argv[0]);
    int log = open(in.log, O_WRONLY | O_CREAT | O_TRUNC);
    struct os_spawn sp = { .path = path, .argv = argv, .fds = { OS_SPAWN_INHERIT, log, log } };
    long pid = spawn(&sp);
    if (log >= 0) {
        close(log);
    }
    return pid;
}

/* Has the tool finished? 0: still running; 1: fine; < 0: it failed (status in in.status). */
static int finished(const char *what) {
    int code = 0;
    long r = wait(in.pid, &code, WNOHANG);
    if (r == 0) {
        return 0;
    }
    if (r < 0 || code != 0) {
        char tail[160] = "";
        int fd = open(in.log, O_RDONLY);
        if (fd >= 0) {
            static char buf[4096];
            long n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            buf[n > 0 ? n : 0] = '\0';
            /* The last line that says something. */
            size_t len = strlen(buf);
            while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
                buf[--len] = '\0';
            }
            char *last = strrchr(buf, '\n');
            strlcpy(tail, last ? last + 1 : buf, sizeof(tail));
        }
        snprintf(in.status, sizeof(in.status), "%s failed (exit status %d)%s%s", what, code, tail[0] ? ": " : ".", tail);
        in.state = INSTALL_FAILED;
        return -1;
    }
    return 1;
}

/* --- copying the system --------------------------------------------------------------------------- */

static char *dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

static void add(const char *from, const char *to, bool dir, uint32_t mode, uint64_t size) {
    if (in.count == MAX_ENTRIES) {
        return;
    }
    struct entry *e = &in.entries[in.count++];
    e->from = from ? dup(from) : NULL;
    e->to = dup(to);
    e->dir = dir;
    e->mode = mode & 07777;
    e->size = size;
    in.total_bytes += size;
}

/* Paths that don't come along: the live system's own, the boot files (they go to the boot
   partition), and the installer's launchers. */
static bool skipped(const char *path) {
    static const char *const skip[] = {
        "/dev", "/media", "/mnt", "/tmp", "/run", "/home", "/boot", INSTALLER_APP,
        _PATH_HOME "/" LIVE_USER "/.config", _PATH_HOME "/" LIVE_USER "/Desktop/" INSTALLER_LINK,
    };
    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
        if (strcmp(path, skip[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Everything under `from` (a folder), to be copied under `to`. */
static void scan(const char *from, const char *to) {
    int fd = open(from, O_RDONLY);
    if (fd < 0) {
        return;
    }
    struct os_file_info info;
    for (uint64_t i = 0; readdir(fd, i, &info) == 0; i++) {
        char src[OS_PATH_MAX], dst[OS_PATH_MAX];
        snprintf(src, sizeof(src), "%s%s%s", from, strcmp(from, "/") == 0 ? "" : "/", info.name);
        snprintf(dst, sizeof(dst), "%s/%s", to, info.name);
        if (skipped(src)) {
            continue;
        }
        if (info.type == OS_DIRECTORY) {
            add(NULL, dst, true, info.mode, 0);
            scan(src, dst);
        } else {
            add(src, dst, false, info.mode, info.size);
        }
    }
    close(fd);
}

/* Copy a whole small file (the boot files). */
static int copy_file(const char *from, const char *to) {
    int src = open(from, O_RDONLY);
    int dst = open(to, O_WRONLY | O_CREAT | O_TRUNC);
    static uint8_t buf[64 * 1024];
    long n = 0;
    while (src >= 0 && dst >= 0 && (n = read(src, buf, sizeof(buf))) > 0) {
        if (write(dst, buf, (size_t)n) != n) {
            n = -EIO;
            break;
        }
    }
    int err = src < 0 ? src : dst < 0 ? dst : n < 0 ? (int)n : 0;
    if (src >= 0) {
        close(src);
    }
    if (dst >= 0) {
        close(dst);
    }
    return err;
}

/* Copy some more. True when everything is copied. */
static bool copy_some(void) {
    uint64_t budget = COPY_CHUNK;
    static uint8_t buf[64 * 1024];
    while (budget > 0 && in.next < in.count) {
        struct entry *e = &in.entries[in.next];
        if (e->dir) {
            mkdir(e->to);
            chmod(e->to, e->mode ? e->mode : 0755);
            in.next++;
            continue;
        }
        if (in.copy_fd_in < 0) {
            in.copy_fd_in = open(e->from, O_RDONLY);
            in.copy_fd_out = open(e->to, O_WRONLY | O_CREAT | O_TRUNC);
            if (in.copy_fd_in < 0 || in.copy_fd_out < 0) {
                fail("Couldn't copy %s", e->from, in.copy_fd_in < 0 ? in.copy_fd_in : in.copy_fd_out);
                return false;
            }
        }
        long n = read(in.copy_fd_in, buf, sizeof(buf));
        if (n > 0) {
            long w = write(in.copy_fd_out, buf, (size_t)n);
            if (w != n) {
                fail("Couldn't write %s", e->to, w < 0 ? (int)w : -ENOSPC);
                return false;
            }
            in.copied_bytes += (uint64_t)n;
            budget = (uint64_t)n >= budget ? 0 : budget - (uint64_t)n;
            continue;
        }
        close(in.copy_fd_in);
        close(in.copy_fd_out);
        in.copy_fd_in = in.copy_fd_out = -1;
        if (n < 0) {
            fail("Couldn't read %s", e->from, (int)n);
            return false;
        }
        chmod(e->to, e->mode ? e->mode : 0644);   /* (programs keep their 0755) */
        in.next++;
    }
    return in.next >= in.count;
}

/* --- the new system's settings -------------------------------------------------------------------- */

static int write_text(const char *path, const char *text, uint32_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        return fd;
    }
    long len = (long)strlen(text);
    long w = write(fd, text, (size_t)len);
    close(fd);
    chmod(path, mode);
    return w == len ? 0 : -EIO;
}

static int configure(void) {
    static char text[4096], hash[160];
    long days = (long)(time(NULL) / 86400);
    const struct choices *c = &in.c;
    users_hash_password(c->password, hash, sizeof(hash));

    snprintf(text, sizeof(text), "root:x:0:0:root:/root:" _PATH_SHELL "\n%s:x:1000:1000:%s:" _PATH_HOME "/%s:" _PATH_SHELL "\n",
             c->user, c->display, c->user);
    int err = write_text(TARGET _PATH_PASSWD, text, 0644);
    snprintf(text, sizeof(text), "root:!:%ld:0:99999:7:::\n%s:%s:%ld:0:99999:7:::\n", days, c->user, hash, days);
    err = err ? err : write_text(TARGET _PATH_SHADOW, text, 0600);
    snprintf(text, sizeof(text), "root:x:0:\nsudo:x:27:%s\n%s:x:1000:\n", c->admin ? c->user : "", c->user);
    err = err ? err : write_text(TARGET _PATH_GROUP, text, 0644);
    snprintf(text, sizeof(text), "%s\n", c->hostname);
    err = err ? err : write_text(TARGET _PATH_HOSTNAME, text, 0644);
    snprintf(text, sizeof(text), "LANG=%s.UTF-8\n", c->language);
    err = err ? err : write_text(TARGET _PATH_ETC "/locale.conf", text, 0644);
    if (err) {
        return err;
    }

    /* The home folder's usual folders (the live user's files are in it already). */
    static const char *const folders[] = { "", "/Desktop", "/Documents", "/Downloads", "/Music", "/Pictures",
                                           "/.config", "/" _PATH_DDE_USER_DIR };
    for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
        char path[OS_PATH_MAX];
        snprintf(path, sizeof(path), TARGET _PATH_HOME "/%s%s", c->user, folders[i]);
        mkdir(path);
    }
    /* Empty folders the system fills while it runs. */
    static const char *const empty[] = { "/dev", "/media", "/mnt", "/run", "/boot" };
    for (size_t i = 0; i < sizeof(empty) / sizeof(empty[0]); i++) {
        char path[OS_PATH_MAX];
        snprintf(path, sizeof(path), TARGET "%s", empty[i]);
        mkdir(path);
    }
    mkdir(TARGET "/tmp");
    chmod(TARGET "/tmp", 01777);
    return 0;
}

/* The boot partition: the kernel, Limine and its settings ("root=UUID=<the system partition>"),
   and for UEFI the firmware's own boot file. */
static int boot_files(void) {
    static struct os_drive_info info;
    char uuid[40] = "";
    for (int i = 0; drives(i, &info) == 0; i++) {
        if (strcmp(info.path, TARGET) == 0) {
            strlcpy(uuid, info.uuid, sizeof(uuid));
        }
    }
    if (!uuid[0]) {
        return -ENOENT;
    }
    mkdir(BOOT_TARGET "/boot");
    mkdir(BOOT_TARGET "/boot/limine");
    int err = copy_file("/boot/kernel.elf", BOOT_TARGET "/boot/kernel.elf");
    if (!err) {
        err = copy_file("/boot/limine/limine-bios.sys", BOOT_TARGET "/boot/limine/limine-bios.sys");
    }
    static char conf[512];
    snprintf(conf, sizeof(conf),
             "# Limine's settings for viralOS (written by the installer)\n"
             "timeout: 3\n\n"
             "/viralOS\n"
             "    protocol: limine\n"
             "    path: boot():/boot/kernel.elf\n"
             "    cmdline: root=UUID=%s\n", uuid);
    if (!err) {
        err = write_text(BOOT_TARGET "/boot/limine/limine.conf", conf, 0644);
    }
    if (!err && in.c.uefi) {
        mkdir(BOOT_TARGET "/EFI");
        mkdir(BOOT_TARGET "/EFI/BOOT");
        err = copy_file("/boot/limine/BOOTX64.EFI", BOOT_TARGET "/EFI/BOOT/BOOTX64.EFI");
    }
    return err;
}

/* --- the steps ------------------------------------------------------------------------------------ */

void install_start(const struct choices *c) {
    memset(&in, 0, sizeof(in));
    in.c = *c;
    in.state = INSTALL_RUNNING;
    in.copy_fd_in = in.copy_fd_out = -1;
    in.entries = calloc(MAX_ENTRIES, sizeof(struct entry));
    status("Getting the disk ready");
}

float install_progress(void) {
    /* Rough shares: preparing 12%, copying 80%, the rest 8%. */
    switch (in.stage) {
    case ST_UNMOUNT: case ST_PARTITION: case ST_WAIT_PARTITIONS: return 0.02f;
    case ST_FORMAT_BOOT: case ST_FORMATTING_BOOT: return 0.04f;
    case ST_FORMAT_ROOT: case ST_FORMATTING_ROOT: return 0.07f;
    case ST_MOUNT: case ST_SCAN: return 0.12f;
    case ST_COPY:
        return 0.12f + 0.8f * (in.total_bytes ? (float)in.copied_bytes / (float)in.total_bytes : 0);
    case ST_CONFIGURE: case ST_BOOT_FILES: return 0.93f;
    case ST_UNMOUNT_TARGET: case ST_BIOS_INSTALL: case ST_BIOS_INSTALLING: return 0.97f;
    default: return 1;
    }
}

const char *install_status(void) {
    return in.status;
}

/* Unmount whatever is mounted from the disk. */
static int unmount_disk(void) {
    static struct os_drive_info info;
    for (int i = 0; drives(i, &info) == 0;) {
        if (strncmp(info.device, in.c.disk, strlen(in.c.disk)) == 0) {
            int err = unmount(info.path);
            if (err < 0) {
                fail("%s is in use and couldn't be unmounted", info.path, err);
                return err;
            }
            continue;   /* (the list moved up) */
        }
        i++;
    }
    return 0;
}

static bool find_partitions(void) {
    static struct os_block_info b;
    in.part_boot[0] = in.part_root[0] = '\0';
    for (int i = 0; blockdev(i, &b) == 0; i++) {
        if (strcmp(b.disk, in.c.disk) == 0 && b.partition == 1) {
            strlcpy(in.part_boot, b.name, sizeof(in.part_boot));
        } else if (strcmp(b.disk, in.c.disk) == 0 && b.partition == 2) {
            strlcpy(in.part_root, b.name, sizeof(in.part_root));
        }
    }
    return in.part_boot[0] && in.part_root[0];
}

static bool start_tool(const char *const *argv, const char *what) {
    in.pid = run(argv);
    if (in.pid < 0) {
        fail("%s couldn't start", what, (int)in.pid);
        return false;
    }
    return true;
}

enum install_state install_step(void) {
    if (in.state != INSTALL_RUNNING) {
        return in.state;
    }
    int err;
    char dev[32];
    switch (in.stage) {
    case ST_UNMOUNT:
        if (!in.entries) {
            fail("Out of memory", NULL, 0);
            break;
        }
        if (unmount_disk() == 0) {
            in.stage = ST_PARTITION;
        }
        break;
    case ST_PARTITION:
        status(in.c.uefi ? "Making a GPT partition table (UEFI)" : "Making an MBR partition table (BIOS)");
        err = write_table();
        if (err) {
            fail("Couldn't write the partition table", NULL, err);
            break;
        }
        err = rescan(in.c.disk);
        if (err) {
            fail("The disk's new partition table couldn't be read", NULL, err);
            break;
        }
        in.started_ms = uptime_ms();
        in.stage = ST_WAIT_PARTITIONS;
        break;
    case ST_WAIT_PARTITIONS:
        if (find_partitions()) {
            in.stage = ST_FORMAT_BOOT;
        } else if (uptime_ms() - in.started_ms > 8000) {
            fail("The new partitions didn't show up", NULL, 0);
        } else {
            sleep_ms(100);
        }
        break;
    case ST_FORMAT_BOOT: {
        status("Formatting the boot partition (FAT32)");
        snprintf(dev, sizeof(dev), _PATH_DEV "/%s", in.part_boot);
        const char *argv[] = { "mkfs.fat", "-F", "32", "-n", "VIRALBOOT", dev, NULL };
        if (start_tool(argv, "mkfs.fat")) {
            in.stage = ST_FORMATTING_BOOT;
        }
        break;
    }
    case ST_FORMATTING_BOOT:
        if (finished("Formatting the boot partition") > 0) {
            in.stage = ST_FORMAT_ROOT;
        } else {
            sleep_ms(50);
        }
        break;
    case ST_FORMAT_ROOT: {
        status("Formatting the system partition (ext4)");
        snprintf(dev, sizeof(dev), _PATH_DEV "/%s", in.part_root);
        const char *argv[] = { "mke2fs", "-t", "ext4", "-F", "-q", "-L", "viralOS", dev, NULL };
        if (start_tool(argv, "mke2fs")) {
            in.stage = ST_FORMATTING_ROOT;
        }
        break;
    }
    case ST_FORMATTING_ROOT:
        if (finished("Formatting the system partition") > 0) {
            in.stage = ST_MOUNT;
        } else {
            sleep_ms(50);
        }
        break;
    case ST_MOUNT:
        status("Mounting them");
        err = mount_device_writable(in.part_root, TARGET);
        if (!err) {
            err = mount_device(in.part_boot, BOOT_TARGET);
        }
        if (err) {
            fail("The new partitions couldn't be mounted", NULL, err);
            break;
        }
        in.stage = ST_SCAN;
        break;
    case ST_SCAN: {
        status("Looking at what to copy");
        scan("/", TARGET);
        /* The live user's files go into the new user's home. */
        char home[OS_PATH_MAX];
        snprintf(home, sizeof(home), TARGET _PATH_HOME "/%s", in.c.user);
        add(NULL, TARGET _PATH_HOME, true, 0755, 0);
        add(NULL, home, true, 0755, 0);
        scan(_PATH_HOME "/" LIVE_USER, home);
        in.stage = ST_COPY;
        break;
    }
    case ST_COPY: {
        char text[96];
        snprintf(text, sizeof(text), "Copying viralOS (%lu of %lu MB)", (unsigned long)(in.copied_bytes >> 20),
                 (unsigned long)(in.total_bytes >> 20));
        status(text);
        if (copy_some()) {
            in.stage = ST_CONFIGURE;
        }
        break;
    }
    case ST_CONFIGURE:
        status("Setting up your account");
        err = configure();
        if (err) {
            fail("Couldn't set up the account", NULL, err);
            break;
        }
        in.stage = ST_BOOT_FILES;
        break;
    case ST_BOOT_FILES:
        status("Installing the bootloader");
        err = boot_files();
        if (err) {
            fail("Couldn't install the bootloader's files", NULL, err);
            break;
        }
        in.stage = ST_UNMOUNT_TARGET;
        break;
    case ST_UNMOUNT_TARGET:
        status("Writing everything to the disk");
        err = unmount(BOOT_TARGET);
        if (!err) {
            err = unmount(TARGET);
        }
        if (err) {
            fail("The new system couldn't be unmounted", NULL, err);
            break;
        }
        in.stage = in.c.uefi ? ST_DONE : ST_BIOS_INSTALL;
        break;
    case ST_BIOS_INSTALL: {
        status("Making the disk start on this PC (BIOS)");
        snprintf(dev, sizeof(dev), _PATH_DEV "/%s", in.c.disk);
        const char *argv[] = { "limine", "bios-install", dev, NULL };
        if (start_tool(argv, "limine")) {
            in.stage = ST_BIOS_INSTALLING;
        }
        break;
    }
    case ST_BIOS_INSTALLING:
        if (finished("Installing the bootloader") > 0) {
            in.stage = ST_DONE;
        } else {
            sleep_ms(50);
        }
        break;
    case ST_DONE: {
        /* Everything written to the disk itself (the tables, limine's boot code) out of the caches. */
        snprintf(dev, sizeof(dev), _PATH_DEV "/%s", in.c.disk);
        int fd = open(dev, O_RDWR);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
        status("viralOS is installed");
        in.state = INSTALL_DONE;
        break;
    }
    }
    if (in.state == INSTALL_FAILED && in.copy_fd_in >= 0) {
        close(in.copy_fd_in);
        close(in.copy_fd_out);
        in.copy_fd_in = in.copy_fd_out = -1;
    }
    return in.state;
}
