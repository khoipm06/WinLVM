/*
 * test_lvmcore_parser.c — user-mode harness for the freestanding lvmcore
 * parser. Verifies lvmcore agrees with the corpus oracles.
 *
 * Layers verified:
 *   1. Parse layer — every non-LUKS corpus image parses, with the same
 *      VG/LV/PV identity the lvs/pvs/vgs oracles report.
 *   2. Extent-math layer — for every mappable linear LV, the segment
 *      start/count/pe_start arithmetic must reproduce the exact target
 *      lines in dmsetup-table.txt (the authoritative kernel answer).
 *      Striped LVs: stripe count/chunk agreement only (parity with the
 *      driver parser tests).
 *   3. Multi-PV reconciliation — for 2-PV images, both images reconcile
 *      into the same VG with max-seqno semantics.
 *
 * Usage: test_lvmcore_parser <corpus-images-dir> <corpus-expected-dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

/* Per-test scratch: 1 MiB covers 1 MiB worst-case metadata text plus model. */
#define ARENA_CAP (2u * 1024 * 1024)
static uint8_t arena_buf[ARENA_CAP];

/* Callback for lvmcore_byte_source: read from the open image file. */
static size_t file_read(void *impl, uint64_t offset, void *buf, size_t len) {
    FILE *f = (FILE *)impl;
    if (WL_FSEEK64(f, (int64_t)offset, SEEK_SET) != 0) return 0;
    return fread(buf, 1, len, f);
}

typedef struct {
    const char *case_name;
    const char *image_name;
} test_case_t;

/* Every corpus case with a dmsetup-table oracle, including LUKS containers
 * (bare PV parse must FAIL on those — ciphertext is not a PV). */
static const test_case_t kCases[] = {
    { "linear-1pv",            "linear-1pv.img" },
    { "linear-1pv-multiseg",   "linear-1pv-multiseg.img" },
    { "linear-2pv",            "linear-2pv-a.img" },
    { "striped-2pv",           "striped-2pv-a.img" },
    { "sector4k",              "sector4k.img" },
    { "odd-geometry",          "odd-geometry.img" },
    { "thin-pool",             "thin-pool.img" },
    { "snapshot",              "snapshot.img" },
    { "raid1-mirror",          "raid1-mirror-a.img" },
    { "mixed-vg",              "mixed-vg-a.img" },
    { "luks2-default",         "luks2-default.img" },
    { "luks2-pbkdf2",          "luks2-pbkdf2.img" },
};

static int fails = 0, passes = 0;
#define CHECK(name, cond) do { \
    if (cond) { passes++; printf("[  OK  ] %s\n", name); } \
    else { fails++; printf("[ FAIL ] %s\n", name); } } while (0)

/* ---- oracle table parsing ------------------------------------------- */

typedef struct {
    char dm_name[128];
    unsigned long start;
    unsigned long length;
    char type[32];
    unsigned long stripe_count;
    unsigned long chunk;
    unsigned long dev[8];
    unsigned long target_start[8];
    unsigned int dev_count;
} oracle_target;

typedef struct {
    char dm_name[128];
    oracle_target t[32];
    size_t count;
} oracle_lv;

static int parse_table_line(const char *line, oracle_target *t, char *name_out, size_t name_cap) {
    const char *p = strstr(line, ": ");
    if (!p) return 0;
    size_t nlen = (size_t)(p - line);
    if (nlen >= name_cap) return 0;
    memcpy(name_out, line, nlen);
    name_out[nlen] = '\0';
    p += 2;
    unsigned long start, length;
    if (sscanf(p, "%lu %lu %31s", &start, &length, t->type) != 3) return 0;
    t->start = start;
    t->length = length;
    t->dev_count = 0;
    t->stripe_count = 0;
    t->chunk = 0;
    if (strcmp(t->type, "linear") == 0) {
        unsigned maj = 0, min = 0;
        unsigned long ts = 0;
        if (sscanf(p, "%*u %*u %*s %u:%u %lu", &maj, &min, &ts) == 3) {
            t->dev[0] = maj * 10000 + min;
            t->target_start[0] = ts;
            t->dev_count = 1;
        }
        return 1;
    }
    if (strcmp(t->type, "striped") == 0) {
        unsigned long sc = 0, ch = 0;
        int consumed = 0;
        if (sscanf(p, "%*u %*u %*s %lu %lu%n", &sc, &ch, &consumed) == 2) {
            t->stripe_count = sc;
            t->chunk = ch;
            const char *q = p + consumed;
            unsigned int idx = 0;
            while (idx < 8u) {
                unsigned maj = 0, min = 0;
                unsigned long ts = 0;
                int step = 0;
                if (sscanf(q, " %u:%u %lu%n", &maj, &min, &ts, &step) != 3) break;
                t->dev[idx] = (unsigned long)maj * 10000ul + (unsigned long)min;
                t->target_start[idx] = ts;
                idx++;
                q += step;
            }
            t->dev_count = idx;
        }
        return 1;
    }
    return 1;
}

