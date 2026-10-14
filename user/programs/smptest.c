/* smptest [seconds]: busy floating-point work for a few seconds, then how much got done (and whether
   the answers stayed right: they would go wrong if the FPU registers got mixed up between CPUs).
   Start a few at once (smptest & smptest & ...): on several CPUs each does about as much as one alone. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    uint64_t seconds = argc > 1 ? (uint64_t)atoi(argv[1]) : 3;
    uint64_t end = uptime_ms() + seconds * 1000;
    uint64_t rounds = 0, wrong = 0;
    while (uptime_ms() < end) {
        /* sum of 1/k^2 for k = 1..2000, about pi^2/6 */
        double sum = 0;
        for (int k = 1; k <= 2000; k++) {
            sum += 1.0 / ((double)k * (double)k);
        }
        if (sum < 1.6444 || sum > 1.6445) {
            wrong++;
        }
        rounds++;
    }
    printf("smptest (pid %ld): %lu rounds in %lu s, %lu wrong\n", (long)getpid(), (unsigned long)rounds,
           (unsigned long)seconds, (unsigned long)wrong);
    return wrong ? 1 : 0;
}
