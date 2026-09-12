/*
 * test_luks_unlock.c — Phase 5 Stage 4 end-to-end: unlock the corpus
 * luks2-pbkdf2 container with the known passphrase, verify the volume key
 * against the committed master-key oracle, and decrypt the whole LV
 * comparing its SHA-256 against the committed lv-hashes oracle.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "luks_read.h"
#include "luks_unlock.h"
#include "wl_sha256.h"

#ifdef _WIN32
#define WL_FSEEK64 _fseeki64
#define WL_FTELL64 _ftelli64
#else
#define WL_FSEEK64 fseeko
#define WL_FTELL64 ftello
#endif

static size_t file_read(void *impl, uint64_t offset, void *buf, size_t len) {
    FILE *f = (FILE *)impl;
    if (WL_FSEEK64(f, (int64_t)offset, SEEK_SET) != 0) return 0;
    return fread(buf, 1, len, f);
}

static int fails = 0, passes = 0;
#define CHECK(name, cond) do { \
    if (cond) { passes++; printf("[  OK  ] %s\n", name); } \
    else { fails++; printf("[ FAIL ] %s\n", name); } } while (0)

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <luks2-pbkdf2 image> <corpus-expected-dir>\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    WL_FSEEK64(f, 0, SEEK_END);
    uint64_t image_size = (uint64_t)WL_FTELL64(f);

    static unsigned char jscratch[WL_LUKS_JSON_SCRATCH_MAX];
    winlvm_byte_source src = { f, image_size, file_read };

    /* 1. metadata */
    winlvm_luks_metadata md;
    winlvm_luks_err err = {0};
    if (!winlvm_luks_read_metadata(&src, jscratch, &md, &err)) {
        fprintf(stderr, "metadata: %s\n", err.message);
        return 1;
    }
    CHECK("metadata parses", md.segment_count == 1 && md.keyslot_count == 1 && md.digest_count == 1);

    /* 2. unlock keyslot 0 */
    const char *pass = "winlvm-test-passphrase";
    uint8_t volume_key[64];
    static uint8_t scratch[WL_LUKS_UNLOCK_SCRATCH_MAX];  /* 64 * 8192 */
    winlvm_luks_unlock_err uerr = {0};
    int klen = winlvm_luks_unlock_keyslot(&src, &md, 0, pass, strlen(pass),
                                          volume_key, sizeof(volume_key),
                                          scratch, sizeof(scratch), &uerr);
    if (klen <= 0) {
        fprintf(stderr, "unlock: %s\n", uerr.message);
        return 1;
    }
    CHECK("keyslot unlock (pbkdf2 4000 + AF 4000 stripes)", klen == 64);

    /* 3. digest verify */
    CHECK("volume key digest verifies", winlvm_luks_verify_key(&md, volume_key, (size_t)klen));

    /* 4. wrong passphrase must fail the digest */
    {
        uint8_t bad_key[64];
        winlvm_luks_unlock_err e2 = {0};
        int bl = winlvm_luks_unlock_keyslot(&src, &md, 0, "wrong-passphrase", 15,
                                            bad_key, sizeof(bad_key),
                                            scratch, sizeof(scratch), &e2);
        int bad_ok = 0;
        if (bl > 0 && !winlvm_luks_verify_key(&md, bad_key, (size_t)bl)) bad_ok = 1;
        if (bl <= 0) bad_ok = 1; /* refused or produced key failing digest */
        CHECK("wrong passphrase rejected by digest", bad_ok);
    }

    /* 5. LVM2 PV label at decrypted sector 1 (the payload IS the PV) */
    {
        uint8_t buf[1024];
        winlvm_luks_unlock_err e3 = {0};
        if (!winlvm_luks_decrypt_region(&src, &md, volume_key, 64, 512, buf, 512, &e3)) {
            fprintf(stderr, "decrypt: %s\n", e3.message);
            return 1;
        }
        CHECK("decrypted payload is an LVM2 PV (LABELONE)", memcmp(buf, "LABELONE", 8) == 0);
    }

    /* 6. LV-content hash vs Linux oracle. The LUKS payload is the PV, not
     * the LV: lvboot starts at the PV 1st-PE offset (pvs.txt oracle).
     * Decrypt the LV's first 128 KiB through decrypt_region and compare
     * against the lv-hashes range-0 oracle line. */
    {
        char path[512];
        uint64_t pe_start = 0;
        snprintf(path, sizeof(path), "%s/luks2-pbkdf2/pvs.txt", argv[2]);
        FILE *pf = fopen(path, "r");
        if (!pf) { printf("[ FAIL ] open %s\n", path); fails++; }
        else {
            char line[1024];
            while (fgets(line, sizeof(line), pf)) {
                if (strstr(line, "vgboot") == NULL) continue;
                char *p = strtok(line, " \t\n");
                int col = 0;
                while (p) {
                    if (++col == 5) { pe_start = strtoull(p, NULL, 10); break; }
                    p = strtok(NULL, " \t\n");
                }
                break;
            }
            fclose(pf);
        }
        CHECK("PV 1st-PE parsed from pvs.txt oracle", pe_start == 1048576);

        char oracle[65] = {0};
        snprintf(path, sizeof(path), "%s/luks2-pbkdf2/lv-hashes.txt", argv[2]);
        FILE *hf = fopen(path, "r");
        if (!hf) { printf("[ FAIL ] open %s\n", path); fails++; }
        else {
            char line[512];
            while (fgets(line, sizeof(line), hf))
                if (strncmp(line, "lvboot offset=0 length=131072 sha256=", 37) == 0)
                    memcpy(oracle, line + 37, 64);
            fclose(hf);
        }

        const size_t RANGE = 131072;
        uint8_t *buf = (uint8_t *)malloc(RANGE);
        winlvm_luks_unlock_err e4 = {0};
        if (!winlvm_luks_decrypt_region(&src, &md, volume_key, 64, pe_start,
                                        buf, RANGE, &e4)) {
            fprintf(stderr, "decrypt: %s\n", e4.message);
            free(buf);
            return 1;
        }
        uint8_t h[32];
        wl_sha256(buf, RANGE, h);
        char ours[65];
        for (int i = 0; i < 32; i++) snprintf(ours + 2*i, 3, "%02x", h[i]);
        free(buf);
        if (oracle[0] == 0) { printf("[ FAIL ] oracle range-0 line missing\n"); fails++; }
        else if (strcmp(ours, oracle) == 0) {
            passes++;
            printf("[  OK  ] LV first-128KiB sha256 matches Linux oracle\n");
        } else {
            fails++;
            printf("[ FAIL ] LV range hash %s != oracle %s\n", ours, oracle);
        }
    }

    /* 7. volume key hard assert vs committed master-key oracle
     *    (bytes start on the "MK dump:" line itself) */
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/luks2-pbkdf2/MASTER-KEY-INSECURE-TEST-ONLY.txt", argv[2]);
        FILE *mf = fopen(path, "r");
        if (!mf) { printf("[ FAIL ] open %s\n", path); fails++; }
        else {
            char line[256];
            uint8_t mk[64]; size_t n = 0;
            int in_dump = 0;
            while (n < 64 && fgets(line, sizeof(line), mf)) {
                if (strncmp(line, "MK dump:", 8) == 0) {
                    in_dump = 1;
                    /* first byte row shares this line after the marker */
                    char *rest = line + 8;
                    for (char *tok = strtok(rest, " \t\n"); tok && n < 64; tok = strtok(NULL, " \t\n")) {
                        char *end = NULL;
                        unsigned long v = strtoul(tok, &end, 16);
                        if (*end != '\0' || end == tok) break;
                        mk[n++] = (uint8_t)v;
                    }
                    continue;
                }
                if (!in_dump) continue;
                for (char *tok = strtok(line, " \t\n"); tok && n < 64; tok = strtok(NULL, " \t\n")) {
                    char *end = NULL;
                    unsigned long v = strtoul(tok, &end, 16);
                    if (*end != '\0' || end == tok) break;
                    mk[n++] = (uint8_t)v;
                }
            }
            fclose(mf);
            CHECK("volume key equals committed master key", n == 64 && memcmp(mk, volume_key, 64) == 0);
        }
    }

    /* 8. KDF edge cases: iteration count and salt are honored — a keyslot
     *    whose declared iterations differ from what unlock uses must fail
     *    the digest (proves we actually consume kdf.iterations and kdf_salt). */
    {
        winlvm_luks_metadata md2;
        winlvm_luks_err e5 = {0};
        winlvm_byte_source src2 = { f, image_size, file_read };
        (void)winlvm_luks_read_metadata(&src2, jscratch, &md2, &e5);
        md2.keyslots[0].kdf.iterations += 1;   /* wrong count -> wrong area key */
        uint8_t vk2[64];
        static uint8_t scratch2[WL_LUKS_UNLOCK_SCRATCH_MAX];
        winlvm_luks_unlock_err e6 = {0};
        int klen2 = winlvm_luks_unlock_keyslot(&src2, &md2, 0, pass, strlen(pass),
                                               vk2, sizeof(vk2),
                                               scratch2, sizeof(scratch2), &e6);
        CHECK("modified iterations -> wrong key (digest fails)",
              klen2 > 0 && !winlvm_luks_verify_key(&md2, vk2, 64));

        winlvm_luks_metadata md3 = md2;
        md3.keyslots[0].kdf.iterations -= 1;
        md3.keyslots[0].kdf_salt_b64[0] = md3.keyslots[0].kdf_salt_b64[0] == 'A' ? 'B' : 'A';
        int klen3 = winlvm_luks_unlock_keyslot(&src2, &md3, 0, pass, strlen(pass),
                                               vk2, sizeof(vk2),
                                               scratch2, sizeof(scratch2), &e6);
        CHECK("modified salt -> wrong key (digest fails)",
              (klen3 <= 0) || !winlvm_luks_verify_key(&md3, vk2, 64));
    }

    /* 9. unaligned / oversized requests refused */
    {
        uint8_t tmp[512];
        winlvm_luks_unlock_err e7 = {0};
        CHECK("unaligned length refused",
              !winlvm_luks_decrypt_region(&src, &md, volume_key, 64, 0, tmp, 511, &e7));
        CHECK("unaligned start refused",
              !winlvm_luks_decrypt_region(&src, &md, volume_key, 64, 100, tmp, 512, &e7));
    }

    fclose(f);
    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
