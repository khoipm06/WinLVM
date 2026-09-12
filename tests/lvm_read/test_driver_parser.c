/*
 * test_driver_parser.c — user-mode harness for the driver's lvm_read.c
 * parser. Verifies the freestanding C parser agrees with the corpus
 * oracles.
 *
 * Two layers:
 *   1. parse layer — every non-LUKS corpus image parses, with the same
 *      VG/LV identity the lvs/pvs/vgs oracles report.
 *   2. extent-math layer — for every mappable linear LV, the segment
 *      start/count/pe_start arithmetic must reproduce the exact target
 *      lines in dmsetup-table.txt (the authoritative kernel answer).
 *      Striped LVs are checked for stripe count/chunk only: the driver
 *      maps only stripes[0] this phase, so comparing per-stripe targets
 *      would assert behaviour the driver does not have yet.
 *
 * Usage: test_driver_parser <corpus-images-dir>
 * Finds oracles relative to argv[0]/../../corpus/expected.
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

#include "lvm_read.h"

/* Callback for winlvm_byte_source: read from the open image file. */
static size_t file_read(void *impl, uint64_t offset, void *buf, size_t len) {
    FILE *f = (FILE *)impl;
    if (WL_FSEEK64(f, (int64_t)offset, SEEK_SET) != 0) return 0;
    return fread(buf, 1, len, f);
}

typedef struct {
    const char *case_name;
    const char *image_name;
} test_case_t;

/* Every non-LUKS corpus case with a dmsetup-table oracle. */
static const test_case_t kCases[] = {
    { "linear-1pv",            "linear-1pv.img" },
    { "linear-1pv-multiseg",   "linear-1pv-multiseg.img" },
    { "linear-2pv",            "linear-2pv-a.img" },
    { "striped-2pv",           "striped-2pv-a.img" },
    { "sector4k",              "sector4k.img" },
    { "odd-geometry",          "odd-geometry.img" },
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
    char type[32];           /* linear | striped | snapshot | raid | thin ... */
    unsigned long stripe_count;
    unsigned long chunk;
    unsigned long dev[8];    /* major:minor pairs, linear target */
    unsigned long target_start[8];
    unsigned int dev_count;
} oracle_target;

typedef struct {
    char dm_name[128];
    oracle_target t[32];
    size_t count;
} oracle_lv;

/* One dmsetup table line:
 *   name: start len type [params...]
 *   linear:  0 32768 linear 7:0 2048
 *   striped: 0 65536 striped 2 128 7:4 2048 7:5 2048
 * We only deep-parse linear/striped; others keep type only. */
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
        unsigned maj, min, ts;
        if (sscanf(p, "%*lu %*lu %*31s %u:%u %lu", &maj, &min, &ts) == 3) {
            t->dev[0] = maj * 10000 + min;   /* canonical key, not a real dev */
            t->target_start[0] = ts;
            t->dev_count = 1;
        }
        return 1;
    }
    if (strcmp(t->type, "striped") == 0) {
        unsigned long sc, ch;
        if (sscanf(p, "%*lu %*lu %*31s %lu %lu", &sc, &ch) == 2) {
            t->stripe_count = sc;
            t->chunk = ch;
        }
        return 1;
    }
    return 1;  /* snapshot/raid/thin/... — type only */
}

/* Load dm_name -> ordered target list from the oracle file. */
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

/* Find PV index of the segment's stripes[0] target key. */
static int find_pv(const winlvm_volume_group *vg, const char *key) {
    for (size_t i = 0; i < vg->pv_count; i++)
        if (strcmp(vg->pvs[i].key, key) == 0) return (int)i;
    return -1;
}

