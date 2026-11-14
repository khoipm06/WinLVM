/*
 * test_sha256.c — Self-contained FIPS 180-4 SHA-256 implementation for tests.
 */
#include "test_sha256.h"

#include <string.h>

static const uint32_t k_sha256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr32(uint32_t x, unsigned int n) {
    return (x >> n) | (x << (32u - n));
}

static void sha256_transform(test_sha256_ctx *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for (unsigned int i = 0; i < 16u; i++) {
        size_t j = (size_t)i * 4u;
        w[i] = ((uint32_t)block[j] << 24) |
               ((uint32_t)block[j + 1u] << 16) |
               ((uint32_t)block[j + 2u] << 8) |
               ((uint32_t)block[j + 3u]);
    }
    for (unsigned int i = 16u; i < 64u; i++) {
        uint32_t s0 = rotr32(w[i - 15u], 7u) ^ rotr32(w[i - 15u], 18u) ^ (w[i - 15u] >> 3);
        uint32_t s1 = rotr32(w[i - 2u], 17u) ^ rotr32(w[i - 2u], 19u) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (unsigned int i = 0; i < 64u; i++) {
        uint32_t s1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + k_sha256[i] + w[i];
        uint32_t s0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void test_sha256_init(test_sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

void test_sha256_update(test_sha256_ctx *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->bitlen += (uint64_t)len * 8u;

    if (ctx->buflen > 0) {
        size_t need = 64u - ctx->buflen;
        if (len < need) {
            memcpy(ctx->buf + ctx->buflen, p, len);
            ctx->buflen += len;
            return;
        }
        memcpy(ctx->buf + ctx->buflen, p, need);
        sha256_transform(ctx, ctx->buf);
        p += need;
        len -= need;
        ctx->buflen = 0;
    }

    while (len >= 64u) {
        sha256_transform(ctx, p);
        p += 64u;
        len -= 64u;
    }

    if (len > 0) {
        memcpy(ctx->buf, p, len);
        ctx->buflen = len;
    }
}

void test_sha256_final(test_sha256_ctx *ctx, uint8_t digest[32]) {
    size_t i = ctx->buflen;
    ctx->buf[i++] = 0x80u;
    if (i > 56u) {
        memset(ctx->buf + i, 0, 64u - i);
        sha256_transform(ctx, ctx->buf);
        i = 0;
    }
    memset(ctx->buf + i, 0, 56u - i);

    uint64_t bits = ctx->bitlen;
    for (unsigned int k = 0; k < 8u; k++) {
        ctx->buf[63u - k] = (uint8_t)(bits & 0xffu);
        bits >>= 8;
    }
    sha256_transform(ctx, ctx->buf);

    for (unsigned int j = 0; j < 8u; j++) {
        digest[j * 4u + 0u] = (uint8_t)((ctx->state[j] >> 24) & 0xffu);
        digest[j * 4u + 1u] = (uint8_t)((ctx->state[j] >> 16) & 0xffu);
        digest[j * 4u + 2u] = (uint8_t)((ctx->state[j] >> 8) & 0xffu);
        digest[j * 4u + 3u] = (uint8_t)(ctx->state[j] & 0xffu);
    }
}

void test_sha256_hex(const uint8_t digest[32], char hex_out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < 32u; i++) {
        hex_out[i * 2u] = hex[(digest[i] >> 4) & 0x0fu];
        hex_out[i * 2u + 1u] = hex[digest[i] & 0x0fu];
    }
    hex_out[64] = '\0';
}

void test_sha256_final_hex(test_sha256_ctx *ctx, char hex_out[65]) {
    uint8_t digest[32];
    test_sha256_final(ctx, digest);
    test_sha256_hex(digest, hex_out);
}
