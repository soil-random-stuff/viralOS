#pragma once

#include <stddef.h>
#include <stdint.h>

struct sha256 {
    uint32_t h[8];
    uint64_t length;
    uint8_t block[64];
    size_t used;
};

void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, size_t len);
void sha256_final(struct sha256 *c, uint8_t out[32]);
void hmac_sha256(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[32]);
void pbkdf2_sha256(const void *password, size_t password_len, const uint8_t *salt, size_t salt_len,
                   uint32_t iterations, uint8_t out[32]);
