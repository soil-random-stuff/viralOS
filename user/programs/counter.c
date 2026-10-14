/* counter [n]: count to n (forever if omitted), one number every half second.
   Handy for trying out background jobs (counter &), ps, kill and Ctrl+C. */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    long limit = 0;
    if (argc > 1) {
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) {
            limit = limit * 10 + (*p - '0');
        }
    }
    long pid = getpid();
    for (long i = 1; limit == 0 || i <= limit; i++) {
        printf("[counter %ld] %ld\n", pid, i);
        sleep_ms(500);
    }
    return 0;
}
