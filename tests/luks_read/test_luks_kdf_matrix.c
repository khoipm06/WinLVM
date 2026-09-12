/*
 * test_luks_kdf_matrix.c — Stage 4 support-matrix test.
 *
 * The audit marked Stage 4 PARTIAL: only the pbkdf2 keyslot KDF is
 * implemented; argon2id (cryptsetup's default) is NOT, by design —
 * argon2 at 1 GiB is infeasible pre-boot (roadmap decision D4).
 *
 * This test documents the current matrix against both corpus images
 * and proves fail-closed behavior on tampered headers:
 *
 *   luks2-pbkdf2.img  keyslot kdf=pbkdf2     -> unlock + digest OK
 *   luks2-default.img keyslot kdf=argon2id    -> unlock refused (NOT_SUPPORTED)
 *   corrupted csum    header tamper           -> read_metadata refused
 *   truncated hdr     version/size garbage    -> read_metadata refused
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "luks_read.h"
#include "luks_unlock.h"

#ifdef _WIN32
#define WL_FSEEK64 _fseeki64
#define WL_FTELL64 _ftelli64
#else
#define WL_FSEEK64 fseeko
#define WL_FTELL64 ftello
#endif

static int fails = 0, passes = 0;
#define CHECK(name, cond) do { \
    if (cond) { passes++; printf("[  OK  ] %s\n", name); } \
    else { fails++; printf("[ FAIL ] %s\n", name); } } while (0)

/* in-memory byte source over a loaded image, so tampering is easy */
typedef struct {
    uint8_t *data;
    uint64_t size;
} mem_src;

static size_t mem_read(void *impl, uint64_t offset, void *buf, size_t len) {
    mem_src *m = (mem_src *)impl;
    if (offset > m->size || len > m->size - offset) return 0;
    memcpy(buf, m->data + offset, len);
    return len;
}

static winlvm_byte_source mem_source(mem_src *m) {
    winlvm_byte_source s = { m, m->size, mem_read };
    return s;
}

static mem_src *load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    WL_FSEEK64(f, 0, SEEK_END);
    long sz = WL_FTELL64(f);
    WL_FSEEK64(f, 0, SEEK_SET);
    mem_src *m = (mem_src *)malloc(sizeof(*m));
    m->data = (uint8_t *)malloc((size_t)sz);
    if (fread(m->data, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return NULL; }
    m->size = (uint64_t)sz;
    fclose(f);
    return m;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <luks2-default.img> <luks2-pbkdf2.img>\n", argv[0]);
        return 1;
    }
    static unsigned char jscratch[WL_LUKS_JSON_SCRATCH_MAX];

    mem_src *argon = load_image(argv[1]);
    mem_src *pbkdf = load_image(argv[2]);
    if (!argon || !pbkdf) return 1;

    /* ---- matrix row 1: argon2id = NOT SUPPORTED (documented gap) ---- */
    {
        winlvm_byte_source s = mem_source(argon);
        winlvm_luks_metadata md;
        winlvm_luks_err err = {0};
        CHECK("argon2id: metadata parses (kdf fields read)",
              winlvm_luks_read_metadata(&s, jscratch, &md, &err));
        CHECK("argon2id: keyslot kdf recognized as argon2id",
              md.keyslot_count == 1 && strcmp(md.keyslots[0].kdf.type, "argon2id") == 0);
        uint8_t vk[64];
        static uint8_t scratch[WL_LUKS_UNLOCK_SCRATCH_MAX];
        winlvm_luks_unlock_err uerr = {0};
        int klen = winlvm_luks_unlock_keyslot(&s, &md, 0,
                                               "winlvm-test-passphrase", 23,
                                               vk, sizeof(vk),
                                               scratch, sizeof(scratch), &uerr);
        CHECK("argon2id: unlock REFUSED (not implemented, fail closed)", klen <= 0);
        CHECK("argon2id: refusal names the KDF",
              uerr.failed && strstr(uerr.message, "KDF") != NULL);
    }

    /* ---- matrix row 2: pbkdf2 = SUPPORTED ---- */
    {
        winlvm_byte_source s = mem_source(pbkdf);
        winlvm_luks_metadata md;
        winlvm_luks_err err = {0};
        CHECK("pbkdf2: metadata parses", winlvm_luks_read_metadata(&s, jscratch, &md, &err));
        CHECK("pbkdf2: keyslot kdf recognized as pbkdf2/sha256",
              md.keyslot_count == 1 &&
              strcmp(md.keyslots[0].kdf.type, "pbkdf2") == 0 &&
              strcmp(md.keyslots[0].kdf.hash, "sha256") == 0);
        CHECK("pbkdf2: area cipher parsed from JSON",
              strcmp(md.keyslots[0].area_encryption, "aes-xts-plain64") == 0 &&
              md.keyslots[0].area_key_size == 64);
        uint8_t vk[64];
        static uint8_t scratch[WL_LUKS_UNLOCK_SCRATCH_MAX];
        winlvm_luks_unlock_err uerr = {0};
        int klen = winlvm_luks_unlock_keyslot(&s, &md, 0,
                                               "winlvm-test-passphrase", 23,
                                               vk, sizeof(vk),
                                               scratch, sizeof(scratch), &uerr);
        CHECK("pbkdf2: unlock succeeds", klen == 64);
        CHECK("pbkdf2: volume key verifies against digest",
              winlvm_luks_verify_key(&md, vk, 64));
    }

    /* ---- checksum integrity: flip one byte in the JSON area -> reject ---- */
    {
        mem_src *tam = load_image(argv[2]);
        /* JSON area begins right after the 4096-byte binary header; flip a
         * byte inside the metadata text (offset 5000 is inside "keyslots") */
        tam->data[5000] ^= 0xff;
        winlvm_byte_source s = mem_source(tam);
        winlvm_luks_metadata md;
        winlvm_luks_err err = {0};
        CHECK("tampered JSON: metadata read refused (csum mismatch)",
              !winlvm_luks_read_metadata(&s, jscratch, &md, &err));
        CHECK("tampered JSON: refusal names checksum",
              err.failed && strstr(err.message, "checksum") != NULL);
        free(tam->data); free(tam);
    }

    /* ---- bad magic / bad version: reject ---- */
    {
        mem_src *tam = load_image(argv[2]);
        tam->data[0] = 'X';  /* break magic */
        winlvm_byte_source s = mem_source(tam);
        winlvm_luks_metadata md;
        winlvm_luks_err err = {0};
        CHECK("bad magic: refused", !winlvm_luks_read_metadata(&s, jscratch, &md, &err));
        free(tam->data); free(tam);

        tam = load_image(argv[2]);
        tam->data[6] = 0x03; tam->data[7] = 0x00;  /* version = 3 */
        s = mem_source(tam);
        CHECK("bad version: refused", !winlvm_luks_read_metadata(&s, jscratch, &md, &err));
        free(tam->data); free(tam);
    }

    free(argon->data); free(argon);
    free(pbkdf->data); free(pbkdf);
    printf("\n%d passed, %d failed (kdf support matrix)\n", passes, fails);
    return fails ? 1 : 0;
}
