/*
 * test_luks_lvm_chain.c — Stage 4 + Stage 1 integration: LUKS2 unlock feeding
 * the freestanding LVM2 parser, producing the same LV content Linux sees.
 *
 * Chain proven here in one run, against the committed corpus:
 *   1. LUKS2 keyslot unlock (PBKDF2-4000 + AF-4000) -> volume key
 *   2. digest verify                            -> wrong-key detection
 *   3. decrypted PV as byte source              -> LABELONE at sector 1
 *   4. winlvm_read_vg                           -> vgboot/lvboot mapping
 *   5. LV range SHA-256                         -> vs corpus lv-hashes oracle
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "luks_read.h"
#include "luks_unlock.h"
#include "lvm_read.h"
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

/* Adapter: lvm_read's winlvm_byte_source over the LUKS-decrypted view. */
typedef struct {
    winlvm_luks_view *view;
} lvm_over_luks;

static size_t view_source_read(void *impl, uint64_t offset, void *buf, size_t len) {
    lvm_over_luks *a = (lvm_over_luks *)impl;
    return winlvm_luks_view_read(a->view, offset, buf, len);
}

static int fails = 0, passes = 0;
#define CHECK(name, cond) do { \
    if (cond) { passes++; printf("[  OK  ] %s\n", name); } \
    else { fails++; printf("[ FAIL ] %s\n", name); } } while (0)

static void hex(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <luks2-pbkdf2.img> <corpus-expected-dir>\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    WL_FSEEK64(f, 0, SEEK_END);
    uint64_t image_size = (uint64_t)WL_FTELL64(f);
    static unsigned char jscratch[WL_LUKS_JSON_SCRATCH_MAX];
    winlvm_byte_source file_src = { f, image_size, file_read };

    /* 1. metadata + unlock */
    winlvm_luks_metadata md;
    winlvm_luks_err err = {0};
    CHECK("luks2 metadata parses", winlvm_luks_read_metadata(&file_src, jscratch, &md, &err));

    uint8_t volume_key[64];
    static uint8_t scratch[WL_LUKS_UNLOCK_SCRATCH_MAX];
    winlvm_luks_unlock_err uerr = {0};
    int klen = winlvm_luks_unlock_keyslot(&file_src, &md, 0,
                                          "winlvm-test-passphrase", 23,
                                          volume_key, sizeof(volume_key),
                                          scratch, sizeof(scratch), &uerr);
    CHECK("keyslot unlock", klen == 64);
    CHECK("volume-key digest verifies", winlvm_luks_verify_key(&md, volume_key, 64));

    /* 2. decrypted view: LABELONE at PV sector 1 */
    winlvm_luks_view view = { &file_src, &md, volume_key, 64 };
    uint8_t label_sec[512];
    CHECK("decrypted view reads 512B at sector 1",
          winlvm_luks_view_read(&view, 512, label_sec, 512) == 512);
    CHECK("PV sector 1 is LABELONE", memcmp(label_sec, "LABELONE", 8) == 0);

    /* 3. LVM2 parse over the decrypted view */
    lvm_over_luks adapter = { &view };
    winlvm_byte_source pv_src = { &adapter, image_size - md.segments[0].offset,
                                 view_source_read };
    winlvm_volume_group vg;
    winlvm_lvm_err lerr = {0};
    CHECK("LVM2 VG parses through LUKS", winlvm_read_vg(&pv_src, &vg, &lerr));
    CHECK("VG is vgboot", strcmp(vg.name, "vgboot") == 0);

    int found = -1;
    for (size_t i = 0; i < vg.lv_count; i++)
        if (strcmp(vg.lvs[i].name, "lvboot") == 0) found = (int)i;
    CHECK("lvboot present and mappable",
          found >= 0 && vg.lvs[found].mappable == 1);

    /* 4. LV range hashes vs the corpus oracle (lv-hashes.txt) */
    if (found >= 0) {
        winlvm_logical_volume *lv = &vg.lvs[found];
        winlvm_physical_volume *pv = &vg.pvs[0];

        /* whole-LV hash, streamed: read extents via the mapping */
        wl_sha256_ctx h;
        wl_sha256_init(&h);
        uint8_t chunk[131072];
        uint64_t lv_bytes = 0;
        for (size_t s = 0; s < lv->segment_count; s++) {
            winlvm_segment *seg = &lv->segments[s];
            if (seg->stripe_count_used != 1) continue;
            uint64_t ext = vg.extent_size_sectors;
            uint64_t seg_start_bytes = seg->start_extent * ext * 512;
            uint64_t seg_len_bytes = seg->extent_count * ext * 512;
            for (uint64_t off = 0; off < seg_len_bytes; off += sizeof(chunk)) {
                uint64_t n = seg_len_bytes - off < sizeof(chunk)
                           ? seg_len_bytes - off : sizeof(chunk);
                uint64_t stripe_start = pv->pe_start + seg->stripes[0].start_extent * ext;
                uint64_t abs = stripe_start * 512 + seg_start_bytes + off;
                if (winlvm_luks_view_read(&view, abs, chunk, (size_t)n) != n) {
                    printf("[ FAIL ] LV read at %llu\n", (unsigned long long)abs);
                    fails++;
                    goto done;
                }
                wl_sha256_update(&h, chunk, (size_t)n);
                lv_bytes += n;
            }
        }
        uint8_t hout[32];
        wl_sha256_final(&h, hout);
        printf("lvboot whole sha256 (winlvm): ");
        hex(hout, 32);
        printf("\n");

        /* oracle: parse lv-hashes.txt for the 'whole' line */
        char path[512];
        snprintf(path, sizeof(path), "%s/luks2-pbkdf2/lv-hashes.txt", argv[2]);
        FILE *hf = fopen(path, "r");
        if (!hf) {
            printf("[ FAIL ] cannot open %s\n", path);
            fails++;
        } else {
            char line[512];
            char oracle_hash[65] = {0};
            while (fgets(line, sizeof(line), hf)) {
                if (strncmp(line, "lvboot whole sha256=", 20) == 0) {
                    strncpy(oracle_hash, line + 20, 64);
                    oracle_hash[64] = '\0';
                }
            }
            fclose(hf);
            char ours[65];
            for (int i = 0; i < 32; i++) snprintf(ours + 2*i, 3, "%02x", hout[i]);
            if (strcmp(ours, oracle_hash) == 0) {
                passes++;
                printf("[  OK  ] lvboot whole-LV sha256 matches Linux oracle\n");
            } else {
                fails++;
                printf("[ FAIL ] oracle %s\n", oracle_hash[0] ? oracle_hash : "(not found)");
            }
        }
        (void)lv_bytes;
    }
done:
    fclose(f);
    printf("\n%d passed, %d failed (luks+lvm chain)\n", passes, fails);
    return fails ? 1 : 0;
}
