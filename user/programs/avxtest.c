/*
 * avxtest [seed]: AVX in user programs.
 *
 * Without arguments: show what the CPU supports and what the kernel enabled,
 * then run some 256-bit vector math and an isolation check.
 * With a seed: only the isolation check, for running several at once:
 *     avxtest 1 & avxtest 2 & avxtest 3
 *
 * The isolation check keeps 4-wide double vectors in YMM registers through
 * a long loop while the timer preempts us. Lanes 2 and 3 live in the upper
 * halves of the YMM registers, which only XSAVE (not the older FXSAVE)
 * preserves across task switches. Every value is a multiple of 1/4, so the
 * arithmetic is exact and the expected result is known in advance.
 *
 * Only the functions marked target("avx") use AVX instructions, so the
 * program still starts (and reports) on a CPU without it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define ITERATIONS 20000000L

typedef double v4d __attribute__((vector_size(32)));   /* one YMM register */
typedef float v8f __attribute__((vector_size(32)));

struct features {
    bool sse2, avx, avx2, fma, avx512f;   /* the CPU has it */
    bool os_xsave, os_avx, os_avx512;     /* the kernel turned it on (XCR0) */
};

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile ("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static struct features detect(void) {
    struct features f = { 0 };
    uint32_t r[4];
    cpuid(0, 0, r);
    uint32_t max_leaf = r[0];

    cpuid(1, 0, r);
    f.sse2 = r[3] & (1u << 26);
    f.fma = r[2] & (1u << 12);
    f.os_xsave = r[2] & (1u << 27);   /* OSXSAVE: the kernel set CR4.OSXSAVE */
    f.avx = r[2] & (1u << 28);
    if (max_leaf >= 7) {
        cpuid(7, 0, r);
        f.avx2 = r[1] & (1u << 5);
        f.avx512f = r[1] & (1u << 16);
    }
    if (f.os_xsave) {
        uint32_t lo, hi;
        __asm__ volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        uint64_t xcr0 = ((uint64_t)hi << 32) | lo;
        f.os_avx = (xcr0 & 0x6) == 0x6;        /* SSE + AVX state */
        f.os_avx512 = (xcr0 & 0xE6) == 0xE6;   /* + opmask, ZMM_Hi256, Hi16_ZMM */
    }
    return f;
}

static const char *yes_no(bool b) {
    return b ? "\x1b[92myes\x1b[0m" : "\x1b[91mno\x1b[0m";
}

/* 8 floats per instruction. */
__attribute__((target("avx"), noinline))
static float dot_avx(const float *a, const float *b, int n) {
    v8f sum = { 0 };
    for (int i = 0; i < n; i += 8) {
        v8f va, vb;
        __builtin_memcpy(&va, a + i, sizeof(va));
        __builtin_memcpy(&vb, b + i, sizeof(vb));
        sum += va * vb;
    }
    float total = 0;
    for (int i = 0; i < 8; i++) {
        total += sum[i];
    }
    return total;
}

static float dot_scalar(const float *a, const float *b, int n) {
    float total = 0;
    for (int i = 0; i < n; i++) {
        total += a[i] * b[i];
    }
    return total;
}

__attribute__((target("avx"), noinline))
static bool ymm_isolation(long seed, double *lane3) {
    const v4d k = { 0.25, 0.5, 0.75, 1.0 };
    const v4d s = { (double)seed, (double)seed, (double)seed, (double)seed };
    v4d a = { 0 }, b = { 0 }, c = { 0 };
    for (long i = 0; i < ITERATIONS; i++) {
        double m = (double)(i % 7);
        v4d mv = { m, m, m, m };
        a += mv * k + s;
        b += mv * k;
        c -= mv;
    }

    long full = ITERATIONS / 7, rest = ITERATIONS % 7;
    double q7 = (double)(full * 21 + rest * (rest - 1) / 2);   /* sum of i % 7 */
    bool ok = true;
    for (int lane = 0; lane < 4; lane++) {
        ok &= a[lane] == q7 * k[lane] + (double)seed * ITERATIONS;
        ok &= b[lane] == q7 * k[lane];
        ok &= c[lane] == -q7;
    }
    *lane3 = a[3];
    return ok;
}

static int isolation(long seed) {
    double lane3;
    bool ok = ymm_isolation(seed, &lane3);
    printf("avxtest %ld (pid %ld): %s  (upper YMM lane: %.2f)\n", seed, getpid(),
           ok ? "OK" : "CORRUPTED", lane3);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    struct features f = detect();
    bool usable = f.avx && f.os_avx;

    if (argc > 1) {
        if (!usable) {
            printf("avxtest: AVX is not available here\n");
            return 1;
        }
        return isolation(atoi(argv[1]));
    }

    printf("CPU supports:   SSE2 %s  AVX %s  AVX2 %s  FMA %s  AVX-512F %s\n",
           yes_no(f.sse2), yes_no(f.avx), yes_no(f.avx2), yes_no(f.fma), yes_no(f.avx512f));
    printf("Kernel enabled: XSAVE %s  AVX state %s  AVX-512 state %s\n",
           yes_no(f.os_xsave), yes_no(f.os_avx), yes_no(f.os_avx512));
    if (!usable) {
        printf("AVX can't be used (in QEMU, start it with -cpu max).\n");
        return 1;
    }

    static float a[4096], b[4096];
    for (int i = 0; i < 4096; i++) {
        a[i] = (float)(i % 13);
        b[i] = (float)(i % 5) * 0.5f;
    }
    float avx = dot_avx(a, b, 4096), scalar = dot_scalar(a, b, 4096);
    printf("Dot product of 4096 floats: AVX %.1f, scalar %.1f -> %s\n", (double)avx,
           (double)scalar, avx == scalar ? "match" : "MISMATCH");

    return isolation(getpid());
}
