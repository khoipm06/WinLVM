/*
 * test_sha256.h — Self-contained FIPS 180-4 SHA-256 helper for host tests.
 */
#ifndef WINLVM_TEST_SHA256_H
#define WINLVM_TEST_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct test_sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   buflen;
} test_sha256_ctx;

void test_sha256_init(test_sha256_ctx *ctx);
void test_sha256_update(test_sha256_ctx *ctx, const void *data, size_t len);
void test_sha256_final(test_sha256_ctx *ctx, uint8_t digest[32]);
void test_sha256_hex(const uint8_t digest[32], char hex_out[65]);
void test_sha256_final_hex(test_sha256_ctx *ctx, char hex_out[65]);

#ifdef __cplusplus
}
#endif

#endif /* WINLVM_TEST_SHA256_H */
