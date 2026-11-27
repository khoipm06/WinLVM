/*
 * test_driver_corpus_map.c — Differential kernel-mapper and block-engine
 * verification against generated LVM2 corpus images and SHA-256 oracles.
 *
 * Exercises the exact freestanding kernel paths:
 *   lvmcore_read_pv_label -> lvmcore_read_vg -> wl_vg_arrival ->
 *   wl_map_begin / wl_map_next (linear & striped) and
 *   wl_complex_lv_read / wl_complex_lv_write (thin, thin-snapshot, snapshot,
 *   snapshot-origin, raid1, mirror).
 *
 * Usage: test_driver_corpus_map <corpus-images-dir> <corpus-expected-dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#ifdef _WIN32
#define WL_FSEEK64 _fseeki64
#define WL_FTELL64 _ftelli64
#else
#define WL_FSEEK64 fseeko
#define WL_FTELL64 ftello
#endif

#include "lvmcore/types.h"
#include "lvmcore/arena.h"
#include "lvmcore/parser.h"
#include "lvmcore/thin.h"
#include "winlvm_vg.h"
#include "test_sha256.h"

#define ARENA_CAP (2u * 1024u * 1024u)
static uint8_t g_arena_pv[2][ARENA_CAP];
static uint8_t g_arena_tmp[ARENA_CAP];
static uint8_t g_io_buf[131072];
static uint8_t g_meta_scratch[LVMCORE_THIN_SCRATCH_META_BYTES];
static uint8_t g_aux_buf[65536];
static uint8_t g_chunk_buf[65536];

static int g_passes = 0;
static int g_fails = 0;
static int g_hashes_verified = 0;

#define CHECK(name, cond) do { \
    if (cond) { g_passes++; printf("[  OK  ] %s\n", (name)); } \
    else { g_fails++; printf("[ FAIL ] %s\n", (name)); } \
} while (0)

static size_t file_read_cb(void *impl, uint64_t offset, void *buf, size_t len) {
    FILE *f = (FILE *)impl;
    if (WL_FSEEK64(f, (int64_t)offset, SEEK_SET) != 0) return 0;
    return fread(buf, 1, len, f);
}

static int pv_file_read(void *ctx, uint32_t pv_slot, uint64_t byte_off, void *buf, size_t len) {
    const wl_vg_record *rec = (const wl_vg_record *)ctx;
    if (!rec || pv_slot >= rec->pv_count || !rec->pv_tag[pv_slot]) return -1;
    FILE *f = (FILE *)(uintptr_t)rec->pv_tag[pv_slot];
    if (WL_FSEEK64(f, (int64_t)byte_off, SEEK_SET) != 0) return -1;
    return (fread(buf, 1, len, f) == len) ? 0 : -1;
}

static int pv_file_write_ro(void *ctx, uint32_t pv_slot, uint64_t byte_off,
                            const void *buf, size_t len) {
    (void)ctx; (void)pv_slot; (void)byte_off; (void)buf; (void)len;
    return -1;
}

static wl_pv_io_ops make_ro_pv_ops(const wl_vg_record *rec) {
    wl_pv_io_ops ops = { (void *)(uintptr_t)rec, pv_file_read, pv_file_write_ro };
    return ops;
}

static int file_open_sized(const char *path, FILE **f_out, uint64_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (WL_FSEEK64(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    int64_t sz = WL_FTELL64(f);
    if (sz < 0 || WL_FSEEK64(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    *f_out = f;
    *size_out = (uint64_t)sz;
    return 1;
}

static int stream_lv_sha256(const lvmcore_volume_group *vg,
                            const wl_vg_record *rec,
                            size_t lv_index,
                            uint64_t offset,
                            uint64_t length,
                            uint64_t xfer_size,
                            char hex_out[65]) {
    test_sha256_ctx sha;
    test_sha256_init(&sha);

    int is_complex = wl_vg_lv_is_complex(vg, lv_index);
    uint64_t cur = offset;
    uint64_t left = length;
    while (left > 0) {
        uint64_t step = left < xfer_size ? left : xfer_size;
        if (is_complex) {
            wl_pv_io_ops ro_ops = make_ro_pv_ops(rec);
            if (step > sizeof(g_io_buf)) step = sizeof(g_io_buf);
            if (wl_complex_lv_read(vg, rec, lv_index, &ro_ops,
                                   g_meta_scratch, sizeof(g_meta_scratch),
                                   g_aux_buf, sizeof(g_aux_buf),
                                   cur, g_io_buf, (size_t)step) != LVMCORE_OK) {
                return 0;
            }
            test_sha256_update(&sha, g_io_buf, (size_t)step);
        } else {
            wl_map_iter it;
            wl_map_begin(&it, vg, rec, lv_index, cur, step);
            uint64_t got_step = 0;
            uint32_t pv_slot = 0;
            uint64_t pv_byte = 0;
            uint64_t piece_len = 0;
            int rc;
            while ((rc = wl_map_next(&it, &pv_slot, &pv_byte, &piece_len)) == 1) {
                if (pv_slot >= rec->pv_count || !rec->pv_tag[pv_slot] || piece_len == 0) return 0;
                FILE *pv_file = (FILE *)rec->pv_tag[pv_slot];
                uint64_t p_off = pv_byte;
                uint64_t p_rem = piece_len;
                while (p_rem > 0) {
                    size_t chunk = p_rem > sizeof(g_io_buf) ? sizeof(g_io_buf) : (size_t)p_rem;
                    if (WL_FSEEK64(pv_file, (int64_t)p_off, SEEK_SET) != 0) return 0;
                    if (fread(g_io_buf, 1, chunk, pv_file) != chunk) return 0;
                    test_sha256_update(&sha, g_io_buf, chunk);
                    p_off += chunk;
                    p_rem -= chunk;
                }
                got_step += piece_len;
            }
            if (rc != 0 || got_step != step) return 0;
        }
        cur += step;
        left -= step;
    }

    test_sha256_final_hex(&sha, hex_out);
    return 1;
}

typedef struct {
    const char *case_name;
    size_t pv_count;
    const char *images[2];
    size_t expected_lvs;
    int test_pv_remove_fail;
    int test_degraded_raid;
} mappable_case_t;

static const mappable_case_t kMappableCases[] = {
    { "linear-1pv",          1, { "linear-1pv.img", NULL },                         1, 0, 0 },
    { "linear-1pv-multiseg", 1, { "linear-1pv-multiseg.img", NULL },                3, 0, 0 },
    { "linear-2pv",          2, { "linear-2pv-a.img", "linear-2pv-b.img" },         1, 1, 0 },
    { "striped-2pv",         2, { "striped-2pv-a.img", "striped-2pv-b.img" },       1, 1, 0 },
    { "sector4k",            1, { "sector4k.img", NULL },                           1, 0, 0 },
    { "odd-geometry",        1, { "odd-geometry.img", NULL },                       1, 0, 0 },
    { "thin-pool",           1, { "thin-pool.img", NULL },                          2, 0, 0 },
    { "snapshot",            1, { "snapshot.img", NULL },                           2, 0, 0 },
    { "raid1-mirror",        2, { "raid1-mirror-a.img", "raid1-mirror-b.img" },     3, 0, 1 },
    { "mixed-vg",            2, { "mixed-vg-a.img", "mixed-vg-b.img" },             4, 0, 0 },
};

static int find_lv_index(const lvmcore_volume_group *vg, const char *lv_name) {
    for (size_t i = 0; i < vg->lv_count; i++) {
        if (strcmp(vg->lvs[i].name, lv_name) == 0) return (int)i;
    }
    return -1;
}

static void verify_mappable_case(const char *images_dir,
                                 const char *expected_dir,
                                 const mappable_case_t *tc) {
    FILE *files[2] = { NULL, NULL };
    uint64_t sizes[2] = { 0, 0 };
    lvmcore_volume_group vgs[2];
    char pv_uuids[2][33];
    char label[256];

    for (size_t p = 0; p < tc->pv_count; p++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", images_dir, tc->images[p]);
        if (!file_open_sized(path, &files[p], &sizes[p])) {
            snprintf(label, sizeof(label), "%s: open %s", tc->case_name, tc->images[p]);
            CHECK(label, 0);
            goto cleanup;
        }

        lvmcore_byte_source src = { files[p], sizes[p], file_read_cb };
        lvmcore_arena tmp_arena;
        lvmcore_arena_init(&tmp_arena, g_arena_tmp, sizeof(g_arena_tmp));
        lvmcore_pv_label pv_lbl;
        lvmcore_err err = {0};
        lvmcore_status st = lvmcore_read_pv_label(&src, &tmp_arena, &pv_lbl, &err);
        if (st != LVMCORE_OK) {
            snprintf(label, sizeof(label), "%s: lvmcore_read_pv_label %s (%s)",
                     tc->case_name, tc->images[p], err.message);
            CHECK(label, 0);
            goto cleanup;
        }
        memcpy(pv_uuids[p], pv_lbl.pv_uuid, sizeof(pv_uuids[p]));

        lvmcore_arena pv_arena;
        lvmcore_arena_init(&pv_arena, g_arena_pv[p], sizeof(g_arena_pv[p]));
        memset(&vgs[p], 0, sizeof(vgs[p]));
        memset(&err, 0, sizeof(err));
        st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &pv_arena, &vgs[p], &err);
        if (st != LVMCORE_OK) {
            snprintf(label, sizeof(label), "%s: lvmcore_read_vg %s (%s)",
                     tc->case_name, tc->images[p], err.message);
            CHECK(label, 0);
            goto cleanup;
        }
    }

    wl_vg_record recs[WL_VG_MAX];
    wl_vg_registry_init(recs, WL_VG_MAX);
    size_t slot = 99;
    size_t newly[WL_VG_MAX_LVS];
    size_t newly_count = 0;

    for (size_t p = 0; p < tc->pv_count; p++) {
        wl_arrival_result ar = wl_vg_arrival(
            recs, WL_VG_MAX, &slot, &vgs[p], pv_uuids[p], files[p],
            NULL, NULL, newly, WL_VG_MAX_LVS, &newly_count);
        if (p + 1u < tc->pv_count) {
            snprintf(label, sizeof(label), "%s: PV%zu arrival pending", tc->case_name, p);
            CHECK(label, ar == WL_ARR_PENDING);
        } else {
            snprintf(label, sizeof(label), "%s: final PV arrival completes VG", tc->case_name);
            CHECK(label, ar == WL_ARR_COMPLETED && slot < WL_VG_MAX &&
                         recs[slot].state == WL_VG_COMPLETE);
        }
    }

    snprintf(label, sizeof(label), "%s: newly eligible LV count == %zu",
             tc->case_name, tc->expected_lvs);
    CHECK(label, newly_count == tc->expected_lvs);

    const lvmcore_volume_group *vg = recs[slot].view;
    char hash_path[1024];
    snprintf(hash_path, sizeof(hash_path), "%s/%s/lv-hashes.txt", expected_dir, tc->case_name);
    FILE *hf = fopen(hash_path, "r");
    snprintf(label, sizeof(label), "%s: open lv-hashes.txt", tc->case_name);
    CHECK(label, hf != NULL);
    if (!hf) goto cleanup;

    static const uint64_t kWindowXferSizes[] = { 513u, 4097u, 65537u };
    char line[512];
    int case_hashes = 0;
    char whole_lv_names[8][64];
    char whole_lv_hex[8][80];
    size_t whole_lv_count = 0;

    while (fgets(line, sizeof(line), hf)) {
        char vg_lv[128];
        uint64_t declared_size = 0;
        if (line[0] == '#') {
            if (sscanf(line, "# %127s size=%" SCNu64, vg_lv, &declared_size) == 2) {
                const char *slash = strchr(vg_lv, '/');
                if (slash) {
                    const char *lv_name = slash + 1;
                    int lv_idx = find_lv_index(vg, lv_name);
                    snprintf(label, sizeof(label), "%s/%s: wl_lv_byte_size == %" PRIu64,
                             tc->case_name, lv_name, declared_size);
                    CHECK(label, lv_idx >= 0 &&
                                 wl_lv_byte_size(vg, (size_t)lv_idx) == declared_size);
                }
            }
            continue;
        }

        char lv_name[64];
        uint64_t off = 0, len = 0;
        char expect_hex[80];

        if (sscanf(line, "%63s offset=%" SCNu64 " length=%" SCNu64 " sha256=%64s",
                   lv_name, &off, &len, expect_hex) == 4) {
            int lv_idx = find_lv_index(vg, lv_name);
            int all_xfer_ok = (lv_idx >= 0);
            for (size_t xi = 0; xi < sizeof(kWindowXferSizes) / sizeof(kWindowXferSizes[0]); xi++) {
                char got_hex[65] = {0};
                if (!all_xfer_ok ||
                    !stream_lv_sha256(vg, &recs[slot], (size_t)lv_idx,
                                      off, len, kWindowXferSizes[xi], got_hex) ||
                    strcmp(got_hex, expect_hex) != 0) {
                    printf("  MISMATCH %s/%s off=%" PRIu64 " len=%" PRIu64 " xfer=%" PRIu64
                           "\n    expect=%s\n    got   =%s\n",
                           tc->case_name, lv_name, off, len, kWindowXferSizes[xi],
                           expect_hex, got_hex);
                    all_xfer_ok = 0;
                    break;
                }
            }
            snprintf(label, sizeof(label), "%s/%s window[%" PRIu64 "+%" PRIu64 "] xfer={513,4097,65537}",
                     tc->case_name, lv_name, off, len);
            CHECK(label, all_xfer_ok);
            if (all_xfer_ok) {
                case_hashes++;
                g_hashes_verified++;
            }
        } else if (sscanf(line, "%63s whole sha256=%64s", lv_name, expect_hex) == 2) {
            int lv_idx = find_lv_index(vg, lv_name);
            uint64_t full_size = (lv_idx >= 0) ? wl_lv_byte_size(vg, (size_t)lv_idx) : 0;
            char got_65537[65] = {0};
            char got_4097[65] = {0};
            int ok = (lv_idx >= 0 && full_size > 0);
            if (ok) {
                ok = stream_lv_sha256(vg, &recs[slot], (size_t)lv_idx,
                                      0, full_size, 65537u, got_65537) &&
                     strcmp(got_65537, expect_hex) == 0;
            }
            if (ok && full_size <= 48u * 1024u * 1024u) {
                ok = stream_lv_sha256(vg, &recs[slot], (size_t)lv_idx,
                                      0, full_size, 4097u, got_4097) &&
                     strcmp(got_4097, expect_hex) == 0;
            }
            if (!ok) {
                printf("  WHOLE MISMATCH %s/%s size=%" PRIu64 "\n    expect=%s\n    got65537=%s\n    got4097=%s\n",
                       tc->case_name, lv_name, full_size, expect_hex, got_65537, got_4097);
            }
            snprintf(label, sizeof(label), "%s/%s whole stream (%" PRIu64 " bytes)",
                     tc->case_name, lv_name, full_size);
            CHECK(label, ok);
            if (ok) {
                case_hashes++;
                g_hashes_verified++;
                if (whole_lv_count < 8u) {
                    snprintf(whole_lv_names[whole_lv_count], 64, "%.63s", lv_name);
                    snprintf(whole_lv_hex[whole_lv_count], 80, "%.64s", expect_hex);
                    whole_lv_count++;
                }
            }
        }
    }
    fclose(hf);

    snprintf(label, sizeof(label), "%s: verified %zu * 5 == %d oracle hashes",
             tc->case_name, tc->expected_lvs, case_hashes);
    CHECK(label, case_hashes == (int)(tc->expected_lvs * 5u));

    if (tc->test_pv_remove_fail && vg->lv_count > 0) {
        wl_vg_pv_removed(recs, WL_VG_MAX, pv_uuids[1]);
        wl_map_iter it;
        uint64_t lv_sz = wl_lv_byte_size(vg, 0);
        uint64_t probe_off = 0u;
        uint64_t probe_len = 131072u;
        if (vg->lvs[0].segment_count > 1u) {
            uint64_t seg1_byte = vg->lvs[0].segments[1].start_extent *
                                 vg->extent_size_sectors * 512ULL;
            if (seg1_byte >= 4096u) probe_off = seg1_byte - 4096u;
        }
        if (probe_off + probe_len > lv_sz) probe_off = 0u;
        wl_map_begin(&it, vg, &recs[slot], 0, probe_off, probe_len);
        uint32_t pv_idx = 0;
        uint64_t pv_b = 0, p_len = 0;
        int rc = 1;
        while ((rc = wl_map_next(&it, &pv_idx, &pv_b, &p_len)) == 1) {
            /* drain pieces on surviving PV until missing PV is hit */
        }
        snprintf(label, sizeof(label), "%s: PV removal fails wl_map_next closed (-1)", tc->case_name);
        CHECK(label, rc == -1);
    }

    if (tc->test_degraded_raid) {
        /* Removing PV0 must allow degraded failover read on PV1 for all 3 raid1/mirror LVs,
         * while writes fail closed without diverging legs. */
        wl_vg_pv_removed(recs, WL_VG_MAX, pv_uuids[0]);
        wl_pv_io_ops ro_ops = make_ro_pv_ops(&recs[slot]);
        for (size_t k = 0; k < whole_lv_count; k++) {
            int lv_idx = find_lv_index(vg, whole_lv_names[k]);
            uint64_t full_size = (lv_idx >= 0) ? wl_lv_byte_size(vg, (size_t)lv_idx) : 0;
            char got_deg[65] = {0};
            uint8_t dummy[512] = {0};
            int deg_ok = (lv_idx >= 0) &&
                         (wl_vg_lv_pv_complete(&recs[slot], (size_t)lv_idx) == 1) &&
                         (wl_vg_lv_raid_all_legs_present(&recs[slot], (size_t)lv_idx) == 0) &&
                         stream_lv_sha256(vg, &recs[slot], (size_t)lv_idx, 0, full_size, 65537u, got_deg) &&
                         (strcmp(got_deg, whole_lv_hex[k]) == 0) &&
                         (wl_complex_lv_write(vg, &recs[slot], (size_t)lv_idx, &ro_ops,
                                              g_meta_scratch, sizeof(g_meta_scratch),
                                              g_chunk_buf, g_aux_buf, sizeof(g_chunk_buf),
                                              0, dummy, 512) != LVMCORE_OK);
            snprintf(label, sizeof(label), "%.63s/%.63s: degraded read failover matches oracle & write fails closed",
                     tc->case_name, whole_lv_names[k]);
            CHECK(label, deg_ok);
        }
        /* Removing PV1 as well must fail reads closed. */
        wl_vg_pv_removed(recs, WL_VG_MAX, pv_uuids[1]);
        int lv0 = find_lv_index(vg, "lvraid1");
        uint8_t buf[512];
        CHECK("raid1-mirror/lvraid1: both PVs removed fails read closed",
              lv0 >= 0 &&
              wl_vg_lv_pv_complete(&recs[slot], (size_t)lv0) == 0 &&
              wl_complex_lv_read(vg, &recs[slot], (size_t)lv0, &ro_ops,
                                 g_meta_scratch, sizeof(g_meta_scratch),
                                 g_aux_buf, sizeof(g_aux_buf),
                                 0, buf, 512) != LVMCORE_OK);
    }

