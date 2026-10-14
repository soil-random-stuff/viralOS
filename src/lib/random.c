/*
 * The kernel's random number generator.
 *
 * Seeding: 32 words from RDSEED (or RDRAND, if that's all the CPU has), XORed with the time-stamp
 * counter's jitter (the low bits of many tiny timed loops: the timing of caches, interrupts and
 * other CPUs). That becomes a ChaCha20 key.
 *
 * Output: ChaCha20 blocks under that key ("fast key erasure": the first 32 bytes of every request's
 * stream become the next key, so a later look at memory can't recover what was handed out before).
 * Each request also mixes in fresh RDRAND output and the TSC, so even a key that leaked heals.
 */

#include "lib/random.h"
#include "arch/x86_64/cpu.h"
#include "drivers/timer.h"
#include "lib/string.h"
#include "sched/sync.h"

#include <stdbool.h>

static struct mutex lock = MUTEX_INIT;
static uint32_t key[8];
static uint64_t counter;
static bool seeded, has_rdrand, has_rdseed;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static bool rdrand64(uint64_t *out) {
    for (int tries = 0; tries < 10; tries++) {
        unsigned char ok;
        __asm__ volatile ("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok));
        if (ok) {
            return true;
        }
    }
    return false;
}

static bool rdseed64(uint64_t *out) {
    for (int tries = 0; tries < 100; tries++) {
        unsigned char ok;
        __asm__ volatile ("rdseed %0; setc %1" : "=r"(*out), "=qm"(ok));
        if (ok) {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

/* --- ChaCha20 (RFC 8439) ---------------------------------------------------------------------- */

#define ROTL(x, n) ((x) << (n) | (x) >> (32 - (n)))
#define ROTL64(x, n) ((x) << (n) | (x) >> (64 - (n)))
#define QR(a, b, c, d) \
    a += b; d ^= a; d = ROTL(d, 16); \
    c += d; b ^= c; b = ROTL(b, 12); \
    a += b; d ^= a; d = ROTL(d, 8);  \
    c += d; b ^= c; b = ROTL(b, 7)

static void chacha_block(const uint32_t k[8], uint64_t n, uint32_t out[16]) {
    uint32_t s[16] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
        k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7],
        (uint32_t)n, (uint32_t)(n >> 32), 0, 0,
    };
    uint32_t x[16];
    memcpy(x, s, sizeof(x));
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        out[i] = x[i] + s[i];
    }
}

/* --- seeding ------------------------------------------------------------------------------------- */

/* One word of timing jitter: many short timed loops, their low bits folded together. */
static uint64_t jitter64(void) {
    uint64_t acc = 0;
    for (int i = 0; i < 64; i++) {
        uint64_t t0 = rdtsc();
        volatile uint32_t spin = 0;
        for (int j = 0; j < 16 + (int)(t0 & 15); j++) {
            spin += (uint32_t)j;
        }
        uint64_t d = rdtsc() - t0;
        acc = ROTL64(acc, 7) ^ d;
    }
    return acc;
}

static void seed(void) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    has_rdrand = c >> 30 & 1;
    cpuid(0, &a, &b, &c, &d);
    if (a >= 7) {
        cpuid(7, &a, &b, &c, &d);
        has_rdseed = b >> 18 & 1;
    }
    uint64_t pool[4] = { rdtsc(), uptime_ms(), 0, 0 };
    for (int i = 0; i < 32; i++) {
        uint64_t w = 0;
        if (!(has_rdseed && rdseed64(&w)) && has_rdrand) {
            rdrand64(&w);
        }
        pool[i & 3] ^= w ^ jitter64();
        /* (mix the pool through ChaCha so every word affects every other) */
        if ((i & 3) == 3) {
            uint32_t k[8], out[16];
            memcpy(k, pool, sizeof(k));
            chacha_block(k, (uint64_t)i, out);
            memcpy(pool, out, sizeof(pool));
        }
    }
    memcpy(key, pool, sizeof(key));
    memset(pool, 0, sizeof(pool));
    seeded = true;
}

void random_bytes(void *buf, size_t len) {
    uint8_t *p = buf;
    uint32_t out[16];
    mutex_lock(&lock);
    if (!seeded) {
        seed();
    }
    /* Fresh input on every request. */
    uint64_t fresh = rdtsc();
    if (has_rdrand) {
        uint64_t r;
        if (rdrand64(&r)) {
            fresh ^= r;
        }
    }
    key[0] ^= (uint32_t)fresh;
    key[1] ^= (uint32_t)(fresh >> 32);
    /* The next key first, then the output. */
    chacha_block(key, counter++, out);
    memcpy(key, out, sizeof(key));
    size_t done = 0;
    while (done < len) {
        chacha_block(key, counter++, out);
        size_t n = len - done < sizeof(out) ? len - done : sizeof(out);
        memcpy(p + done, out, n);
        done += n;
    }
    /* Fast key erasure: the stream that made this output won't be made again. */
    chacha_block(key, counter++, out);
    memcpy(key, out, sizeof(key));
    memset(out, 0, sizeof(out));
    mutex_unlock(&lock);
}

uint64_t random_u64(void) {
    uint64_t v;
    random_bytes(&v, sizeof(v));
    return v;
}
