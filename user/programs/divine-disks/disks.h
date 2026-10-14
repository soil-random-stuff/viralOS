#pragma once

/*
 * Disks: see the disks and their partitions, format, partition, resize,
 * label and mount them. Shaped after GNOME Disks, simpler.
 *
 *   model.c   what's there: disks and partitions (blockdev()), the
 *             filesystems on them (read from /dev), names for things
 *   part.c    reading and writing partition tables (GPT, MBR)
 *   jobs.c    running the disk tools (mkfs.*, tune2fs, resize2fs...: ported
 *             Linux programs) and other steps one after the other
 *   main.c    the window: drawing, clicks, dialogs, menus
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <viralos.h>

#define MAX_DISKS    16
#define MAX_VOLUMES  64
#define MIB          (1024ull * 1024)

enum vol_kind { VOL_PARTITION, VOL_FREE, VOL_WHOLE };

struct volume {
    enum vol_kind kind;
    char dev[16];                /* /dev name (partition, or the disk for VOL_WHOLE); "" for free space */
    int number;                  /* partition number */
    uint64_t start, sectors;     /* on the disk */
    uint32_t mbr_type;
    uint8_t type_guid[16];
    char part_name[72];
    bool extended;               /* an MBR extended partition (holds the logical ones) */
    bool logical;
    /* The filesystem on it. */
    char fs[16];                 /* "vfat", "exfat", "ext4", "ext3", "ext2", "ntfs", "swap", "" */
    char fs_desc[48];            /* "FAT (32-bit version)", "Unknown" */
    char label[OS_NAME_MAX];
    char uuid[40];
    char mounted_at[OS_PATH_MAX];
    uint64_t fs_size, fs_free;   /* if mounted (0: not known) */
    bool mounted_ro;             /* mounted read-only */
};

struct disk {
    char name[16];               /* "usb0" */
    char model[48];
    char serial[40];
    uint64_t sectors;
    uint32_t sector_size;
    uint32_t flags;              /* OS_BLOCK_* */
    char table[8];               /* "gpt", "mbr", "" */
    struct volume vols[MAX_VOLUMES];
    int nvols;
};

extern struct disk disks[MAX_DISKS];
extern int disk_count;

/* --- model.c ------------------------------------------------------------------------------------------- */

void model_refresh(void);
void model_forget(void);                         /* re-read every volume on the next refresh */
uint64_t disk_bytes(const struct disk *d);
uint64_t vol_bytes(const struct disk *d, const struct volume *v);
void human_size(uint64_t bytes, char *out, size_t size);           /* "1.0 TB", "524 MB" */
void disk_title(const struct disk *d, char *out, size_t size);     /* "1.0 TB Drive" */
const char *part_type_name(const struct disk *d, const struct volume *v);
const char *fs_short(const struct volume *v);                      /* "FAT", "exFAT", "NTFS", "Ext4" */
void guid_string(const uint8_t *g, char *out);                     /* 37 bytes */
bool guid_parse(const char *s, uint8_t *g);
void guid_random(uint8_t *g);

/* Common partition types to offer. */
struct part_type {
    const char *name;
    const char *guid;            /* GPT */
    uint8_t mbr;                 /* MBR */
};
extern const struct part_type part_types[];
extern const int part_type_count;

/* --- part.c -------------------------------------------------------------------------------------------- */

#define PT_MAX 128

struct pt_entry {
    int number;                  /* 1.. (GPT: entry index + 1, MBR: slot + 1) */
    uint64_t start, sectors;
    uint8_t mbr_type;
    uint8_t type_guid[16];
    uint8_t guid[16];
    uint64_t attrs;
    char name[72];               /* GPT name (UTF-8) */
    bool boot;                   /* MBR active flag */
};

struct ptable {
    char type[8];                /* "gpt", "mbr" */
    struct pt_entry e[PT_MAX];
    int count;
    uint8_t disk_guid[16];
    uint64_t first_usable, last_usable;
    uint8_t mbr_head[446];       /* MBR: boot code, kept */
    uint32_t mbr_signature;
    bool has_logical;            /* MBR with an extended partition: not editable here */
};

int pt_read(const struct disk *d, struct ptable *t);
void pt_new(const struct disk *d, struct ptable *t, const char *type);
int pt_write(const struct disk *d, const struct ptable *t);
/* Wipe partition tables and filesystem signatures at the start and end of the disk ("no partitioning"). */
int pt_wipe(const struct disk *d);
struct pt_entry *pt_find(struct ptable *t, int number);
/* Add a partition in [start, start + sectors); returns its number, or -errno. */
int pt_add(struct ptable *t, uint64_t start, uint64_t sectors, const char *type_guid, uint8_t mbr_type, const char *name);
int pt_remove(struct ptable *t, int number);
/* 1 MiB alignment, in sectors */
uint64_t pt_align(const struct disk *d);
/* The biggest free stretch starting at `from`, up to the next partition / the end. */
uint64_t pt_free_after(const struct disk *d, const struct ptable *t, uint64_t from, int except_number);

/* --- jobs.c ------------------------------------------------------------------------------------------------ */

enum step_kind { STEP_RUN, STEP_UNMOUNT, STEP_MOUNT, STEP_RESCAN, STEP_WAIT_DEVICE, STEP_WRITE_TABLE, STEP_WIPE,
                 STEP_SAVE_MOUNT_OPTIONS };

#define JOB_STEPS 12

struct step {
    enum step_kind kind;
    char args[16][160];          /* STEP_RUN: argv; others: [0] device/path, [1] path */
    int argc;
    bool optional;               /* a failure doesn't stop the job */
};

struct job {
    char title[96];              /* "Formatting usb0p1..." */
    struct step steps[JOB_STEPS];
    int count;
    struct ptable table;         /* for STEP_WRITE_TABLE */
    int disk;                    /* index in disks[] for table steps */
    char mount_uuid[40];         /* STEP_SAVE_MOUNT_OPTIONS */
    char mount_path[OS_PATH_MAX];
    bool mount_auto;
    bool mount_rw;
};

void job_begin(const char *title);
struct job *job_current(void);
void job_run(const char *tool, ...);              /* a program and its arguments, NULL-terminated */
void job_step(enum step_kind kind, const char *a, const char *b);
void job_optional(void);                          /* the last step may fail */
void job_start(void);
bool job_busy(void);
/* Advance a running job (call often); true when it just finished. *ok / *message say how it went. */
bool job_poll(bool *ok, char *message, size_t size);
const char *job_status(void);                     /* what it's doing now */

/* Mount options (/etc/fstab, see <fstab.h>): where the filesystem with this UUID goes, whether
   automatically, and rw (NTFS/ext4 read-write). */
bool mount_options_get(const char *uuid, char *path, size_t size, bool *automatic, bool *rw);
int mount_options_set(const char *uuid, const char *path, bool automatic, bool rw);
