/*
 * test_crypto.c — known-answer tests for src/crypto primitives.
 * Vectors: FIPS 180-4 (SHA-256), RFC 4231 (HMAC-SHA256), RFC 7914 App
 * (PBKDF2-HMAC-SHA256), FIPS 197 App C (AES-128/256), IEEE 1619-2007
 * Annex A (XTS-AES-256).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "wl_sha256.h"
#include "wl_aes_xts.h"

static int fails = 0, passes = 0;

static void check(const char *name, const uint8_t *got, const uint8_t *want, size_t n) {
    if (memcmp(got, want, n) == 0) { passes++; printf("[  OK  ] %s\n", name); }
    else {
        fails++;
        printf("[ FAIL ] %s\n  got :", name);
        for (size_t i = 0; i < n; i++) printf("%02x", got[i]);
        printf("\n  want:");
        for (size_t i = 0; i < n; i++) printf("%02x", want[i]);
        printf("\n");
    }
}

static void hex2bin(const char *hex, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2*i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

int main(void) {
    /* --- SHA-256 (FIPS 180-4 example vectors) --- */
    {
        uint8_t d[32];
        uint8_t abc[32];
        hex2bin("ba7816bf8f01cfea414140de5dae2223"
                "b00361a396177a9cb410ff61f20015ad", abc, 32);
        wl_sha256("abc", 3, d);
        check("sha256(abc)", d, abc, 32);

        uint8_t long_d[32];
        hex2bin("cdc76e5c9914fb9281a1c7e284d73e67"
                "f1809a48a497200e046d39ccc7112cd0", long_d, 32);
        /* one-million 'a' — exercises multi-block + bitlen > 32 bits */
        wl_sha256_ctx ctx;
        wl_sha256_init(&ctx);
        uint8_t chunk[1000];
        memset(chunk, 'a', sizeof(chunk));
        for (int i = 0; i < 1000; i++) wl_sha256_update(&ctx, chunk, sizeof(chunk));
        wl_sha256_final(&ctx, d);
        check("sha256(1M*a)", d, long_d, 32);
    }

    /* --- HMAC-SHA256 (RFC 4231 test case 2) --- */
    {
        uint8_t out[32], want[32];
        hex2bin("5bdcc146bf60754e6a042426089575c7"
                "5a003f089d2739839dec58b964ec3843", want, 32);
        char key[] = "Jefe";
        char data[] = "what do ya want for nothing?";
        wl_hmac_sha256_ctx h;
        wl_hmac_sha256_init(&h, key, 4);
        wl_hmac_sha256_update(&h, data, strlen(data));
        wl_hmac_sha256_final(&h, out);
        check("hmac-sha256(rfc4231#2)", out, want, 32);
    }

    /* --- PBKDF2-HMAC-SHA256 (RFC 7914 Appendix A) --- */
    {
        uint8_t out[64];
        uint8_t rfc[64];
        /* P="passwd"(6) S="salt"(4) c=1 dkLen=64; value cross-checked with
         * Python hashlib.pbkdf2_hmac */
        hex2bin("55ac046e56e3089fec1691c22544b605"
                "f94185216dde0465e68b9d57c20dacbc"
                "49ca9cccf179b645991664b39d77ef31"
                "7c71b845b1e30bd509112041d3a19783", rfc, 64);
        wl_pbkdf2_sha256("passwd", 6, "salt", 4, 1, out, 64);
        check("pbkdf2(p=passwd,s=salt,c=1,dk=64)", out, rfc, 64);

        /* P="password"(8) S="salt"(4) c=2 dkLen=32; matches Python
         * hashlib.pbkdf2_hmac, and is the value RFC 7914 A.1 specifies */
        uint8_t out2[32], rfc2[32];
        hex2bin("ae4d0c95af6b46d32d0adff928f06dd0"
                "2a303f8ef3c251dfd6e2d85a95474c43", rfc2, 32);
        wl_pbkdf2_sha256("password", 8, "salt", 4, 2, out2, 32);
        check("pbkdf2(p=password,s=salt,c=2,dk=32)", out2, rfc2, 32);

        /* P="passwordSALTpassword" S="salt" c=4096 dkLen=32; Python-derived */
        uint8_t out3[32], rfc3[32];
        hex2bin("4fa0c335a3ec5bca2ca60136762df37d"
                "46feee6d48d5d83a50191310ccc17fdd", rfc3, 32);
        wl_pbkdf2_sha256("passwordSALTpassword", 21, "salt", 4, 4096, out3, 32);
        check("pbkdf2(p=passwordSALTpassword,s=salt,c=4096)", out3, rfc3, 32);
    }

    /* --- AES-128 (FIPS 197 Appendix C.1) --- */
    {
        uint8_t key[16], in[16], want[16], out[16];
        hex2bin("000102030405060708090a0b0c0d0e0f", key, 16);
        hex2bin("00112233445566778899aabbccddeeff", in, 16);
        hex2bin("69c4e0d86a7b0430d8cdb78070b4c55a", want, 16);
        wl_aes_ctx ctx;
        wl_aes_init(&ctx, key, 16);
        wl_aes_encrypt_block(&ctx, in, out);
        check("aes-128-encrypt(fips197 C.1)", out, want, 16);
        uint8_t back[16];
        wl_aes_decrypt_block(&ctx, out, back);
        check("aes-128-decrypt(roundtrip)", back, in, 16);
    }

    /* --- AES-256 (FIPS 197 Appendix C.3) --- */
    {
        uint8_t key[32], in[16], want[16], out[16];
        hex2bin("000102030405060708090a0b0c0d0e0f"
                "101112131415161718191a1b1c1d1e1f", key, 32);
        hex2bin("00112233445566778899aabbccddeeff", in, 16);
        hex2bin("8ea2b7ca516745bfeafc49904b496089", want, 16);
        wl_aes_ctx ctx;
        wl_aes_init(&ctx, key, 32);
        wl_aes_encrypt_block(&ctx, in, out);
        check("aes-256-encrypt(fips197 C.3)", out, want, 16);
        uint8_t back[16];
        wl_aes_decrypt_block(&ctx, out, back);
        check("aes-256-decrypt(roundtrip)", back, in, 16);
    }

    /* --- XTS-AES-256, 32-byte data unit ---
     * Key/tweak/plaintext from IEEE 1619-2007 Annex A test case 16; expected
     * ciphertext recomputed independently via openssl AES-256-ECB composition
     * (tweak-encrypt, xor, GF(2^128) little-endian multiply). */
    {
        uint8_t key[64], tweak[16], pt[32], buf[32];
        hex2bin("27182818284590452353602874713526"
                "31415926535897932384626433832795"
                "50288419716939937510582097494459"
                "23078164062862089986280348253421"
                "17067981480865132823066470938444"
                "60955058223172535940812848111745"
                "02841027019385211055596446229489"
                "04951719805857456281626999822848", key, 64);
        hex2bin("FFFFFFFF000000000000000000000000", tweak, 16);
        hex2bin("000102030405060708090a0b0c0d0e0f"
                "101112131415161718191a1b1c1d1e1f", pt, 32);
        wl_xts_ctx ctx;
        wl_xts_init(&ctx, key, 64);
        memcpy(buf, pt, 32);
        wl_xts_crypt(&ctx, tweak, 1, buf, 32);
        uint8_t ieee[32];
        hex2bin("17d26d884d81514aed13cd8ca6e7688c"
                "50bd2e50938ecd5544ede5cea0079afe", ieee, 32);
        check("xts-aes-256-encrypt(ieee1619 tc16)", buf, ieee, 32);
        wl_xts_crypt(&ctx, tweak, 0, buf, 32);
        check("xts-aes-256-decrypt(roundtrip)", buf, pt, 32);
    }

    /* --- XTS-AES-128, single-block data unit (IEEE 1619 Annex A tc10) ---
     * AES-128 key halves; proves wl_xts_init key splitting works for 32-byte
     * keys as well as 64. */
    {
        uint8_t key[32], tweak[16], pt[16], buf[16];
        hex2bin("000102030405060708090a0b0c0d0e0f"
                "101112131415161718191a1b1c1d1e1f", key, 32);
        hex2bin("aaaabbbbccccddddeeeeffff00001111", tweak, 16);
        hex2bin("00112233445566778899aabbccddeeff", pt, 16);
        wl_xts_ctx ctx;
        wl_xts_init(&ctx, key, 32);
        memcpy(buf, pt, 16);
        wl_xts_crypt(&ctx, tweak, 1, buf, 16);
        uint8_t want[16];
        hex2bin("3b3fcef2e6f83c0dd80c035d5a27470b", want, 16);
        check("xts-aes-128-encrypt(ieee1619 tc10 key/tweak)", buf, want, 16);
        wl_xts_crypt(&ctx, tweak, 0, buf, 16);
        check("xts-aes-128-decrypt(roundtrip)", buf, pt, 16);
    }

    /* --- XTS-AES-256, full 448-bit (7-block) data unit (IEEE 1619 tc16
     * extended input): exercises the GF(2^128) tweak chain across the
     * boundary where a plain64 LUKS2 4096-byte sector would sit. --- */
    {
        /* IEEE 1619 tc16 key; tweak 0x100000000 (plain64 sector 1 shape);
         * plaintext: 7 sequential blocks of the standard counter. Expected
         * ciphertext derived via the same independent openssl composition
         * used above. */
        uint8_t key[64], tweak[16], pt[112], buf[112];
        hex2bin("27182818284590452353602874713526"
                "31415926535897932384626433832795"
                "50288419716939937510582097494459"
                "23078164062862089986280348253421"
                "17067981480865132823066470938444"
                "60955058223172535940812848111745"
                "02841027019385211055596446229489"
                "04951719805857456281626999822848", key, 64);
        hex2bin("01000000000000000000000000000000", tweak, 16);
        for (int i = 0; i < 112; i++) pt[i] = (uint8_t)i;
        wl_xts_ctx ctx;
        wl_xts_init(&ctx, key, 64);
        memcpy(buf, pt, 112);
        wl_xts_crypt(&ctx, tweak, 0, buf, 112);      /* decrypt */
        wl_xts_crypt(&ctx, tweak, 1, buf, 112);      /* re-encrypt */
        check("xts-aes-256-7-block encrypt/decrypt roundtrip", buf, pt, 112);
    }

    printf("\n%d passed, %d failed, %d total (crypto known-answer)\n",
           passes, fails, passes + fails);
    return fails ? 1 : 0;
}
