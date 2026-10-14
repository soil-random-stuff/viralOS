#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * FPU, SSE, AVX (and AVX-512 if present) for user programs. The kernel
 * itself never uses these registers (it's built with -mno-sse), so they
 * only need saving when the scheduler switches between threads that own
 * an FPU state.
 *
 * With XSAVE (any CPU from the last ~15 years) the save area holds every
 * register set the CPU has; without it we fall back to the 512-byte
 * FXSAVE area (x87 + SSE only).
 */
void fpu_init(void);
/* The same on an application processor (after fpu_init ran on the BSP). */
void fpu_init_cpu(void);

/* A clean, 64-byte aligned save area for a new thread. NULL if out of memory. */
void *fpu_state_alloc(void);
void fpu_state_free(void *state);

void fpu_save(void *state);
void fpu_restore(const void *state);

/* For the boot log: e.g. "x87 SSE AVX", and the save area size in bytes. */
const char *fpu_features(void);
size_t fpu_state_size(void);
