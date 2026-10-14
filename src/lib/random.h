#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Random bytes good enough for keys (TLS, passwords' salts): a ChaCha20 generator seeded from the
 * CPU's RDSEED/RDRAND and the jitter of the time-stamp counter, with RDRAND mixed in again on every
 * request when the CPU has it. Thread context only (it takes a mutex).
 */
void random_bytes(void *buf, size_t len);
uint64_t random_u64(void);