cleanup:
    for (size_t p = 0; p < tc->pv_count; p++) {
        if (files[p]) fclose(files[p]);
    }
}

static void verify_mixed_vg_per_lv_gating(const char *images_dir) {
    FILE *fa = NULL, *fb = NULL;
    uint64_t sa = 0, sb = 0;
    char pa[1024], pb[1024];
    snprintf(pa, sizeof(pa), "%s/mixed-vg-a.img", images_dir);
    snprintf(pb, sizeof(pb), "%s/mixed-vg-b.img", images_dir);
    if (!file_open_sized(pa, &fa, &sa) || !file_open_sized(pb, &fb, &sb)) {
        CHECK("mixed-vg gating: open both PVs", 0);
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return;
    }

    FILE *files[2] = { fa, fb };
    uint64_t sizes[2] = { sa, sb };
    lvmcore_volume_group vgs[2];
    char pv_uuids[2][33];

    for (size_t p = 0; p < 2; p++) {
        lvmcore_byte_source src = { files[p], sizes[p], file_read_cb };
        lvmcore_arena tmp;
        lvmcore_arena_init(&tmp, g_arena_tmp, sizeof(g_arena_tmp));
        lvmcore_pv_label lbl;
        lvmcore_err err = {0};
        lvmcore_read_pv_label(&src, &tmp, &lbl, &err);
        memcpy(pv_uuids[p], lbl.pv_uuid, sizeof(pv_uuids[p]));

        lvmcore_arena arena;
        lvmcore_arena_init(&arena, g_arena_pv[p], sizeof(g_arena_pv[p]));
        memset(&vgs[p], 0, sizeof(vgs[p]));
        lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vgs[p], &err);
    }

    wl_vg_record recs[WL_VG_MAX];
    wl_vg_registry_init(recs, WL_VG_MAX);
    size_t slot = 99;
    size_t newly[WL_VG_MAX_LVS];
    size_t newly_count = 0;
    wl_vg_arrival(recs, WL_VG_MAX, &slot, &vgs[0], pv_uuids[0], files[0], NULL, NULL, newly, WL_VG_MAX_LVS, &newly_count);
    wl_vg_arrival(recs, WL_VG_MAX, &slot, &vgs[1], pv_uuids[1], files[1], NULL, NULL, newly, WL_VG_MAX_LVS, &newly_count);

    const lvmcore_volume_group *vg = recs[slot].view;
    static const char *kRefusedNames[] = {
        "lvcache", "tpool", "snapshot0",
        "lvol0_pmspare", "tpool_tmeta", "tpool_tdata",
        "lvraid_rmeta_0", "lvraid_rimage_0", "lvraid_rmeta_1", "lvraid_rimage_1",
        "lvcache_corig", "cpool", "cpool_cdata", "cpool_cmeta"
    };
    for (size_t k = 0; k < sizeof(kRefusedNames) / sizeof(kRefusedNames[0]); k++) {
        int idx = find_lv_index(vg, kRefusedNames[k]);
        char name[128];
        snprintf(name, sizeof(name), "mixed-vg/%s: ineligible & wl_map_begin fails closed", kRefusedNames[k]);
        int refused = 0;
        if (idx >= 0) {
            wl_map_iter it;
            wl_map_begin(&it, vg, &recs[slot], (size_t)idx, 0, 4096);
            uint32_t pv_idx = 0;
            uint64_t pv_b = 0, p_len = 0;
            refused = (wl_vg_lv_child_eligible(&vg->lvs[idx]) == 0) &&
                      (wl_vg_lv_pv_complete(&recs[slot], (size_t)idx) == 0) &&
                      (it.err != 0) &&
                      (wl_map_next(&it, &pv_idx, &pv_b, &p_len) == -1);
        }
        CHECK(name, refused);
    }

    /* Supported complex LVs MUST be child-eligible, complex, and refuse direct raw wl_map_begin. */
    static const char *kSupportedComplex[] = { "lvplain", "lvsnap", "lvthin", "lvraid" };
    for (size_t k = 0; k < sizeof(kSupportedComplex) / sizeof(kSupportedComplex[0]); k++) {
        int idx = find_lv_index(vg, kSupportedComplex[k]);
        char name[128];
        snprintf(name, sizeof(name), "mixed-vg/%s: eligible complex LV (blocks raw bypass)", kSupportedComplex[k]);
        int ok = 0;
        if (idx >= 0) {
            wl_map_iter it;
            wl_map_begin(&it, vg, &recs[slot], (size_t)idx, 0, 4096);
            ok = (wl_vg_lv_child_eligible(&vg->lvs[idx]) == 1) &&
                 (wl_vg_lv_pv_complete(&recs[slot], (size_t)idx) == 1) &&
                 (wl_vg_lv_is_complex(vg, (size_t)idx) == 1) &&
                 (it.err != 0);
        }
        CHECK(name, ok);
    }

    fclose(fa);
    fclose(fb);
}

