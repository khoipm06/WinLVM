/*
 * test_lvmcore_differential.c — differential verification gate.
 *
 * Links BOTH the legacy src/driver/lvm_read.c and the new freestanding
 * src/lvmcore into ONE binary. Parses every corpus image with both implementations
 * and asserts exact field-by-field parity:
 *   - VG name, UUID/id, seqno, extent_size_sectors
 *   - PV count, keys, UUIDs, pe_start, pe_count
 *   - LV count, names, UUIDs, visibility, mappability, unsupported reasons
 *   - Segment count, start_extent, extent_count, stripe_count
 *   - Stripe keys and start_extents
 *
 * Ensures ZERO divergences between the old driver parser and the new lvmcore.
 *
 * Usage: test_lvmcore_differential <corpus-images-dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#define WL_FSEEK64 _fseeki64
#define WL_FTELL64 _ftelli64
#else
#define WL_FSEEK64 fseeko
#define WL_FTELL64 ftello
#endif

/* Include both parser headers */
#include "lvm_read.h"
#include "lvmcore/types.h"
#include "lvmcore/arena.h"
#include "lvmcore/parser.h"

#define ARENA_CAP (2u * 1024 * 1024)
static uint8_t arena_buf[ARENA_CAP];

static size_t file_read_cb(void *impl, uint64_t offset, void *buf, size_t len) {
    FILE *f = (FILE *)impl;
    if (WL_FSEEK64(f, (int64_t)offset, SEEK_SET) != 0) return 0;
    return fread(buf, 1, len, f);
}

static int fails = 0, passes = 0;
#define CHECK(name, cond) do { \
    if (cond) { passes++; printf("[  OK  ] %s\n", name); } \
    else { fails++; printf("[ FAIL ] %s\n", name); } } while (0)

/* All corpus images tested */
static const char *kImages[] = {
    "linear-1pv.img",
    "linear-1pv-multiseg.img",
    "linear-2pv-a.img",
    "linear-2pv-b.img",
    "striped-2pv-a.img",
    "striped-2pv-b.img",
    "odd-geometry.img",
    "sector4k.img",
    "mixed-vg-a.img",
    "mixed-vg-b.img",
    "luks2-default.img",
    "luks2-pbkdf2.img",
};

