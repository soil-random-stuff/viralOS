#include <stdbool.h>

#include "arch/x86_64/fpu.h"
#include "arch/x86_64/cpu.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "mm/heap.h"

#define CR0_MP (1ULL << 1)    /* WAIT/FWAIT honour TS */
#define CR0_EM (1ULL << 2)    /* no FPU present: emulate (we don't) */
#define CR0_TS (1ULL << 3)    /* task switched: trap the next FPU use */
#define CR0_NE (1ULL << 5)    /* report x87 errors as #MF exceptions */
#define CR4_OSFXSR     (1ULL << 9)   /* OS saves state with FXSAVE: enables SSE */
#define CR4_OSXMMEXCPT (1ULL << 10)  /* OS handles SIMD exceptions (#XM) */
#define CR4_OSXSAVE    (1ULL << 18)  /* OS uses XSAVE: enables XGETBV/XSETBV and AVX */

/* CPUID leaf 1 */
#define CPUID_EDX_FXSR  (1u << 24)
#define CPUID_EDX_SSE   (1u << 25)
#define CPUID_EDX_SSE2  (1u << 26)
#define CPUID_ECX_XSAVE (1u << 26)
#define CPUID_ECX_AVX   (1u << 28)

/* XCR0: which register sets XSAVE manages and the CPU lets programs use. */
#define XCR0_X87       (1ULL << 0)
#define XCR0_SSE       (1ULL << 1)
#define XCR0_AVX       (1ULL << 2)   /* upper halves of YMM0-15 */
#define XCR0_OPMASK    (1ULL << 5)   /* AVX-512 k0-k7 */
#define XCR0_ZMM_HI256 (1ULL << 6)   /* upper halves of ZMM0-15 */
#define XCR0_HI16_ZMM  (1ULL << 7)   /* ZMM16-31 */
#define XCR0_AVX512    (XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM)

#define FXSAVE_SIZE 512

static bool use_xsave;
static uint64_t xcr0;
static size_t state_size = FXSAVE_SIZE;
static void *initial_state;          /* template copied into every new thread */
static char features[32];

static uint64_t read_cr0(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(v));
    return v;
}

static void write_cr0(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr0" : : "r"(v) : "memory");
}

static uint64_t read_cr4(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    return v;
}

static void write_cr4(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr4" : : "r"(v) : "memory");
}

static void xsetbv(uint32_t reg, uint64_t value) {
    __asm__ volatile ("xsetbv" : : "c"(reg), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

void fpu_save(void *state) {
    if (use_xsave) {
        __asm__ volatile ("xsave64 (%0)"
                          : : "r"(state), "a"((uint32_t)xcr0), "d"((uint32_t)(xcr0 >> 32))
                          : "memory");
    } else {
        __asm__ volatile ("fxsave64 (%0)" : : "r"(state) : "memory");
    }
}

void fpu_restore(const void *state) {
    if (use_xsave) {
        __asm__ volatile ("xrstor64 (%0)"
                          : : "r"(state), "a"((uint32_t)xcr0), "d"((uint32_t)(xcr0 >> 32))
                          : "memory");
    } else {
        __asm__ volatile ("fxrstor64 (%0)" : : "r"(state) : "memory");
    }
}

/* Turn on XSAVE and every register set we know how to handle. */
static void enable_xsave(uint32_t cpuid1_ecx) {
    write_cr4(read_cr4() | CR4_OSXSAVE);

    uint32_t a, b, c, d;
    cpuid(0xD, &a, &b, &c, &d);   /* sub-leaf 0: what XCR0 may contain */
    uint64_t supported = ((uint64_t)d << 32) | a;

    xcr0 = XCR0_X87 | XCR0_SSE;
    if ((cpuid1_ecx & CPUID_ECX_AVX) && (supported & XCR0_AVX)) {
        xcr0 |= XCR0_AVX;
        if ((supported & XCR0_AVX512) == XCR0_AVX512) {
            xcr0 |= XCR0_AVX512;
        }
    }
    xsetbv(0, xcr0);

    cpuid(0xD, &a, &b, &c, &d);   /* EBX: save area size for what's enabled now */
    state_size = b;
    use_xsave = true;
}

/* Another CPU: the same registers on, the same XCR0 (fpu_init found out what that is). */
void fpu_init_cpu(void) {
    write_cr0((read_cr0() & ~(CR0_EM | CR0_TS)) | CR0_MP | CR0_NE);
    write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);
    if (use_xsave) {
        write_cr4(read_cr4() | CR4_OSXSAVE);
        xsetbv(0, xcr0);
    }
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile ("fninit; ldmxcsr %0" : : "m"(mxcsr));
}

void fpu_init(void) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    uint32_t needed = CPUID_EDX_FXSR | CPUID_EDX_SSE | CPUID_EDX_SSE2;
    if ((d & needed) != needed) {
        panic("fpu: CPU lacks FXSAVE/SSE/SSE2, which every x86_64 CPU should have");
    }

    write_cr0((read_cr0() & ~(CR0_EM | CR0_TS)) | CR0_MP | CR0_NE);
    write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);
    if (c & CPUID_ECX_XSAVE) {
        enable_xsave(c);
    }

    strlcpy(features, "x87 SSE", sizeof(features));
    if (xcr0 & XCR0_AVX) {
        strlcpy(features + strlen(features), " AVX", sizeof(features) - strlen(features));
    }
    if (xcr0 & XCR0_AVX512) {
        strlcpy(features + strlen(features), " AVX-512", sizeof(features) - strlen(features));
    }

    /* The template: x87 reset, MXCSR at its power-on default (all SIMD
       exceptions masked, round to nearest), vector registers zero. The
       area must start zeroed: XRSTOR rejects garbage in its header. */
    initial_state = fpu_state_alloc();
    if (!initial_state) {
        panic("fpu: out of memory");
    }
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile ("fninit; ldmxcsr %0" : : "m"(mxcsr));
    fpu_save(initial_state);
}

/*
 * XSAVE needs 64-byte alignment; kmalloc gives 16. Over-allocate, align,
 * and keep the real pointer just in front of the area for freeing.
 */
void *fpu_state_alloc(void) {
    uint8_t *raw = kmalloc(state_size + 64 + sizeof(void *));
    if (!raw) {
        return NULL;
    }
    uint8_t *state = (uint8_t *)(((uintptr_t)raw + sizeof(void *) + 63) & ~(uintptr_t)63);
    ((void **)state)[-1] = raw;
    if (initial_state) {
        memcpy(state, initial_state, state_size);
    } else {
        memset(state, 0, state_size);
    }
    return state;
}

void fpu_state_free(void *state) {
    if (state) {
        kfree(((void **)state)[-1]);
    }
}

const char *fpu_features(void) {
    return features;
}

size_t fpu_state_size(void) {
    return state_size;
}
