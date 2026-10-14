/*
 * fputest [seed]: check that FPU/SSE registers survive task switches.
 *
 * Long loops keep several accumulators in SSE and x87 registers while the
 * timer preempts us. Every value is a multiple of 1/4, so double and long
 * double arithmetic is exact here and the result is known in advance; if
 * the kernel let another process's registers leak in, it would differ.
 * Run a few at once:  fputest 1 & fputest 2 & fputest 3
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define ITERATIONS 20000000L

int main(int argc, char **argv) {
    long seed = argc > 1 ? atoi(argv[1]) : getpid();
    double a = 0, b = 0, c = 0;
    long double x87 = 0;
    for (long i = 0; i < ITERATIONS; i++) {
        double step = (double)(i % 7) * 0.25;
        a += step + (double)seed;
        b += step * 2;
        c -= step;
        x87 += (long double)(i % 5) * 0.25L;
    }

    /* The same sums in integer arithmetic (in quarters). */
    long full = ITERATIONS / 7, rest = ITERATIONS % 7;
    long q7 = full * 21 + rest * (rest - 1) / 2;   /* sum of i % 7 */
    long full5 = ITERATIONS / 5, rest5 = ITERATIONS % 5;
    long q5 = full5 * 10 + rest5 * (rest5 - 1) / 2;
    double want_a = (double)q7 / 4 + (double)seed * ITERATIONS;
    double want_b = (double)q7 / 2;
    double want_c = -(double)q7 / 4;
    long double want_x87 = (long double)q5 / 4;

    bool ok = a == want_a && b == want_b && c == want_c && x87 == want_x87;
    printf("fputest %ld (pid %ld): %s  a=%.2f b=%.2f c=%.2f x87=%.2f\n", seed, getpid(),
           ok ? "OK" : "CORRUPTED", a, b, c, (double)x87);
    return ok ? 0 : 1;
}
