#pragma once

/*
 * The viralOS installer: puts the running system on a disk so it starts from it (and keeps what's
 * done on it). Runs in the live system (the ISO), whose root filesystem is in RAM.
 *
 *   main.c     the wizard: language, the license ("yes" to go on), the disk, the account, a
 *              summary, then the installation's progress
 *   install.c  the work, a step at a time so the window stays alive:
 *                1. unmount what's on the disk
 *                2. a new partition table: a boot partition (FAT32, 256 MB) and the system (ext4)
 *                   on the rest; GPT with the boot partition an EFI System Partition (UEFI) or
 *                   MBR (BIOS), the way this PC started
 *                3. mkfs.fat and mke2fs; mounted at /mnt/boot and /mnt/target
 *                4. the system copied over (the live user's home becomes the new user's), each
 *                   file keeping its permissions
 *                5. the account (/etc/passwd, shadow, group), the computer's name, the language
 *                6. the bootloader on the boot partition: Limine, started by the firmware (UEFI:
 *                   /EFI/BOOT/BOOTX64.EFI; BIOS: "limine bios-install"), the kernel, and
 *                   limine.conf saying "root=UUID=<the ext4 partition>" so the kernel mounts it as /
 *
 * Why two partitions: Limine (and UEFI) start from FAT; the system is on ext4, which the kernel
 * writes itself (lwext4) and which keeps permissions.
 */

#include <gui.h>
#include <stdbool.h>
#include <stdint.h>
#include <viralos.h>

#define MIN_DISK_BYTES (1024ull * 1024 * 1024)   /* 1 GiB */
#define TARGET         "/mnt/target"               /* the system partition, while installing */
#define BOOT_TARGET    "/mnt/boot"                 /* the boot partition */
#define LIVE_USER      "user"                    /* the live system's account: it doesn't come along */
#define INSTALLER_APP  _PATH_APPLICATIONS "/divine-installer.desktop"
#define INSTALLER_LINK "Install viralOS.desktop"   /* on the live user's desktop */

struct choices {
    char language[16];            /* "en_US" */
    char disk[16];                /* "sata0" */
    char disk_model[48];
    uint64_t disk_bytes;
    uint32_t sector_size;
    bool uefi;                    /* set up for UEFI (else BIOS) */
    char display[64];             /* "Alex Smith" */
    char user[OS_USER_MAX];       /* "alex" */
    char password[128];
    bool admin;
    char hostname[64];
};

enum install_state { INSTALL_RUNNING, INSTALL_DONE, INSTALL_FAILED };

/* install.c */
void install_start(const struct choices *c);
/* Do the next piece of work (call again and again). */
enum install_state install_step(void);
float install_progress(void);         /* 0..1 */
const char *install_status(void);     /* what it's doing now, or why it failed */
