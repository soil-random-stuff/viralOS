/* uptime: how long the system has been running. */

#include <stdio.h>
#include <unistd.h>

int main(void) {
    uint64_t ms = uptime_ms();
    uint64_t s = ms / 1000;
    printf("up %lu:%02lu:%02lu (%lu.%03lu s)\n", s / 3600, s / 60 % 60, s % 60, s, ms % 1000);
    return 0;
}
