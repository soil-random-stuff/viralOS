/* unmount_drives_all: before turning the computer off (see <unistd.h>). */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_DRIVES 16

int unmount_drives_all(char *failed, size_t size) {
    static char paths[MAX_DRIVES][OS_PATH_MAX];
    static struct os_drive_info info;
    int n = 0;
    while (n < MAX_DRIVES && drives(n, &info) == 0) {
        strlcpy(paths[n++], info.path, sizeof(paths[0]));
    }
    int bad = 0;
    if (failed && size) {
        failed[0] = '\0';
    }
    for (int i = 0; i < n; i++) {
        int r = unmount(paths[i]);
        if (r == 0 || r == -ENOENT) {   /* (gone already: another of its disk's volumes took it along) */
            continue;
        }
        bad++;
        if (failed && size) {
            char one[OS_PATH_MAX + 48];
            snprintf(one, sizeof(one), "%s%s (%s)", failed[0] ? ", " : "", paths[i],
                     r == -EBUSY ? "in use" : strerror(r));
            strlcat(failed, one, size);
        }
    }
    return bad;
}
