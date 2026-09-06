/*
 * wl_aes_xts.h — freestanding AES-128/256 (FIPS 197) and XTS-AES
 * (IEEE 1619-2007 / NIST SP 800-38E).
 */
#ifndef WL_AES_XTS_H
#define WL_AES_XTS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t round_keys[60];   /* max: 15 rounds * 4 words */
    int      rounds;           /* 10 (AES-128), 14 (AES-256) */
} wl_aes_ctx;

/* key_len is 16 (AES-128) or 32 (AES-256) bytes. */
void wl_aes_init(wl_aes_ctx *ctx, const uint8_t *key, size_t key_len);
void wl_aes_encrypt_block(const wl_aes_ctx *ctx, const uint8_t in[16], uint8_t out[16]);
void wl_aes_decrypt_block(const wl_aes_ctx *ctx, const uint8_t in[16], uint8_t out[16]);

/* XTS-AES. data_unit_bits is typically 128 bits (16-byte tweak), used to
 * carry a LUKS2 plain64 sector number. key is the full XTS key:
 * 32 bytes for AES-128-XTS, 64 bytes for AES-256-XTS. */
typedef struct {
    wl_aes_ctx crypt;      /* first half of key */
    wl_aes_ctx tweak;     /* second half of key */
} wl_xts_ctx;

void wl_xts_init(wl_xts_ctx *ctx, const uint8_t *key, size_t key_len);
void wl_xts_crypt(wl_xts_ctx *ctx, const uint8_t tweak[16],
                  int encrypt, uint8_t *buf, size_t len);

#endif