static void verify_luks_fail_closed(const char *images_dir) {
    static const char *kLuksImages[] = {
        "luks2-default.img",
        "luks2-pbkdf2.img",
    };
    for (size_t i = 0; i < sizeof(kLuksImages) / sizeof(kLuksImages[0]); i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", images_dir, kLuksImages[i]);
        FILE *f = NULL;
        uint64_t sz = 0;
        char label[160];
        snprintf(label, sizeof(label), "%s: open LUKS2 image", kLuksImages[i]);
        if (!file_open_sized(path, &f, &sz)) {
            CHECK(label, 0);
            continue;
        }
        CHECK(label, 1);

        lvmcore_byte_source src = { f, sz, file_read_cb };
        lvmcore_arena arena;
        lvmcore_arena_init(&arena, g_arena_tmp, sizeof(g_arena_tmp));
        lvmcore_volume_group vg;
        lvmcore_err err = {0};
        lvmcore_status st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vg, &err);
        snprintf(label, sizeof(label), "%s: lvmcore_read_vg returns LVMCORE_ERR_NO_LABEL",
                 kLuksImages[i]);
        CHECK(label, st == LVMCORE_ERR_NO_LABEL);
        fclose(f);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <corpus-images-dir> <corpus-expected-dir>\n", argv[0]);
        return 1;
    }
    const char *images_dir = argv[1];
    const char *expected_dir = argv[2];

    {
        test_sha256_ctx ctx;
        char hex[65];
        test_sha256_init(&ctx);
        test_sha256_final_hex(&ctx, hex);
        CHECK("sha256: NIST empty vector",
              strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
        test_sha256_init(&ctx);
        test_sha256_update(&ctx, "abc", 3u);
        test_sha256_final_hex(&ctx, hex);
        CHECK("sha256: NIST abc vector",
              strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    }

    for (size_t i = 0; i < sizeof(kMappableCases) / sizeof(kMappableCases[0]); i++) {
        verify_mappable_case(images_dir, expected_dir, &kMappableCases[i]);
    }

    CHECK("expected oracle hash count verified across mappable LVs",
          g_hashes_verified == 95);

    verify_mixed_vg_per_lv_gating(images_dir);
    verify_luks_fail_closed(images_dir);

    printf("\ntest_driver_corpus_map: %d passed, %d failed (%d hashes verified)\n",
           g_passes, g_fails, g_hashes_verified);
    return g_fails == 0 ? 0 : 1;
}