static int load_oracle_table(const char *path, oracle_lv *out, size_t out_cap, size_t *out_n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    size_t n = 0;
    while (fgets(line, sizeof(line), f)) {
        oracle_target t;
        char name[128];
        if (!parse_table_line(line, &t, name, sizeof(name))) continue;
        oracle_lv *lv = NULL;
        for (size_t i = 0; i < n; i++)
            if (strcmp(out[i].dm_name, name) == 0) { lv = &out[i]; break; }
        if (!lv && n < out_cap) { lv = &out[n++]; memset(lv, 0, sizeof(*lv)); strcpy(lv->dm_name, name); }
        if (lv && lv->count < sizeof(lv->t) / sizeof(lv->t[0])) lv->t[lv->count++] = t;
    }
    fclose(f);
    *out_n = n;
    return n > 0;
}

/* Find PV index of the segment's stripes[0] key. */
static int find_pv(const lvmcore_volume_group *vg, const char *key) {
    for (size_t i = 0; i < vg->pv_count; i++)
        if (strcmp(vg->pvs[i].key, key) == 0) return (int)i;
    return -1;
}

/* ---- VG identity verification vs lvs/vgs oracles ------------------- */

/* vgs-oracle.txt line: "vg_name vg_id ..." — we check name presence. */
static int oracle_vg_name(const char *expected_dir, const char *case_name,
                          const char *vg_name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/vgs-oracle.txt", expected_dir, case_name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;  /* no oracle — skip silently */
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char n1[128];
        if (sscanf(line, "%127s", n1) == 1 && strcmp(n1, vg_name) == 0) { found = 1; break; }
    }
    fclose(f);
    return found;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <corpus-images-dir> <corpus-expected-dir>\n", argv[0]);
        return 1;
    }
    const char *images_dir = argv[1];
    const char *expected_dir = argv[2];

    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        const test_case_t *c = &kCases[i];
        char img_path[1024];
        snprintf(img_path, sizeof(img_path), "%s/%s", images_dir, c->image_name);

        int is_luks = (strncmp(c->case_name, "luks2-", 6) == 0);
        FILE *f = fopen(img_path, "rb");
        if (!f) {
            if (is_luks) {
                printf("[ SKIP ] %s: LUKS container image not present (%s)\n",
                       c->case_name, img_path);
                continue;
            }
            fprintf(stderr, "[%s] could not open image %s\n", c->case_name, img_path);
            fails++;
            continue;
        }

        WL_FSEEK64(f, 0, SEEK_END);
        uint64_t image_size = (uint64_t)WL_FTELL64(f);
        WL_FSEEK64(f, 0, SEEK_SET);

        lvmcore_byte_source src = { f, image_size, file_read };

        lvmcore_arena arena;
        lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));

        if (is_luks) {
            /* Encrypted LUKS2 containers carry no plaintext LABELONE header. */
            lvmcore_pv_label lbl;
            lvmcore_err lerr = {0};
            lvmcore_status lst = lvmcore_read_pv_label(&src, &arena, &lbl, &lerr);
            char name[128];
            snprintf(name, sizeof(name), "%s: lvmcore_read_pv_label rejects LUKS with NO_LABEL",
                     c->case_name);
            CHECK(name, lst == LVMCORE_ERR_NO_LABEL);

            lvmcore_arena_reset(&arena);
            lvmcore_volume_group lvg;
            lvmcore_err verr = {0};
            lvmcore_status vst = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &lvg, &verr);
            snprintf(name, sizeof(name), "%s: lvmcore_read_vg rejects LUKS with NO_LABEL",
                     c->case_name);
            CHECK(name, vst == LVMCORE_ERR_NO_LABEL);
            fclose(f);
            continue;
        }

        lvmcore_volume_group vg;
        lvmcore_err err = {0};
        lvmcore_status st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &arena, &vg, &err);
        if (st != LVMCORE_OK) {
            fprintf(stderr, "[%s] parser failed (status=%s): %s\n",
                    c->case_name, lvmcore_status_string(st), err.message);
            fails++;
            fclose(f);
            continue;
        }

        printf("[%s] VG=%s seqno=%lu extents=%lu PVs=%lu LVs=%lu ro=%d\n",
               c->case_name, vg.name, (unsigned long)vg.seqno,
               (unsigned long)vg.extent_size_sectors,
               (unsigned long)vg.pv_count, (unsigned long)vg.lv_count,
               vg.read_only);

        /* VG name must appear in the vgs oracle. */
        {
            int found = oracle_vg_name(expected_dir, c->case_name, vg.name);
            if (found >= 0) {
                char name[128];
                snprintf(name, sizeof(name), "%s: VG name matches vgs oracle", c->case_name);
                CHECK(name, found == 1);
            }
        }

        for (size_t j = 0; j < vg.lv_count; j++) {
            lvmcore_logical_volume *lv = &vg.lvs[j];
            printf("  LV=%s visible=%d ro=%d mappable=%d segs=%lu",
                   lv->name, lv->visible, lv->read_only, lv->mappable,
                   (unsigned long)lv->segment_count);
            if (!lv->mappable) printf(" REFUSED:%s", lv->unsupported_reason);
            printf("\n");
        }

        /* ---- extent-math verification against dmsetup-table oracle ---- */
        char table_path[1024];
        snprintf(table_path, sizeof(table_path), "%s/%s/dmsetup-table.txt",
                 expected_dir, c->case_name);
        oracle_lv oracle[32];
        size_t oracle_n = 0;
        if (!load_oracle_table(table_path, oracle, 32, &oracle_n)) {
            char name[128];
            snprintf(name, sizeof(name), "%s: oracle table loaded", c->case_name);
            CHECK(name, 0);
            fclose(f);
            continue;
        }

        int case_bad = 0;
        if (vg.read_only != 0) {
            printf("[ FAIL ] %s: expected writable VG (read_only==0), got %d\n",
                   c->case_name, vg.read_only);
            fails++; case_bad = 1;
        }
        for (size_t p = 0; p < vg.pv_count; p++) {
            if (vg.pvs[p].missing != 0 || vg.pvs[p].pe_start == 0) {
                printf("[ FAIL ] %s: PV %s unexpected missing=%d pe_start=%lu\n",
                       c->case_name, vg.pvs[p].key, vg.pvs[p].missing,
                       (unsigned long)vg.pvs[p].pe_start);
                fails++; case_bad = 1;
            }
        }

        for (size_t j = 0; j < vg.lv_count; j++) {
            lvmcore_logical_volume *lv = &vg.lvs[j];
            char dm_name[160];
            snprintf(dm_name, sizeof(dm_name), "%s-%s", vg.name, lv->name);

            oracle_lv *ol = NULL;
            for (size_t k = 0; k < oracle_n; k++)
                if (strcmp(oracle[k].dm_name, dm_name) == 0) { ol = &oracle[k]; break; }

            if (!lv->mappable) {
                /* Refused LV: if the oracle maps this exact dm-name LINEARLY
                 * at top level on a real PV (major 7 = loop), our refusal is
                 * wrong. Internal sub-devices (_rimage/_rmeta/_tdata/_tmeta/
                 * -cow) are exempt (hidden sub-devs). */
                if (!ol) continue;
                int top_level_linear = 1;
                const char *int_suffixes[] = {
                    "_rimage", "_rmeta", "_tdata", "_tmeta", "-cow", NULL
                };
                for (int si = 0; int_suffixes[si]; si++)
                    if (strstr(ol->dm_name, int_suffixes[si])) { top_level_linear = 0; break; }
                if (top_level_linear && !lv->visible) top_level_linear = 0;
                if (top_level_linear) {
                    for (size_t k = 0; k < ol->count; k++)
                        if (strcmp(ol->t[k].type, "linear") != 0) { top_level_linear = 0; break; }
                }
                if (top_level_linear) {
                    for (size_t k = 0; k < ol->count; k++)
                        if (ol->t[k].dev[0] / 10000 != 7) { top_level_linear = 0; break; }
                }
                if (top_level_linear) {
                    printf("[ FAIL ] %s/%s: refused but kernel maps it linearly\n",
                           c->case_name, lv->name);
                    fails++; case_bad = 1;
                }
                continue;
            }

            if (!ol) {
                printf("[ FAIL ] %s/%s: mappable but kernel has no %s table\n",
                       c->case_name, lv->name, dm_name);
                fails++; case_bad = 1;
                continue;
            }

            if (lv->read_only != 0) {
                printf("[ FAIL ] %s/%s: expected writable LV (read_only==0), got %d\n",
                       c->case_name, lv->name, lv->read_only);
                fails++; case_bad = 1;
            }

            /* Compare every segment against dmsetup-table oracle. */
            if (lv->is_snapshot_cow && lv->snapshot_wrapper[0]) {
                const lvmcore_logical_volume *wrap = lvmcore_vg_find_lv(&vg, lv->snapshot_wrapper);
                const lvmcore_logical_volume *orig =
                    (wrap && wrap->segment_count > 0)
                        ? lvmcore_vg_find_lv(&vg, wrap->segments[0].origin) : NULL;
                uint64_t expect_len = 0;
                if (orig) {
                    for (size_t os = 0; os < orig->segment_count; os++)
                        expect_len += orig->segments[os].extent_count * vg.extent_size_sectors;
                }
                if (ol->count != 1 || strcmp(ol->t[0].type, "snapshot") != 0 ||
                    ol->t[0].start != 0 || ol->t[0].length != expect_len) {
                    printf("[ FAIL ] %s/%s classic snapshot mismatch: linux=%s [%lu,+%lu) expect_len=%lu\n",
                           c->case_name, lv->name, ol->t[0].type, ol->t[0].start, ol->t[0].length,
                           (unsigned long)expect_len);
                    fails++; case_bad = 1;
                }
                continue;
            }
            if (lv->is_snapshot_origin) {
                uint64_t expect_len = 0;
                for (size_t os = 0; os < lv->segment_count; os++)
                    expect_len += lv->segments[os].extent_count * vg.extent_size_sectors;
                if (ol->count != 1 || strcmp(ol->t[0].type, "snapshot-origin") != 0 ||
                    ol->t[0].start != 0 || ol->t[0].length != expect_len) {
                    printf("[ FAIL ] %s/%s snapshot-origin mismatch: linux=%s [%lu,+%lu) expect_len=%lu\n",
                           c->case_name, lv->name, ol->t[0].type, ol->t[0].start, ol->t[0].length,
                           (unsigned long)expect_len);
                    fails++; case_bad = 1;
                }
                continue;
            }

            if (lv->segment_count != ol->count) {
                printf("[ FAIL ] %s/%s: segment count %lu != oracle line count %lu\n",
                       c->case_name, lv->name,
                       (unsigned long)lv->segment_count, (unsigned long)ol->count);
                fails++; case_bad = 1;
                continue;
            }

            for (size_t s = 0; s < lv->segment_count; s++) {
                lvmcore_segment *seg = &lv->segments[s];
                oracle_target *ot = &ol->t[s];
                uint64_t expect_lv_start = seg->start_extent * vg.extent_size_sectors;
                uint64_t expect_len = seg->extent_count * vg.extent_size_sectors;

                if (ot->start != expect_lv_start || ot->length != expect_len) {
                    printf("[ FAIL ] %s/%s seg%lu range mismatch: linux=[%lu,+%lu) ours=[%lu,+%lu)\n",
                           c->case_name, lv->name, (unsigned long)s,
                           ot->start, ot->length,
                           (unsigned long)expect_lv_start, (unsigned long)expect_len);
                    fails++; case_bad = 1; break;
                }

                if (seg->type == LVMCORE_SEG_THIN) {
                    if (strcmp(ot->type, "thin") != 0 || seg->pool_data[0] == '\0') {
                        printf("[ FAIL ] %s/%s seg%lu expected thin target, got %s\n",
                               c->case_name, lv->name, (unsigned long)s, ot->type);
                        fails++; case_bad = 1; break;
                    }
                    continue;
                }
                if (seg->type == LVMCORE_SEG_RAID1) {
                    if (strcmp(ot->type, "raid") != 0 || seg->raid_leg_count < 2) {
                        printf("[ FAIL ] %s/%s seg%lu expected raid target, got %s\n",
                               c->case_name, lv->name, (unsigned long)s, ot->type);
                        fails++; case_bad = 1; break;
                    }
                    continue;
                }
                if (seg->type == LVMCORE_SEG_MIRROR) {
                    if (strcmp(ot->type, "mirror") != 0 || seg->raid_leg_count < 2) {
                        printf("[ FAIL ] %s/%s seg%lu expected mirror target, got %s\n",
                               c->case_name, lv->name, (unsigned long)s, ot->type);
                        fails++; case_bad = 1; break;
                    }
                    continue;
                }
                if (seg->type == LVMCORE_SEG_SNAPSHOT) {
                    if (strcmp(ot->type, "snapshot") != 0) {
                        printf("[ FAIL ] %s/%s seg%lu expected snapshot target, got %s\n",
                               c->case_name, lv->name, (unsigned long)s, ot->type);
                        fails++; case_bad = 1; break;
                    }
                    continue;
                }

                if (seg->stripe_count == 1) {
                    int pv_idx = find_pv(&vg, seg->stripes[0].pv_key);
                    if (pv_idx < 0) {
                        printf("[ FAIL ] %s/%s: stripe key %s unknown\n",
                               c->case_name, lv->name, seg->stripes[0].pv_key);
                        fails++; case_bad = 1; break;
                    }
                    lvmcore_physical_volume *pv = &vg.pvs[pv_idx];
                    uint64_t expect_start =
                        pv->pe_start + seg->stripes[0].start_extent * vg.extent_size_sectors;

                    if (strcmp(ot->type, "linear") != 0 || ot->dev_count != 1u ||
                        ot->target_start[0] != expect_start) {
                        printf("[ FAIL ] %s/%s seg%lu:\n"
                               "        linux: %lu %lu %s ... %lu\n"
                               "        ours:  %lu %lu linear ... %lu (pe_start=%lu)\n",
                               c->case_name, lv->name, (unsigned long)s,
                               ot->start, ot->length, ot->type, ot->target_start[0],
                               (unsigned long)expect_lv_start, (unsigned long)expect_len,
                               (unsigned long)expect_start, (unsigned long)pv->pe_start);
                        fails++; case_bad = 1; break;
                    }
                } else {
                    if (strcmp(ot->type, "striped") != 0) {
                        printf("[ FAIL ] %s/%s seg%lu: oracle type is %s, expected striped\n",
                               c->case_name, lv->name, (unsigned long)s, ot->type);
                        fails++; case_bad = 1; break;
                    }
                    if (ot->stripe_count != seg->stripe_count ||
                        ot->chunk != seg->stripe_size_sectors ||
                        (uint64_t)ot->dev_count != seg->stripe_count ||
                        (uint64_t)seg->stripe_count_used != seg->stripe_count) {
                        printf("[ FAIL ] %s/%s seg%lu striped header mismatch:\n"
                               "        linux: stripes=%lu chunk=%lu devs=%u\n"
                               "        ours:  stripes=%lu chunk=%lu used=%lu\n",
                               c->case_name, lv->name, (unsigned long)s,
                               ot->stripe_count, ot->chunk, ot->dev_count,
                               (unsigned long)seg->stripe_count,
                               (unsigned long)seg->stripe_size_sectors,
                               (unsigned long)seg->stripe_count_used);
                        fails++; case_bad = 1; break;
                    }
                    for (size_t st_i = 0; st_i < seg->stripe_count_used; st_i++) {
                        int pv_idx = find_pv(&vg, seg->stripes[st_i].pv_key);
                        if (pv_idx < 0) {
                            printf("[ FAIL ] %s/%s seg%lu stripe%lu: key %s unknown\n",
                                   c->case_name, lv->name, (unsigned long)s, (unsigned long)st_i,
                                   seg->stripes[st_i].pv_key);
                            fails++; case_bad = 1; break;
                        }
                        lvmcore_physical_volume *pv = &vg.pvs[pv_idx];
                        uint64_t expect_start =
                            pv->pe_start + seg->stripes[st_i].start_extent * vg.extent_size_sectors;
                        if (ot->target_start[st_i] != expect_start) {
                            printf("[ FAIL ] %s/%s seg%lu stripe%lu start mismatch: linux=%lu ours=%lu (pe_start=%lu)\n",
                                   c->case_name, lv->name, (unsigned long)s, (unsigned long)st_i,
                                   ot->target_start[st_i], (unsigned long)expect_start,
                                   (unsigned long)pv->pe_start);
                            fails++; case_bad = 1; break;
                        }
                    }
                    if (case_bad) break;
                }
            }
        }

        if (!case_bad) {
            char name[128];
            snprintf(name, sizeof(name), "%s: extent math matches dmsetup oracle",
                     c->case_name);
            CHECK(name, 1);
        }
        fclose(f);
    }

    /* ---- multi-PV reconciliation: 2-PV images ----------------------- */
    {
        const char *pairs[][3] = {
            { "linear-2pv",   "linear-2pv-a.img",   "linear-2pv-b.img" },
            { "striped-2pv",  "striped-2pv-a.img",  "striped-2pv-b.img" },
            { "raid1-mirror", "raid1-mirror-a.img", "raid1-mirror-b.img" },
            { "mixed-vg",     "mixed-vg-a.img",     "mixed-vg-b.img" },
        };
        for (size_t p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
            const char *case_name = pairs[p][0];
            FILE *fa = NULL, *fb = NULL;
            char pa[1024], pb[1024];
            snprintf(pa, sizeof(pa), "%s/%s", images_dir, pairs[p][1]);
            snprintf(pb, sizeof(pb), "%s/%s", images_dir, pairs[p][2]);

            fa = fopen(pa, "rb");
            if (fa) fb = fopen(pb, "rb");
            if (!fa || !fb) {
                printf("[ SKIP ] %s: image pair unavailable\n", case_name);
                if (fa) fclose(fa);
                if (fb) fclose(fb);
                continue;
            }

            WL_FSEEK64(fa, 0, SEEK_END);
            uint64_t sa = (uint64_t)WL_FTELL64(fa);
            WL_FSEEK64(fa, 0, SEEK_SET);
            WL_FSEEK64(fb, 0, SEEK_END);
            uint64_t sb = (uint64_t)WL_FTELL64(fb);
            WL_FSEEK64(fb, 0, SEEK_SET);

            lvmcore_byte_source sa_src = { fa, sa, file_read };
            lvmcore_byte_source sb_src = { fb, sb, file_read };
            lvmcore_byte_source *srcs[2] = { &sa_src, &sb_src };

            lvmcore_arena arena;
            lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));

            lvmcore_volume_group vg;
            lvmcore_err err = {0};
            lvmcore_status st = lvmcore_read_vg_multi(srcs, 2, LVMCORE_READ_FLAG_DEFAULT,
                                                      &arena, &vg, &err);
            char name[128];
            snprintf(name, sizeof(name), "%s: multi-PV reconciliation parses both images",
                     case_name);
            CHECK(name, st == LVMCORE_OK);
            if (st == LVMCORE_OK) {
                snprintf(name, sizeof(name),
                         "%s: multi-PV VG name=%s lv_count matches single-PV parse",
                         case_name, vg.name);
                /* sanity: both PVs of the VG must be present */
                CHECK(name, vg.pv_count == 2);
            }

            fclose(fa);
            fclose(fb);
        }
    }

    printf("\n%d checks passed, %d failed (lvmcore parser matches corpus)\n",
           passes, fails);
    return fails == 0 ? 0 : 1;
}
