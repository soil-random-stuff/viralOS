/*
 * mbedTLS for viralOS: the library's default configuration (TLS 1.2 and 1.3, X.509, the usual
 * ciphers) with these changes, applied after it (MBEDTLS_USER_CONFIG_FILE):
 *   - no files, sockets or timers of its own: tls.c gives it the CA bundle as memory and does the
 *     socket I/O through callbacks
 *   - randomness from the kernel (getrandom: RDSEED/RDRAND + jitter, ChaCha20)
 *   - gmtime_r and the millisecond clock from tls.c (the C library has localtime_r only; the
 *     clock holds local time)
 */

/* Not here: files, BSD sockets, timers, PSA key files, self-tests, VIA PadLock. */
#undef MBEDTLS_FS_IO
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_PADLOCK_C
#undef MBEDTLS_DEBUG_C

/* Datagram TLS: nothing uses it, and it wants a timer. */
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_COOKIE_C

/* Entropy: mbedtls_hardware_poll() in tls.c. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* Dates on certificates; a millisecond clock (session ticket ages). */
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* Wiping keys from memory: tls.c (mbedTLS would pick glibc's explicit_bzero, seeing glibc's
   <limits.h> through the compiler's). */
#define MBEDTLS_PLATFORM_ZEROIZE_ALT

/* Writing certificates and requests: a client doesn't. */
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_X509_CSR_WRITE_C

/* 128-bit division would need libgcc. */
#define MBEDTLS_NO_UDBL_DIVISION