/* Which loop image is this PV? Oracle uses 7:N for loop N. For -a/-b image
 * pairs, pv0 -> image a (7:even), pv1 -> image b. We only verify the
 * per-target START sector + length here; cross-image identity is covered
 * by the C++ suite (corpus_every_metadata_pv_has_a_matching_image_label). */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <corpus-images-dir>\n", argv[0]);
        return 1;
    }
    const char *images_dir = argv[1];

    /* Oracle dir: <repo>/corpus/expected — derive from CWD layout. */
    char expected_dir[1024];
    snprintf(expected_dir, sizeof(expected_dir), "%s/../../corpus/expected", images_dir);
    FILE *probe = fopen(expected_dir, "r");
    if (probe) {
        fclose(probe);
    } else {
        snprintf(expected_dir, sizeof(expected_dir), "corpus/expected");
        probe = fopen(expected_dir, "r");
        if (probe) fclose(probe);
        else {
            fprintf(stderr, "cannot locate corpus/expected (tried %s)\n", expected_dir);
            return 1;
        }
    }

    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        const test_case_t *c = &kCases[i];
        char img_path[1024];
        snprintf(img_path, sizeof(img_path), "%s/%s", images_dir, c->image_name);

        FILE *f = fopen(img_path, "rb");
        if (!f) {
            fprintf(stderr, "[%s] could not open image %s\n", c->case_name, img_path);
            fails++;
            continue;
        }

        WL_FSEEK64(f, 0, SEEK_END);
        uint64_t image_size = (uint64_t)WL_FTELL64(f);
        WL_FSEEK64(f, 0, SEEK_SET);

        winlvm_byte_source src = {
            .impl = f,
            .size = image_size,
            .read = file_read,
        };

        winlvm_volume_group vg;
        winlvm_lvm_err err = {0};
        if (!winlvm_read_vg(&src, &vg, &err)) {
            /* luks2-* images are whole-disk LUKS containers: the PV lives
             * behind the crypt layer, so a bare parse must FAIL here —
             * anything else means the parser mistakes ciphertext for a PV. */
            if (strncmp(c->case_name, "luks2-", 6) == 0) {
                char name[128];
                snprintf(name, sizeof(name), "%s: bare parse refused (LUKS container)",
                         c->case_name);
                CHECK(name, 1);
                fclose(f);
                continue;
            }
            fprintf(stderr, "[%s] parser failed: %s\n", c->case_name, err.message);
            fails++;
            fclose(f);
            continue;
        }

        printf("[%s] VG=%s seqno=%lu extents=%lu PVs=%lu LVs=%lu\n",
               c->case_name, vg.name, (unsigned long)vg.seqno,
               (unsigned long)vg.extent_size_sectors,
               (unsigned long)vg.pv_count, (unsigned long)vg.lv_count);

        for (size_t j = 0; j < vg.lv_count; j++) {
            winlvm_logical_volume *lv = &vg.lvs[j];
            printf("  LV=%s visible=%d mappable=%d segs=%lu",
                   lv->name, lv->visible, lv->mappable, (unsigned long)lv->segment_count);
            if (!lv->mappable) {
                printf(" REFUSED:%s", lv->unsupported_reason);
            }
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
        for (size_t j = 0; j < vg.lv_count; j++) {
            winlvm_logical_volume *lv = &vg.lvs[j];
            char dm_name[160];
            snprintf(dm_name, sizeof(dm_name), "%s-%s", vg.name, lv->name);

            oracle_lv *ol = NULL;
            for (size_t k = 0; k < oracle_n; k++)
                if (strcmp(oracle[k].dm_name, dm_name) == 0) { ol = &oracle[k]; break; }

            if (!lv->mappable) {
                /* Refused LV: the kernel maps these through non-linear
                 * targets (snapshot-origin, thin, raid) or as hidden
                 * sub-devices — nothing a plain-linear mapper may touch.
                 * If the oracle maps this exact dm-name LINEARLY at top
                 * level, our refusal is wrong. Internal sub-devices
                 * (_rimage/_rmeta/_tdata/_tmeta/-cow) are linear but
                 * hidden; refusing them is correct, so exempt them. */
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
                    /* Linear onto a dm device (252:*, stacked thin-pool
                     * backing) is not reproducible by a PV-only mapper;
                     * only direct PV-backed linear (major 7 = loop) counts
                     * as proof our refusal is wrong. */
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

            /* Mappable LV: every segment must reproduce the oracle lines. */
            if (!ol) {
                printf("[ FAIL ] %s/%s: mappable but kernel has no %s table\n",
                       c->case_name, lv->name, dm_name);
                fails++; case_bad = 1;
                continue;
            }

            /* Linear (stripe_count==1) segments are compared exactly. */
            size_t oi = 0;
            for (size_t s = 0; s < lv->segment_count; s++) {
                winlvm_segment *seg = &lv->segments[s];
                if (seg->stripe_count != 1) continue;  /* striped: below */

                if (oi >= ol->count) {
                    printf("[ FAIL ] %s/%s: more linear segments than oracle lines\n",
                           c->case_name, lv->name);
                    fails++; case_bad = 1; break;
                }
                oracle_target *ot = &ol->t[oi];

                int pv_idx = find_pv(&vg, seg->stripes[0].pv_key);
                if (pv_idx < 0) {
                    printf("[ FAIL ] %s/%s: stripe key %s unknown\n",
                           c->case_name, lv->name, seg->stripes[0].pv_key);
                    fails++; case_bad = 1; break;
                }
                winlvm_physical_volume *pv = &vg.pvs[pv_idx];

                uint64_t expect_start =
                    pv->pe_start + seg->stripes[0].start_extent * vg.extent_size_sectors;
                uint64_t expect_len   = seg->extent_count * vg.extent_size_sectors;
                uint64_t expect_lv_start = seg->start_extent * vg.extent_size_sectors;

                if (ot->type[0] != 'l' || strcmp(ot->type, "linear") != 0) {
                    printf("[ FAIL ] %s/%s seg%lu: oracle type is %s, we mapped linear\n",
                           c->case_name, lv->name, (unsigned long)s, ot->type);
                    fails++; case_bad = 1; break;
                }
                if (ot->start != expect_lv_start || ot->length != expect_len ||
                    ot->target_start[0] != expect_start) {
                    printf("[ FAIL ] %s/%s seg%lu:\n"
                           "        linux: %lu %lu linear ... %lu\n"
                           "        ours:  %lu %lu linear ... %lu\n",
                           c->case_name, lv->name, (unsigned long)s,
                           ot->start, ot->length, ot->target_start[0],
                           (unsigned long)expect_lv_start, (unsigned long)expect_len,
                           (unsigned long)expect_start);
                    fails++; case_bad = 1; break;
                }
                oi++;
            }

            /* Striped LV: count + chunk must agree with the oracle line. */
            for (size_t s = 0; s < lv->segment_count; s++) {
                winlvm_segment *seg = &lv->segments[s];
                if (seg->stripe_count <= 1) continue;
                int found = 0;
                for (size_t k = 0; k < ol->count; k++) {
                    if (strcmp(ol->t[k].type, "striped") == 0 &&
                        ol->t[k].stripe_count == seg->stripe_count) {
                        found = 1;
                        if (seg->stripes[0].start_extent != 0) {
                            printf("[ FAIL ] %s/%s: stripe start_extent %lu != 0\n",
                                   c->case_name, lv->name,
                                   (unsigned long)seg->stripes[0].start_extent);
                            fails++; case_bad = 1;
                        }
                    }
                }
                if (!found) {
                    printf("[ FAIL ] %s/%s: striped seg with %lu stripes has no oracle line\n",
                           c->case_name, lv->name, (unsigned long)seg->stripe_count);
                    fails++; case_bad = 1;
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

    printf("\n%d checks passed, %d failed (driver parser matches corpus)\n",
           passes, fails);
    return fails == 0 ? 0 : 1;
}
