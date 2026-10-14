/* drives: list the mounted drives (USB sticks and disks) with their size and free space. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void human(uint64_t bytes, char *out, size_t size) {
    const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    int u = 0;
    uint64_t whole = bytes, tenths = 0;
    while (whole >= 1000 && u < 4) {
        tenths = whole % 1000 / 100;
        whole /= 1000;
        u++;
    }
    if (u && whole < 10) {
        snprintf(out, size, "%lu.%lu %s", whole, tenths, units[u]);
    } else {
        snprintf(out, size, "%lu %s", whole, units[u]);
    }
}

int main(void) {
    static struct os_drive_info d;
    int n = 0;
    for (; drives(n, &d) == 0; n++) {
        char total[16], free[16];
        human(d.total, total, sizeof(total));
        human(d.free, free, sizeof(free));
        printf("%-24s %-6s %9s %9s free  %s%s (%s)\n", d.path, d.filesystem, total, free,
               d.flags & OS_DRIVE_READ_ONLY ? "read-only, " : "", d.model, d.device);
    }
    if (!n) {
        printf("No drives. Plug in a USB stick: it shows up in /media.\n");
    }
    return 0;
}
