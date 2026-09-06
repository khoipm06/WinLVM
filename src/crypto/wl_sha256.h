/*
 * wl_sha256.h — freestanding SHA-256 (FIPS 180-4).
 *
 * Kernel/UEFI/user tri-target: no CRT, no allocation, no global state.
 */
#ifndef WL_SHA256_H
#define WL_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define WL_SHA256_DIGEST_LENGTH 32

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  block[64];
    size_t   block_len;
} wl_sha256_ctx;

void wl_sha256_init(wl_sha256_ctx *ctx);
void wl_sha256_update(wl_sha256_ctx *ctx, const void *data, size_t len);
void wl_sha256_final(wl_sha256_ctx *ctx, uint8_t out[WL_SHA256_DIGEST_LENGTH]);
void wl_sha256(const void *data, size_t len, uint8_t out[WL_SHA256_DIGEST_LENGTH]);

/* HMAC (FIPS 198-1), SHA-256 as inner hash. */
typedef struct {
    wl_sha256_ctx inner;
    wl_sha256_ctx outer;
} wl_hmac_sha256_ctx;

void wl_hmac_sha256_init(wl_hmac_sha256_ctx *ctx, const void *key, size_t key_len);
void wl_hmac_sha256_update(wl_hmac_sha256_ctx *ctx, const void *data, size_t len);
void wl_hmac_sha256_final(wl_hmac_sha256_ctx *ctx, uint8_t out[WL_SHA256_DIGEST_LENGTH]);

/* PBKDF2-HMAC-SHA-256 (RFC 2898 / RFC 8018 §5.2). dkLen <= 32*UINT32_MAX. */
void wl_pbkdf2_sha256(const void *password, size_t password_len,
                      const void *salt, size_t salt_len,
                      uint32_t iterations,
                      uint8_t *out, size_t out_len);

#endif