static void diff_compare_vg(const char *image_name,
                            const winlvm_volume_group *old_vg,
                            const lvmcore_volume_group *new_vg) {
    char name[256];

    /* VG name */
    snprintf(name, sizeof(name), "%s: VG name parity (%s vs %s)",
             image_name, old_vg->name, new_vg->name);
    CHECK(name, strcmp(old_vg->name, new_vg->name) == 0);

    /* VG id */
    snprintf(name, sizeof(name), "%s: VG id parity (%s vs %s)",
             image_name, old_vg->id, new_vg->id);
    CHECK(name, strcmp(old_vg->id, new_vg->id) == 0);

    /* VG seqno */
    snprintf(name, sizeof(name), "%s: VG seqno parity (%lu vs %lu)",
             image_name, (unsigned long)old_vg->seqno, (unsigned long)new_vg->seqno);
    CHECK(name, old_vg->seqno == new_vg->seqno);

    /* VG extent size */
    snprintf(name, sizeof(name), "%s: VG extent_size parity (%lu vs %lu)",
             image_name, (unsigned long)old_vg->extent_size_sectors,
             (unsigned long)new_vg->extent_size_sectors);
    CHECK(name, old_vg->extent_size_sectors == new_vg->extent_size_sectors);

    /* PV count */
    snprintf(name, sizeof(name), "%s: PV count parity (%lu vs %lu)",
             image_name, (unsigned long)old_vg->pv_count, (unsigned long)new_vg->pv_count);
    CHECK(name, old_vg->pv_count == new_vg->pv_count);

    /* Compare each PV */
    for (size_t i = 0; i < old_vg->pv_count; i++) {
        const winlvm_physical_volume *old_pv = &old_vg->pvs[i];
        const lvmcore_physical_volume *new_pv = lvmcore_vg_find_pv(new_vg, old_pv->key);
        snprintf(name, sizeof(name), "%s: PV '%s' exists in both", image_name, old_pv->key);
        CHECK(name, new_pv != NULL);
        if (!new_pv) continue;

        snprintf(name, sizeof(name), "%s: PV '%s' UUID parity (%s vs %s)",
                 image_name, old_pv->key, old_pv->id, new_pv->id);
        CHECK(name, strcmp(old_pv->id, new_pv->id) == 0);

        snprintf(name, sizeof(name), "%s: PV '%s' pe_start parity (%lu vs %lu)",
                 image_name, old_pv->key, (unsigned long)old_pv->pe_start,
                 (unsigned long)new_pv->pe_start);
        CHECK(name, old_pv->pe_start == new_pv->pe_start);

        snprintf(name, sizeof(name), "%s: PV '%s' pe_count parity (%lu vs %lu)",
                 image_name, old_pv->key, (unsigned long)old_pv->pe_count,
                 (unsigned long)new_pv->pe_count);
        CHECK(name, old_pv->pe_count == new_pv->pe_count);
    }

    /* LV count */
    snprintf(name, sizeof(name), "%s: LV count parity (%lu vs %lu)",
             image_name, (unsigned long)old_vg->lv_count, (unsigned long)new_vg->lv_count);
    CHECK(name, old_vg->lv_count == new_vg->lv_count);

    /* Compare each LV */
    for (size_t j = 0; j < old_vg->lv_count; j++) {
        const winlvm_logical_volume *old_lv = &old_vg->lvs[j];
        const lvmcore_logical_volume *new_lv = lvmcore_vg_find_lv(new_vg, old_lv->name);
        snprintf(name, sizeof(name), "%s: LV '%s' exists in both", image_name, old_lv->name);
        CHECK(name, new_lv != NULL);
        if (!new_lv) continue;

        snprintf(name, sizeof(name), "%s: LV '%s' UUID parity", image_name, old_lv->name);
        CHECK(name, strcmp(old_lv->id, new_lv->id) == 0);

        snprintf(name, sizeof(name), "%s: LV '%s' visible parity (%d vs %d)",
                 image_name, old_lv->name, old_lv->visible, new_lv->visible);
        CHECK(name, old_lv->visible == new_lv->visible);

        snprintf(name, sizeof(name), "%s: LV '%s' mappable parity (%d vs %d)",
                 image_name, old_lv->name, old_lv->mappable, new_lv->mappable);
        CHECK(name, old_lv->mappable == new_lv->mappable);

        if (!old_lv->mappable && !new_lv->mappable) {
            snprintf(name, sizeof(name), "%s: LV '%s' unsupported reason parity (%s vs %s)",
                     image_name, old_lv->name, old_lv->unsupported_reason, new_lv->unsupported_reason);
            CHECK(name, strcmp(old_lv->unsupported_reason, new_lv->unsupported_reason) == 0);
        }

        snprintf(name, sizeof(name), "%s: LV '%s' segment count parity (%lu vs %lu)",
                 image_name, old_lv->name, (unsigned long)old_lv->segment_count,
                 (unsigned long)new_lv->segment_count);
        CHECK(name, old_lv->segment_count == new_lv->segment_count);

        size_t seg_limit = old_lv->segment_count < new_lv->segment_count ?
                           old_lv->segment_count : new_lv->segment_count;
        for (size_t s = 0; s < seg_limit; s++) {
            const winlvm_segment *old_seg = &old_lv->segments[s];
            const lvmcore_segment *new_seg = &new_lv->segments[s];

            snprintf(name, sizeof(name), "%s: LV '%s' seg%lu start_extent parity (%lu vs %lu)",
                     image_name, old_lv->name, (unsigned long)s,
                     (unsigned long)old_seg->start_extent, (unsigned long)new_seg->start_extent);
            CHECK(name, old_seg->start_extent == new_seg->start_extent);

            snprintf(name, sizeof(name), "%s: LV '%s' seg%lu extent_count parity (%lu vs %lu)",
                     image_name, old_lv->name, (unsigned long)s,
                     (unsigned long)old_seg->extent_count, (unsigned long)new_seg->extent_count);
            CHECK(name, old_seg->extent_count == new_seg->extent_count);

            snprintf(name, sizeof(name), "%s: LV '%s' seg%lu stripe_count parity (%lu vs %lu)",
                     image_name, old_lv->name, (unsigned long)s,
                     (unsigned long)old_seg->stripe_count, (unsigned long)new_seg->stripe_count);
            CHECK(name, old_seg->stripe_count == new_seg->stripe_count);

            snprintf(name, sizeof(name), "%s: LV '%s' seg%lu stripes_used parity (%lu vs %lu)",
                     image_name, old_lv->name, (unsigned long)s,
                     (unsigned long)old_seg->stripe_count_used, (unsigned long)new_seg->stripe_count_used);
            CHECK(name, old_seg->stripe_count_used == new_seg->stripe_count_used);

            size_t stripe_limit = old_seg->stripe_count_used < new_seg->stripe_count_used ?
                                  old_seg->stripe_count_used : new_seg->stripe_count_used;
            for (size_t st = 0; st < stripe_limit; st++) {
                snprintf(name, sizeof(name), "%s: LV '%s' seg%lu stripe%lu key parity (%s vs %s)",
                         image_name, old_lv->name, (unsigned long)s, (unsigned long)st,
                         old_seg->stripes[st].pv_key, new_seg->stripes[st].pv_key);
                CHECK(name, strcmp(old_seg->stripes[st].pv_key, new_seg->stripes[st].pv_key) == 0);

                snprintf(name, sizeof(name), "%s: LV '%s' seg%lu stripe%lu start_extent parity (%lu vs %lu)",
                         image_name, old_lv->name, (unsigned long)s, (unsigned long)st,
                         (unsigned long)old_seg->stripes[st].start_extent,
                         (unsigned long)new_seg->stripes[st].start_extent);
                CHECK(name, old_seg->stripes[st].start_extent == new_seg->stripes[st].start_extent);
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <corpus-images-dir>\n", argv[0]);
        return 1;
    }
    const char *images_dir = argv[1];

    for (size_t i = 0; i < sizeof(kImages) / sizeof(kImages[0]); i++) {
        const char *img_name = kImages[i];
        char img_path[1024];
        snprintf(img_path, sizeof(img_path), "%s/%s", images_dir, img_name);

        FILE *f = fopen(img_path, "rb");
        if (!f) {
            fprintf(stderr, "[%s] could not open image %s\n", img_name, img_path);
            fails++;
            continue;
        }

        WL_FSEEK64(f, 0, SEEK_END);
        uint64_t image_size = (uint64_t)WL_FTELL64(f);
        WL_FSEEK64(f, 0, SEEK_SET);

        winlvm_byte_source old_src = { f, image_size, file_read_cb };
        lvmcore_byte_source new_src = { f, image_size, file_read_cb };

        winlvm_volume_group old_vg;
        winlvm_lvm_err old_err = {0};
        int old_ok = winlvm_read_vg(&old_src, &old_vg, &old_err);

        lvmcore_arena arena;
        lvmcore_arena_init(&arena, arena_buf, sizeof(arena_buf));
        lvmcore_volume_group new_vg;
        lvmcore_err new_err = {0};
        lvmcore_status new_st = lvmcore_read_vg(&new_src, LVMCORE_READ_FLAG_DEFAULT,
                                                &arena, &new_vg, &new_err);
        int new_ok = (new_st == LVMCORE_OK);

        char name[256];
        snprintf(name, sizeof(name), "%s: parse success status parity (old=%d, new=%d)",
                 img_name, old_ok, new_ok);
        CHECK(name, old_ok == new_ok);

        if (old_ok && new_ok) {
            diff_compare_vg(img_name, &old_vg, &new_vg);
        }

        fclose(f);
    }

    printf("\n%d checks passed, %d failed (differential gate old vs new)\n",
           passes, fails);
    return fails == 0 ? 0 : 1;
}
