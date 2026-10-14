/*
 * The desktop's sounds (<sound.h>): Startup when it starts, DeviceAdd/DeviceRemove when a disk is
 * plugged in or taken out, VolumeChange as feedback when the volume is changed. (Shutdown is played
 * by the power menu; dialogs play their own.)
 */

#include <sound.h>
#include <string.h>

#include "desktop.h"

#define MAX_DISKS     16
#define SETTLE_MS     10000   /* disks found this soon after starting were there all along */
#define CHECK_MS      1000
#define VOLUME_GAP_MS 150     /* the wheel turns fast: one blip per this much at most */

static char known[MAX_DISKS][16];
static int known_count;
static uint64_t started_ms, checked_ms, volume_sound_ms;

/* The disks plugged in from outside (USB): those are the ones that come and go. */
static int list_disks(char out[MAX_DISKS][16]) {
    static struct os_block_info info;
    int n = 0;
    for (int i = 0; blockdev(i, &info) == 0 && n < MAX_DISKS; i++) {
        if ((info.flags & OS_BLOCK_DISK) && (info.flags & OS_BLOCK_EXTERNAL)) {
            strlcpy(out[n++], info.name, sizeof(out[0]));
        }
    }
    return n;
}

static bool in_list(char list[MAX_DISKS][16], int n, const char *name) {
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i], name) == 0) {
            return true;
        }
    }
    return false;
}

void sounds_init(struct desktop *d) {
    (void)d;
    started_ms = checked_ms = uptime_ms();
    known_count = list_disks(known);   /* (the Startup sound is the login screen's) */
}

void sounds_tick(struct desktop *d) {
    (void)d;
    uint64_t now = uptime_ms();
    if (now - checked_ms < CHECK_MS) {
        return;
    }
    checked_ms = now;
    char current[MAX_DISKS][16];
    int n = list_disks(current);
    if (now - started_ms >= SETTLE_MS) {
        bool added = false, removed = false;
        for (int i = 0; i < n; i++) {
            added |= !in_list(known, known_count, current[i]);
        }
        for (int i = 0; i < known_count; i++) {
            removed |= !in_list(current, n, known[i]);
        }
        if (added) {
            system_sound("DeviceAdd");
        } else if (removed) {
            system_sound("DeviceRemove");
        }
    }
    memcpy(known, current, sizeof(current));
    known_count = n;
}

void sounds_volume_changed(struct desktop *d) {
    (void)d;
    uint64_t now = uptime_ms();
    if (now - volume_sound_ms >= VOLUME_GAP_MS) {
        volume_sound_ms = now;
        system_sound("VolumeChange");
    }
}
