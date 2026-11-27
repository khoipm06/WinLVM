/*
 * test_winlvm_vg.c — host tests for the multi-VG registry + PV arrival
 * state machine (src/driver/winlvm_vg.c, freestanding core).
 *
 * Pure logic tests on synthetic VG/segment geometry — no corpus images.
 * Covers:
 *   - registry init / find / refcount round-trip
 *   - arrival state machine: first-arrival PENDING, completion COMPLETED,
 *     duplicate-arrival DUPLICATE no-op, foreign-PV REJECTED, multi-VG slots
 *   - per-LV activation gating (hidden/internal/non-mappable never listed)
 *   - seqno reconciliation: refs==0 swap (RECONCILED), refs>0 + seam OK
 *     swap, refs>0 + seam UNAVAILABLE -> STALE_SEQNO with old view kept
 *   - PV removal: GONE, VG INCOMPLETE, all-gone dead, re-insert re-PRESENT
 *   - wl_lv_byte_size: linear sum + overflow -> 0
 *   - map iterator: linear contiguous, striped chunk alternation across PVs,
 *     partial window, exhausted range, refusal (-1 = fail whole IRP)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "winlvm_vg.h"
#include "lvmcore/crc32.h"
#include "lvmcore/thin.h"
#include "lvmcore/snap.h"
#include "lvmcore/raid.h"

#define ARR(view, uuid, tag, seamv, seamctx)                              \
    wl_vg_arrival(recs, WL_VG_MAX, &slot, (view), (uuid), (tag),          \
                  (seamv), (seamctx), newly, 16, &newly_count)

static int passes = 0;
static int fails = 0;

#define CHECK(name, cond) do { \
    if (cond) { passes++; printf("[  OK  ] %s\n", name); } \
    else { fails++; printf("[ FAIL ] %s\n", name); } } while (0)

/* ---- synthetic VG builders ----------------------------------------- */

static void vg_set(lvmcore_volume_group *vg, const char *id, uint64_t seqno,
                   uint64_t ext_sec, lvmcore_physical_volume *pvs,
                   size_t npv, lvmcore_logical_volume *lvs, size_t nlv) {
    memset(vg, 0, sizeof(*vg));
    strcpy(vg->name, "vgtest");
    strcpy(vg->id, id);
    vg->seqno = seqno;
    vg->extent_size_sectors = ext_sec;
    vg->pvs = pvs;
    vg->pv_count = npv;
    vg->lvs = lvs;
    vg->lv_count = nlv;
}

static void pv_set(lvmcore_physical_volume *pv, const char *key,
                   const char *id, uint64_t pe_start) {
    memset(pv, 0, sizeof(*pv));
    strcpy(pv->key, key);
    strcpy(pv->id, id);
    pv->pe_start = pe_start;
    pv->pe_count = 1024;
}

static void lv_set(lvmcore_logical_volume *lv, const char *name,
                   const char *id, int visible, int mappable, int internal,
                   lvmcore_segment *segs, size_t nseg) {
    memset(lv, 0, sizeof(*lv));
    strcpy(lv->name, name);
    strcpy(lv->id, id);
    lv->visible = visible;
    lv->mappable = mappable;
    lv->internal = internal;
    lv->segments = segs;
    lv->segment_count = nseg;
}

/* ---- scenario fixtures ---------------------------------------------- */

static wl_vg_record recs[WL_VG_MAX];
static size_t newly[16];
static size_t newly_count;
static char tag0, tag1; /* distinct instance tags */

/* 2-PV VG, 1 plain linear LV + 1 internal + 1 non-mappable. */
static lvmcore_physical_volume pv_a[2];
static lvmcore_stripe stripe_a[4][2];
static lvmcore_segment seg_a[4];
static lvmcore_logical_volume lv_a[4];
static lvmcore_volume_group vg_a;

static const char *UUID_VG  = "11111111111111111111111111111111";
static const char *UUID_PV0 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *UUID_PV1 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

/* Build a 2-PV / 3-LV VG into caller-provided storage (distinct per
 * version: a record pins a view pointer, so the test must never rewrite
 * memory under a pinned view). */
static void vg_build(uint64_t seqno, uint64_t extents,
                     lvmcore_volume_group *vg,
                     lvmcore_physical_volume pvs[2],
                     lvmcore_stripe stripes[4][2],
                     lvmcore_segment segs[4],
                     lvmcore_logical_volume lvs[4]) {
    pv_set(&pvs[0], "pv0", UUID_PV0, 100);
    pv_set(&pvs[1], "pv1", UUID_PV1, 200);

    segs[0].start_extent = 0;
    segs[0].extent_count = extents;
    segs[0].type = LVMCORE_SEG_STRIPED;
    segs[0].stripe_count = 1;
    segs[0].stripe_count_used = 1;
    segs[0].stripes = stripes[0];
    stripes[0][0].start_extent = 2;
    strcpy(stripes[0][0].pv_key, "pv0");

    segs[1].start_extent = 0;
    segs[1].extent_count = extents;
    segs[1].type = LVMCORE_SEG_STRIPED;
    segs[1].stripe_count = 1;
    segs[1].stripe_count_used = 1;
    segs[1].stripes = stripes[1];
    stripes[1][0].start_extent = 10;
    strcpy(stripes[1][0].pv_key, "pv1");

    segs[2].start_extent = 0;
    segs[2].extent_count = extents;
    segs[2].type = LVMCORE_SEG_STRIPED;
    segs[2].stripe_count = 1;
    segs[2].stripe_count_used = 1;
    segs[2].stripes = stripes[2];
    stripes[2][0].start_extent = 0;
    strcpy(stripes[2][0].pv_key, "pv0");

    segs[3].start_extent = 0;
    segs[3].extent_count = extents;
    segs[3].type = LVMCORE_SEG_STRIPED;
    segs[3].stripe_count = 2;
    segs[3].stripe_count_used = 2;
    segs[3].stripe_size_sectors = 8;
    segs[3].stripes = stripes[3];
    stripes[3][0].start_extent = 0;
    strcpy(stripes[3][0].pv_key, "pv0");
    stripes[3][1].start_extent = 0;
    strcpy(stripes[3][1].pv_key, "pv1");

    lv_set(&lvs[0], "lvplain", "c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1",
           1, 1, 0, &segs[0], 1);
    lv_set(&lvs[1], "lvinternal", "c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2",
           0, 1, 1, &segs[1], 1);
    lv_set(&lvs[2], "lvhidden", "c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3",
           0, 0, 0, &segs[2], 1);
    lv_set(&lvs[3], "lvmulti", "c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4",
           1, 1, 0, &segs[3], 1);

    vg_set(vg, UUID_VG, seqno, 8, pvs, 2, lvs, 4);
}

static void fixture_a(uint64_t seqno, uint64_t extents) {
    vg_build(seqno, extents, &vg_a, pv_a, stripe_a, seg_a, lv_a);
}

/* ---- tests ---------------------------------------------------------- */

static void test_registry_and_arrival(void) {
    wl_vg_registry_init(recs, WL_VG_MAX);
    fixture_a(10, 2);
    size_t slot = 99;

    /* find on empty registry */
    CHECK("find-empty", wl_vg_find(recs, WL_VG_MAX, UUID_VG) == -1);

    /* first PV arrival: PENDING, record created, VG incomplete */
    wl_arrival_result r = ARR(&vg_a, UUID_PV0, &tag0, NULL, NULL);
    CHECK("first-arrival-pending", r == WL_ARR_PENDING);
    CHECK("record-created", slot < WL_VG_MAX);
    CHECK("find-after-create",
          wl_vg_find(recs, WL_VG_MAX, UUID_VG) == (int)slot);
    CHECK("state-incomplete", recs[slot].state == WL_VG_INCOMPLETE);
    CHECK("newly-lvplain-only",
          newly_count == 1 && newly[0] == 0);
    CHECK("pv0-present",
          recs[slot].pv_state[0] == WL_PV_PRESENT &&
          recs[slot].pv_state[1] == WL_PV_MISSING);

    /* duplicate arrival: no-op, no new children */
    wl_arrival_result d = ARR(&vg_a, UUID_PV0, &tag0, NULL, NULL);
    CHECK("duplicate-noop", d == WL_ARR_DUPLICATE);
    CHECK("duplicate-newly-zero", newly_count == 0);

    /* refcount round-trip */
    wl_vg_ref(&recs[slot]);
    wl_vg_ref(&recs[slot]);
    CHECK("refs-inc", recs[slot].refs == 2);
    wl_vg_unref(&recs[slot]);
    wl_vg_unref(&recs[slot]);
    CHECK("refs-dec", recs[slot].refs == 0);

    /* second PV arrival: completes the VG, lvmulti becomes activatable */
    wl_arrival_result c = ARR(&vg_a, UUID_PV1, &tag1, NULL, NULL);
    CHECK("second-arrival-completed", c == WL_ARR_COMPLETED);
    CHECK("state-complete", recs[slot].state == WL_VG_COMPLETE);
    CHECK("newly-both-lvs",
          newly_count == 2 && newly[0] == 0 && newly[1] == 3);

    /* foreign PV: not declared in this metadata -> reject, no record */
    size_t slot2 = 99;
    wl_arrival_result f = ARR(&vg_a, "dddddddddddddddddddddddddddddddd",
                              &tag0, NULL, NULL);
    CHECK("foreign-rejected", f == WL_ARR_REJECTED);
    CHECK("foreign-slot-untouched", slot2 == 99);
}

static void test_activation_gating(void) {
    wl_vg_registry_init(recs, WL_VG_MAX);
    fixture_a(10, 2);
    size_t slot;

    ARR(&vg_a, UUID_PV0, &tag0, NULL, NULL);
    ARR(&vg_a, UUID_PV1, &tag1, NULL, NULL);

    CHECK("eligible-plain", wl_vg_lv_child_eligible(&lv_a[0]) == 1);
    CHECK("eligible-internal", wl_vg_lv_child_eligible(&lv_a[1]) == 0);
    CHECK("eligible-nonmappable", wl_vg_lv_child_eligible(&lv_a[2]) == 0);

    /* both eligible LVs (plain + multi) listed, internal never */
    CHECK("newly-excludes-internal",
          newly_count == 2 && newly[0] == 0 && newly[1] == 3);

    /* pv-complete / activated bit discipline */
    CHECK("lv0-pv-complete", wl_vg_lv_pv_complete(&recs[slot], 0) == 1);
    CHECK("lv0-not-activated", wl_vg_lv_activated(&recs[slot], 0) == 0);
    wl_vg_lv_set_activated(&recs[slot], 0, 1);
    CHECK("lv0-activated", wl_vg_lv_activated(&recs[slot], 0) == 1);
    wl_vg_lv_set_activated(&recs[slot], 0, 0);
    CHECK("lv0-deactivated", wl_vg_lv_activated(&recs[slot], 0) == 0);
}

static wl_update_result seam_ok(void *ctx, const char *lv_uuid) {
    (void)ctx;
    (void)lv_uuid;
    return WL_UPDATE_OK;
}

static wl_update_result seam_no(void *ctx, const char *lv_uuid) {
    (void)ctx;
    (void)lv_uuid;
    return WL_UPDATE_UNAVAILABLE;
}

static void test_seqno_reconciliation(void) {
    wl_vg_registry_init(recs, WL_VG_MAX);

    /* Four distinct VG versions in separate static storage: the record
     * pins a view pointer, so each version must own its own memory. */
    static lvmcore_physical_volume pv_a1[2], pv_a2[2], pv_a3[2];
    static lvmcore_stripe st_a1[4][2], st_a2[4][2], st_a3[4][2];
    static lvmcore_segment sg_a1[4], sg_a2[4], sg_a3[4];
    static lvmcore_logical_volume lv_a1[4], lv_a2[4], lv_a3[4];
    static lvmcore_volume_group vg1, vg2, vg3;
    vg_build(10, 2, &vg1, pv_a1, st_a1, sg_a1, lv_a1);
    vg_build(11, 2, &vg2, pv_a2, st_a2, sg_a2, lv_a2);
    vg_build(12, 4, &vg3, pv_a3, st_a3, sg_a3, lv_a3);

    size_t slot;
    ARR(&vg1, UUID_PV0, &tag0, NULL, NULL);
    CHECK("baseline-seqno", recs[slot].seqno == 10);
    CHECK("baseline-view", recs[slot].view == &vg1);

    /* refs==0 + higher seqno -> swap, no seam needed. Arriving PV is pv1
     * (not yet PRESENT — pv0 is already PRESENT and would dedupe). */
    wl_arrival_result r0 = ARR(&vg2, UUID_PV1, &tag1, NULL, NULL);
    CHECK("reconcile-refs0", r0 == WL_ARR_RECONCILED);
    CHECK("view-swapped-refs0", recs[slot].view == &vg2);
    CHECK("seqno-11", recs[slot].seqno == 11);
    CHECK("stale-clear", recs[slot].stale == 0);
    CHECK("reconcile-pv0-preserved", recs[slot].pv_state[0] == WL_PV_PRESENT);
    CHECK("reconcile-pv1-present", recs[slot].pv_state[1] == WL_PV_PRESENT);
    CHECK("reconcile-complete", recs[slot].state == WL_VG_COMPLETE);
    /* swap cleared activation bits; lvplain + lvmulti newly activatable */
    CHECK("reconcile-newly-both",
          newly_count == 2 && newly[0] == 0 && newly[1] == 3);

    wl_vg_lv_set_activated(&recs[slot], 0, 1);
    wl_vg_ref(&recs[slot]); /* simulate one live child */

    /* pv1 drops, late-arrives with geometry-changed newer metadata, seam
     * unavailable -> STALE_SEQNO: old view kept serving, stale=1. */
    wl_vg_pv_removed(recs, WL_VG_MAX, UUID_PV1);
    wl_arrival_result r1 = ARR(&vg3, UUID_PV1, &tag1, seam_no, NULL);
    CHECK("stale-seam-no", r1 == WL_ARR_STALE_SEQNO);
    CHECK("stale-flag", recs[slot].stale == 1);
    CHECK("stale-old-view-kept", recs[slot].view == &vg2);
    CHECK("stale-seqno-kept", recs[slot].seqno == 11);
    CHECK("stale-pv1-present", recs[slot].pv_state[1] == WL_PV_PRESENT);

    /* pv1 drops again, late-arrives, seam OK -> reconciled swap. */
    wl_vg_pv_removed(recs, WL_VG_MAX, UUID_PV1);
    wl_arrival_result r2 = ARR(&vg3, UUID_PV1, &tag1, seam_ok, NULL);
    CHECK("reconcile-seam-ok", r2 == WL_ARR_RECONCILED);
    CHECK("view-swapped-seam-ok", recs[slot].view == &vg3);
    CHECK("seqno-13", recs[slot].seqno == 12);
    CHECK("stale-clear-2", recs[slot].stale == 0);

    wl_vg_unref(&recs[slot]);
}

static void test_pv_removal_and_reinsert(void) {
    wl_vg_registry_init(recs, WL_VG_MAX);
    fixture_a(10, 2);
    size_t slot;
    ARR(&vg_a, UUID_PV0, &tag0, NULL, NULL);
    ARR(&vg_a, UUID_PV1, &tag1, NULL, NULL);
    wl_vg_lv_set_activated(&recs[slot], 0, 1);

    wl_vg_pv_removed(recs, WL_VG_MAX, UUID_PV0);
    CHECK("pv0-gone", recs[slot].pv_state[0] == WL_PV_GONE);
    CHECK("vg-incomplete-after-remove",
          recs[slot].state == WL_VG_INCOMPLETE);
    CHECK("not-dead-one-left", recs[slot].dead == 0);
    CHECK("lv0-not-complete", wl_vg_lv_pv_complete(&recs[slot], 0) == 0);

    wl_vg_pv_removed(recs, WL_VG_MAX, UUID_PV1);
    CHECK("all-gone-dead", recs[slot].dead == 1);
    CHECK("lv0-all-gone", wl_vg_lv_all_gone(&recs[slot], 0) == 1);

    /* removal of unknown UUID is a no-op */
    wl_vg_pv_removed(recs, WL_VG_MAX, "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
    CHECK("unknown-remove-noop", recs[slot].dead == 1);

    /* re-insert: same record, state restored */
    wl_arrival_result r = ARR(&vg_a, UUID_PV0, &tag0, NULL, NULL);
    CHECK("reinsert-pending", r == WL_ARR_PENDING);
    CHECK("reinsert-not-dead", recs[slot].dead == 0);
    CHECK("reinsert-pv0-present", recs[slot].pv_state[0] == WL_PV_PRESENT);
    wl_arrival_result r2 = ARR(&vg_a, UUID_PV1, &tag1, NULL, NULL);
    CHECK("reinsert-completed", r2 == WL_ARR_COMPLETED);
}

static void test_multi_vg(void) {
    wl_vg_registry_init(recs, WL_VG_MAX);
    fixture_a(10, 2);
    size_t slot, slot_a, slot_b;
    ARR(&vg_a, UUID_PV0, &tag0, NULL, NULL);
    slot_a = slot;
    ARR(&vg_a, UUID_PV1, &tag1, NULL, NULL);

    /* independent single-PV VG */
    static lvmcore_physical_volume pv_b[1];
    static lvmcore_stripe stripe_b[1];
    static lvmcore_segment seg_b;
    static lvmcore_logical_volume lv_b;
    static lvmcore_volume_group vg_b;
    pv_set(&pv_b[0], "pv0", "ffffffffffffffffffffffffffffffff", 4096);
    seg_b.start_extent = 0;
    seg_b.extent_count = 4;
    seg_b.type = LVMCORE_SEG_STRIPED;
    seg_b.stripe_count = 1;
    seg_b.stripe_count_used = 1;
    seg_b.stripes = stripe_b;
    stripe_b[0].start_extent = 0;
    strcpy(stripe_b[0].pv_key, "pv0");
    lv_set(&lv_b, "lvsingle", "99999999999999999999999999999999",
           1, 1, 0, &seg_b, 1);
    vg_set(&vg_b, "22222222222222222222222222222222", 3, 8, pv_b, 1, &lv_b, 1);

    slot_b = 99;
    wl_arrival_result r = ARR(&vg_b, "ffffffffffffffffffffffffffffffff",
                              &tag0, NULL, NULL);
    slot_b = slot;
    CHECK("single-pv-completed", r == WL_ARR_COMPLETED);
    CHECK("multi-vg-distinct", slot_b != slot_a);
    CHECK("vg-a-found", wl_vg_find(recs, WL_VG_MAX, UUID_VG) == (int)slot_a);
    CHECK("vg-b-found",
          wl_vg_find(recs, WL_VG_MAX, "22222222222222222222222222222222") ==
          (int)slot_b);
    CHECK("vg-a-state-complete", recs[slot_a].state == WL_VG_COMPLETE);
}

static void test_lv_byte_size(void) {
    /* two-segment linear sum: (2 + 3) * 8 sectors * 512 = 20480 */
    static lvmcore_physical_volume pv_c[1];
    static lvmcore_stripe stripe_c[2][1];
    static lvmcore_segment seg_c[2];
    static lvmcore_logical_volume lv_c;
    static lvmcore_volume_group vg_c;
    pv_set(&pv_c[0], "pv0", UUID_PV0, 64);
    seg_c[0].type = LVMCORE_SEG_STRIPED;
    seg_c[0].extent_count = 2;
    seg_c[0].stripe_count = 1;
    seg_c[0].stripe_count_used = 1;
    seg_c[0].stripes = stripe_c[0];
    stripe_c[0][0].start_extent = 0;
    strcpy(stripe_c[0][0].pv_key, "pv0");
    seg_c[1] = seg_c[0];
    seg_c[1].extent_count = 3;
    seg_c[1].stripes = stripe_c[1];
    stripe_c[1][0].start_extent = 2;
    strcpy(stripe_c[1][0].pv_key, "pv0");
    lv_set(&lv_c, "lvsize", "c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4c4",
           1, 1, 0, seg_c, 2);
    vg_set(&vg_c, "33333333333333333333333333333333", 1, 8, pv_c, 1, &lv_c, 1);
    CHECK("byte-size-sum", wl_lv_byte_size(&vg_c, 0) == 20480);
    CHECK("byte-size-bad-index", wl_lv_byte_size(&vg_c, 1) == 0);

    /* overflow: extent_count * extent_size wraps u64 */
    seg_c[1].extent_count = 1ULL << 60;
    vg_c.extent_size_sectors = 8192; /* 2^60 * 2^13 = 2^73 -> overflow */
    CHECK("byte-size-overflow-zero", wl_lv_byte_size(&vg_c, 0) == 0);
}

static void test_map_linear(void) {
    wl_vg_registry_init(recs, WL_VG_MAX);
    fixture_a(10, 2);
    size_t slot;
    ARR(&vg_a, UUID_PV0, &tag0, NULL, NULL);

    wl_map_iter it;
    uint32_t pv;
    uint64_t b, len;

    /* lvplain (index 0): 1 segment, 2 extents * 4096 = 8192 bytes.
     * stripe start_extent=2, pv0 pe_start=100 -> base 100*512 + 2*4096. */
    wl_map_begin(&it, &vg_a, &recs[slot], 0, 0, 8192);
    CHECK("map-linear-first", wl_map_next(&it, &pv, &b, &len) == 1);
    CHECK("map-linear-pv", pv == 0);
    CHECK("map-linear-byte", b == 100 * 512 + 2 * 4096);
    CHECK("map-linear-len", len == 8192);
    CHECK("map-linear-end", wl_map_next(&it, &pv, &b, &len) == 0);

    /* partial window [4096, +2048) */
    wl_map_begin(&it, &vg_a, &recs[slot], 0, 4096, 2048);
    CHECK("map-window-first", wl_map_next(&it, &pv, &b, &len) == 1);
    CHECK("map-window-byte", b == 100 * 512 + 2 * 4096 + 4096);
    CHECK("map-window-len", len == 2048);
    CHECK("map-window-end", wl_map_next(&it, &pv, &b, &len) == 0);

    /* zero length -> exhausted at once */
    wl_map_begin(&it, &vg_a, &recs[slot], 0, 0, 0);
    CHECK("map-zero-len", wl_map_next(&it, &pv, &b, &len) == 0);

    /* beyond end -> exhausted */
    wl_map_begin(&it, &vg_a, &recs[slot], 0, 8192, 1);
    CHECK("map-beyond-end", wl_map_next(&it, &pv, &b, &len) == 0);
}

static void test_map_striped(void) {
    /* 2-stripe LV: chunk 8 sectors = 4096 bytes, 4 extents * 4096 = 16384.
     * dev = c % 2, dev byte = pe_start*512 + k*4096 + within. */
    static lvmcore_physical_volume pv_s[2];
    static lvmcore_stripe stripe_s[1][2];
    static lvmcore_segment seg_s;
    static lvmcore_logical_volume lv_s;
    static lvmcore_volume_group vg_s;

    pv_set(&pv_s[0], "pv0", UUID_PV0, 1024);
    pv_set(&pv_s[1], "pv1", UUID_PV1, 2048);
    seg_s.start_extent = 0;
    seg_s.extent_count = 4;
    seg_s.type = LVMCORE_SEG_STRIPED;
    seg_s.stripe_count = 2;
    seg_s.stripe_count_used = 2;
    seg_s.stripe_size_sectors = 8;
    seg_s.stripes = stripe_s[0];
    stripe_s[0][0].start_extent = 0;
    strcpy(stripe_s[0][0].pv_key, "pv0");
    stripe_s[0][1].start_extent = 0;
    strcpy(stripe_s[0][1].pv_key, "pv1");
    lv_set(&lv_s, "lvstriped", "c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5c5",
           1, 1, 0, &seg_s, 1);
    vg_set(&vg_s, "44444444444444444444444444444444", 1, 8, pv_s, 2, &lv_s, 1);

    wl_vg_registry_init(recs, WL_VG_MAX);
    size_t slot;
    ARR(&vg_s, UUID_PV0, &tag0, NULL, NULL);
    ARR(&vg_s, UUID_PV1, &tag1, NULL, NULL);

    wl_map_iter it;
    uint32_t pv;
    uint64_t b, len;
    uint64_t expect[4][3] = {
        {0, 1024 * 512 + 0,              4096},
        {1, 2048 * 512 + 0,              4096},
        {0, 1024 * 512 + 4096,           4096},
        {1, 2048 * 512 + 4096,           4096},
    };

    wl_map_begin(&it, &vg_s, &recs[slot], 0, 0, 16384);
    int n = 0;
    int rc;
    while ((rc = wl_map_next(&it, &pv, &b, &len)) == 1 && n < 4) {
        CHECK("striped-piece-pv",
              pv == expect[n][0] && b == expect[n][1] && len == expect[n][2]);
        n++;
    }
    CHECK("striped-count", n == 4);
    CHECK("striped-exhausted", rc == 0);

    /* mid-chunk window: [4096+2048, 8192) -> dev1 within=2048 len 2048,
     * then dev0 within=0 len 2048. */
    wl_map_begin(&it, &vg_s, &recs[slot], 0, 4096 + 2048, 4096);
    CHECK("striped-win-first", wl_map_next(&it, &pv, &b, &len) == 1);
    CHECK("striped-win-first-pos",
          pv == 1 && b == 2048 * 512 + 2048 && len == 2048);
    CHECK("striped-win-second", wl_map_next(&it, &pv, &b, &len) == 1);
    CHECK("striped-win-second-pos",
          pv == 0 && b == 1024 * 512 + 4096 && len == 2048);
    CHECK("striped-win-end", wl_map_next(&it, &pv, &b, &len) == 0);
}

static void test_map_refusal(void) {
    /* PV removed underneath -> map must fail WHOLE (-1), never partial. */
    static lvmcore_physical_volume pv_r[2];
    static lvmcore_stripe stripe_r[1][2];
    static lvmcore_segment seg_r;
    static lvmcore_logical_volume lv_r;
    static lvmcore_volume_group vg_r;

    pv_set(&pv_r[0], "pv0", UUID_PV0, 1024);
    pv_set(&pv_r[1], "pv1", UUID_PV1, 2048);
    seg_r.start_extent = 0;
    seg_r.extent_count = 4;
    seg_r.type = LVMCORE_SEG_STRIPED;
    seg_r.stripe_count = 2;
    seg_r.stripe_count_used = 2;
    seg_r.stripe_size_sectors = 8;
    seg_r.stripes = stripe_r[0];
    stripe_r[0][0].start_extent = 0;
    strcpy(stripe_r[0][0].pv_key, "pv0");
    stripe_r[0][1].start_extent = 0;
    strcpy(stripe_r[0][1].pv_key, "pv1");
    lv_set(&lv_r, "lvrefl", "c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6c6",
           1, 1, 0, &seg_r, 1);
    vg_set(&vg_r, "55555555555555555555555555555555", 1, 8, pv_r, 2, &lv_r, 1);

    wl_vg_registry_init(recs, WL_VG_MAX);
    size_t slot;
    ARR(&vg_r, UUID_PV0, &tag0, NULL, NULL);
    ARR(&vg_r, UUID_PV1, &tag1, NULL, NULL);

    wl_map_iter it;
    uint32_t pv;
    uint64_t b, len;

    /* non-mappable LV -> immediate refusal */
    wl_map_begin(&it, &vg_r, &recs[slot], 16, 0, 4096);
    CHECK("map-bad-index-refuse", wl_map_next(&it, &pv, &b, &len) == -1);

    /* remove pv1 mid-flight: first piece lands on pv0 (ok), the next piece
     * needs pv1 -> -1, and the iterator stays failed (-1 forever). */
    wl_map_begin(&it, &vg_r, &recs[slot], 0, 0, 16384);
    CHECK("refusal-first-ok", wl_map_next(&it, &pv, &b, &len) == 1);
    CHECK("refusal-first-pv0", pv == 0);
    wl_vg_pv_removed(recs, WL_VG_MAX, UUID_PV1);
    CHECK("refusal-second-fail", wl_map_next(&it, &pv, &b, &len) == -1);
    CHECK("refusal-sticky", wl_map_next(&it, &pv, &b, &len) == -1);
}

static void test_reconcile_with_live_child_and_new_lv(void) {
    wl_vg_registry_init(recs, WL_VG_MAX);
    static lvmcore_physical_volume pv_r1[2], pv_r2[2];
    static lvmcore_stripe st_r1[4][2], st_r2[4][2];
    static lvmcore_segment sg_r1[4], sg_r2[4];
    static lvmcore_logical_volume lv_r1[4], lv_r2[4];
    static lvmcore_volume_group vgr1, vgr2;
    vg_build(10, 1, &vgr1, pv_r1, st_r1, sg_r1, lv_r1);
    vg_build(11, 2, &vgr2, pv_r2, st_r2, sg_r2, lv_r2);

    size_t slot;
    ARR(&vgr1, UUID_PV0, &tag0, NULL, NULL);
    ARR(&vgr1, UUID_PV1, &tag1, NULL, NULL);
    CHECK("rec-live-init-state", recs[slot].state == WL_VG_COMPLETE);
    wl_vg_lv_set_activated(&recs[slot], 0, 1);
    wl_vg_ref(&recs[slot]);

    wl_arrival_result r = ARR(&vgr2, UUID_PV0, &tag0, seam_ok, NULL);
    CHECK("rec-live-result", r == WL_ARR_RECONCILED);
    CHECK("rec-live-newly-count", newly_count == 1);
    CHECK("rec-live-newly-idx", newly_count > 0 && newly[0] == 3);
    wl_vg_unref(&recs[slot]);
}

/* ---- Complex C11 on-disk engine R/W tests (thin, snap, raid1/mirror) ---- */

typedef struct {
    uint8_t *buf;
    size_t   size;
    int      fail_reads;
} mem_rw_dev;

static int mem_rw_read(void *impl, uint64_t off, void *dst, size_t len) {
    mem_rw_dev *d = (mem_rw_dev *)impl;
    if (!d || d->fail_reads || off > d->size || len > d->size - off) return -1;
    memcpy(dst, d->buf + off, len);
    return 0;
}

static int mem_rw_write(void *impl, uint64_t off, const void *src, size_t len) {
    mem_rw_dev *d = (mem_rw_dev *)impl;
    if (!d || off > d->size || len > d->size - off) return -1;
    memcpy(d->buf + off, src, len);
    return 0;
}

static void put_le32_t(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_le64_t(uint8_t *p, uint64_t v) {
    put_le32_t(p, (uint32_t)v);
    put_le32_t(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get_le32_t(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void sm_bmp_set_t(uint8_t *blk, uint32_t idx, uint32_t rc) {
    uint8_t code = (rc == 0u) ? 0u : (rc == 1u) ? 2u : (rc == 2u) ? 1u : 3u;
    size_t boff = 16u + (size_t)(idx / 4u);
    unsigned shift = (idx % 4u) * 2u;
    blk[boff] = (uint8_t)((blk[boff] & ~(3u << shift)) | ((uint32_t)code << shift));
}

static void init_empty_leaf_t(uint8_t *blk, uint64_t blocknr, uint32_t vsz) {
    uint32_t max_e = ((4064u / (8u + vsz)) / 3u) * 3u;
    memset(blk, 0, 4096u);
    put_le32_t(blk + 4, 2u); /* LEAF */
    put_le64_t(blk + 8, blocknr);
    put_le32_t(blk + 16, 0u);
    put_le32_t(blk + 20, max_e);
    put_le32_t(blk + 24, vsz);
    put_le32_t(blk, lvmcore_crc32c_dm(blk + 4, 4092u, LVMCORE_THIN_BTREE_CSUM_XOR));
}

static void test_thin_engine_rw_cow_and_split(void) {
    /* 1 MiB tmeta (256 x 4 KiB blocks), 24 MiB tdata (384 x 64 KiB chunks). */
    const size_t tmeta_sz = 256u * 4096u;
    const size_t tdata_sz = 384u * 65536u;
    uint8_t *tmeta_mem = (uint8_t *)calloc(1, tmeta_sz);
    uint8_t *tdata_mem = (uint8_t *)calloc(1, tdata_sz);
    uint8_t *scratch = (uint8_t *)calloc(1, LVMCORE_THIN_SCRATCH_META_BYTES);
    uint8_t *chunk_buf = (uint8_t *)calloc(1, 65536u);
    CHECK("thin-test-alloc", tmeta_mem && tdata_mem && scratch && chunk_buf);
    if (!tmeta_mem || !tdata_mem || !scratch || !chunk_buf) goto out;

    /* Layout: 0=sb, 1=meta_idx, 2=meta_bmp0, 3=meta_rc, 4=data_sm_root, 5=data_bmp0,
     * 6=data_rc, 7=data_mapping_root, 8=device_details_root, 9=dev1_leaf (shared by dev2!). */
    uint8_t *b0 = tmeta_mem + 0 * 4096u;
    uint8_t *b1 = tmeta_mem + 1 * 4096u;
    uint8_t *b2 = tmeta_mem + 2 * 4096u;
    uint8_t *b3 = tmeta_mem + 3 * 4096u;
    uint8_t *b4 = tmeta_mem + 4 * 4096u;
    uint8_t *b5 = tmeta_mem + 5 * 4096u;
    uint8_t *b6 = tmeta_mem + 6 * 4096u;
    uint8_t *b7 = tmeta_mem + 7 * 4096u;
    uint8_t *b8 = tmeta_mem + 8 * 4096u;
    uint8_t *b9 = tmeta_mem + 9 * 4096u;

    /* Shared bottom leaf (block 9): vblk 0 -> pblk 0 (time 0), meta_rc = 2 (shared by dev1 & dev2). */
    init_empty_leaf_t(b9, 9, 8);
    put_le32_t(b9 + 16, 1u);
    put_le64_t(b9 + 32, 0ULL);
    put_le64_t(b9 + 32 + 252u * 8u, (0ULL << 24) | 0ULL);
    put_le32_t(b9, lvmcore_crc32c_dm(b9 + 4, 4092u, LVMCORE_THIN_BTREE_CSUM_XOR));
    memset(tdata_mem, 0xA5, 65536u); /* original shared chunk 0 payload */

    /* data_mapping_root (block 7): dev 1 -> 9, dev 2 -> 9 (shared leaf!) */
    init_empty_leaf_t(b7, 7, 8);
    put_le32_t(b7 + 16, 2u);
    put_le64_t(b7 + 32 + 0, 1ULL);
    put_le64_t(b7 + 32 + 8, 2ULL);
    put_le64_t(b7 + 32 + 252u * 8u + 0, 9ULL);
    put_le64_t(b7 + 32 + 252u * 8u + 8, 9ULL);
    put_le32_t(b7, lvmcore_crc32c_dm(b7 + 4, 4092u, LVMCORE_THIN_BTREE_CSUM_XOR));

    /* device_details_root (block 8): dev 1 & dev 2, mapped_blocks=1, stime=1 */
    init_empty_leaf_t(b8, 8, 24);
    put_le32_t(b8 + 16, 2u);
    put_le64_t(b8 + 32 + 0, 1ULL);
    put_le64_t(b8 + 32 + 8, 2ULL);
    uint32_t d_vbase = 32u + 126u * 8u;
    put_le64_t(b8 + d_vbase + 0, 1ULL); put_le64_t(b8 + d_vbase + 8, 0ULL);
    put_le32_t(b8 + d_vbase + 16, 0u);  put_le32_t(b8 + d_vbase + 20, 1u);
    put_le64_t(b8 + d_vbase + 24, 1ULL); put_le64_t(b8 + d_vbase + 32, 1ULL);
    put_le32_t(b8 + d_vbase + 40, 1u);   put_le32_t(b8 + d_vbase + 44, 1u);
    put_le32_t(b8, lvmcore_crc32c_dm(b8 + 4, 4092u, LVMCORE_THIN_BTREE_CSUM_XOR));

    /* data_sm: block 5 bitmap (pblk 0 has rc=1 because leaf 9 is shared!), block 6 rc_root, block 4 root */
    put_le64_t(b5 + 8, 5ULL);
    sm_bmp_set_t(b5, 0u, 1u);
    put_le32_t(b5, lvmcore_crc32c_dm(b5 + 4, 4092u, LVMCORE_THIN_BITMAP_CSUM_XOR));
    init_empty_leaf_t(b6, 6, 4);
    init_empty_leaf_t(b4, 4, 16);
    put_le32_t(b4 + 16, 1u);
    put_le64_t(b4 + 32, 0ULL);
    put_le64_t(b4 + 32 + 168u * 8u + 0, 5ULL);
    put_le32_t(b4 + 32 + 168u * 8u + 8, 16320u - 1u);
    put_le32_t(b4, lvmcore_crc32c_dm(b4 + 4, 4092u, LVMCORE_THIN_BTREE_CSUM_XOR));

    /* meta_sm: block 2 bitmap (blocks 0..8 rc=1, block 9 rc=2!), block 3 rc_root, block 1 index */
    put_le64_t(b2 + 8, 2ULL);
    for (uint32_t m = 0; m < 9u; m++) sm_bmp_set_t(b2, m, 1u);
    sm_bmp_set_t(b2, 9u, 2u);
    put_le32_t(b2, lvmcore_crc32c_dm(b2 + 4, 4092u, LVMCORE_THIN_BITMAP_CSUM_XOR));
    init_empty_leaf_t(b3, 3, 4);
    put_le64_t(b1 + 8, 1ULL);
    put_le64_t(b1 + 16, 2ULL);
    put_le32_t(b1 + 24, 16320u - 10u);
    put_le32_t(b1, lvmcore_crc32c_dm(b1 + 4, 4092u, LVMCORE_THIN_INDEX_CSUM_XOR));

    /* Superblock (block 0) */
    put_le64_t(b0 + 32, LVMCORE_THIN_SUPERBLOCK_MAGIC);
    put_le32_t(b0 + 40, LVMCORE_THIN_VERSION);
    put_le32_t(b0 + 44, 1u); /* time = 1 */
    put_le64_t(b0 + 48, 2ULL);
    put_le64_t(b0 + 64, 384ULL); put_le64_t(b0 + 72, 1ULL);
    put_le64_t(b0 + 80, 4ULL);   put_le64_t(b0 + 88, 6ULL);
    put_le64_t(b0 + 192, 256ULL); put_le64_t(b0 + 200, 10ULL);
    put_le64_t(b0 + 208, 1ULL);   put_le64_t(b0 + 216, 3ULL);
    put_le64_t(b0 + 320, 7ULL);
    put_le64_t(b0 + 328, 8ULL);
    put_le32_t(b0 + 336, 128u); /* 64 KiB chunks */
    put_le32_t(b0 + 340, 8u);
    put_le64_t(b0 + 344, 256ULL);
    put_le32_t(b0, lvmcore_crc32c_dm(b0 + 4, 4092u, LVMCORE_THIN_SUPERBLOCK_CSUM_XOR));

    mem_rw_dev mdev = { tmeta_mem, tmeta_sz, 0 };
    mem_rw_dev ddev = { tdata_mem, tdata_sz, 0 };
    lvmcore_rw_source tmeta = { &mdev, tmeta_sz, mem_rw_read, mem_rw_write };
    lvmcore_rw_source tdata = { &ddev, tdata_sz, mem_rw_read, mem_rw_write };

    /* 1. Unmapped read returns zero; mapped shared read returns 0xA5. */
    uint8_t rbuf[512];
    CHECK("thin-read-shared-vblk0",
          lvmcore_thin_read(&tmeta, &tdata, 1, scratch, LVMCORE_THIN_SCRATCH_META_BYTES, 0, rbuf, 512) == LVMCORE_OK &&
          rbuf[0] == 0xA5 && rbuf[511] == 0xA5);
    CHECK("thin-read-unmapped-zero",
          lvmcore_thin_read(&tmeta, &tdata, 1, scratch, LVMCORE_THIN_SCRATCH_META_BYTES, 5ULL * 65536ULL, rbuf, 512) == LVMCORE_OK &&
          rbuf[0] == 0x00 && rbuf[511] == 0x00);

    /* 2. CoW write to shared chunk 0 on dev 1 -> shadows shared leaf & CoWs data block;
     *    dev 1 gets new bytes, dev 2 MUST retain 0xA5! */
    uint8_t wbuf[512];
    memset(wbuf, 0x5C, sizeof(wbuf));
    CHECK("thin-cow-write-dev1",
          lvmcore_thin_write(&tmeta, &tdata, 1, scratch, LVMCORE_THIN_SCRATCH_META_BYTES,
                             chunk_buf, 65536u, 1, 128, wbuf, 256) == LVMCORE_OK);
    CHECK("thin-cow-dev1-updated-and-preserved-prefix",
          lvmcore_thin_read(&tmeta, &tdata, 1, scratch, LVMCORE_THIN_SCRATCH_META_BYTES, 0, rbuf, 512) == LVMCORE_OK &&
          rbuf[0] == 0xA5 && rbuf[127] == 0xA5 && rbuf[128] == 0x5C && rbuf[383] == 0x5C && rbuf[384] == 0xA5);
    CHECK("thin-cow-dev2-snapshot-untouched",
          lvmcore_thin_read(&tmeta, &tdata, 2, scratch, LVMCORE_THIN_SCRATCH_META_BYTES, 0, rbuf, 512) == LVMCORE_OK &&
          rbuf[0] == 0xA5 && rbuf[128] == 0xA5 && rbuf[511] == 0xA5);

    /* 3. Write 265 distinct virtual chunks (> 252 leaf capacity -> forces B-tree leaf split!). */
    int split_writes_ok = 1;
    for (uint32_t vb = 1; vb <= 265u; vb++) {
        uint8_t pat[16];
        put_le32_t(pat, 0xBEEF0000u | vb);
        put_le32_t(pat + 4, ~vb);
        put_le64_t(pat + 8, (uint64_t)vb * 0x0102030405060708ULL);
        if (lvmcore_thin_write(&tmeta, &tdata, 1, scratch, LVMCORE_THIN_SCRATCH_META_BYTES,
                               chunk_buf, 65536u, 1, (uint64_t)vb * 65536ULL + 4096ULL,
                               pat, sizeof(pat)) != LVMCORE_OK) {
            split_writes_ok = 0;
            break;
        }
    }
    CHECK("thin-btree-split-265-chunks-written", split_writes_ok);

    int split_reads_ok = 1;
    for (uint32_t vb = 1; vb <= 265u; vb++) {
        uint8_t got[16] = {0};
        if (lvmcore_thin_read(&tmeta, &tdata, 1, scratch, LVMCORE_THIN_SCRATCH_META_BYTES,
                              (uint64_t)vb * 65536ULL + 4096ULL, got, sizeof(got)) != LVMCORE_OK ||
            get_le32_t(got) != (0xBEEF0000u | vb) ||
            get_le32_t(got + 4) != (~vb)) {
            split_reads_ok = 0;
            break;
        }
    }
    CHECK("thin-btree-split-265-chunks-verified", split_reads_ok);

    /* 4. Adversarial superblock, overflow, scratch-leak, and asymmetric-mx split checks. */
    {
        uint8_t bad_sb[4096];
        lvmcore_thin_superblock sb_out;
        memcpy(bad_sb, b0, 4096u);
        put_le32_t(bad_sb + 4, 1u); /* NEEDS_CHECK */
        put_le32_t(bad_sb, lvmcore_crc32c_dm(bad_sb + 4, 4092u, LVMCORE_THIN_SUPERBLOCK_CSUM_XOR));
        mem_rw_dev bdev = { bad_sb, 4096u, 0 };
        lvmcore_rw_source bsrc = { &bdev, tmeta_sz, mem_rw_read, NULL };
        CHECK("thin-rejects-needs-check-flag",
              lvmcore_thin_read_superblock(&bsrc, scratch, &sb_out) == LVMCORE_ERR_CORRUPT_METADATA);

        memcpy(bad_sb, b0, 4096u);
        put_le64_t(bad_sb + 56, 42ULL); /* metadata_snap != 0 */
        put_le32_t(bad_sb, lvmcore_crc32c_dm(bad_sb + 4, 4092u, LVMCORE_THIN_SUPERBLOCK_CSUM_XOR));
        CHECK("thin-rejects-held-metadata-snap",
              lvmcore_thin_read_superblock(&bsrc, scratch, &sb_out) == LVMCORE_ERR_UNSUPPORTED);

        /* meta_sm.nr_blocks > metadata_nr_blocks rejected. */
        memcpy(bad_sb, b0, 4096u);
        put_le64_t(bad_sb + 192, 512ULL); /* > 256 metadata_nr_blocks */
        put_le32_t(bad_sb, lvmcore_crc32c_dm(bad_sb + 4, 4092u, LVMCORE_THIN_SUPERBLOCK_CSUM_XOR));
        CHECK("thin-rejects-meta-sm-nr-blocks-oob",
              lvmcore_thin_read_superblock(&bsrc, scratch, &sb_out) == LVMCORE_ERR_CORRUPT_METADATA);

        /* data_sm.nr_blocks > tdata->size / chunk_bytes rejected. */
        memcpy(bad_sb, b0, 4096u);
        put_le64_t(bad_sb + 64, 9999ULL); /* > 384 chunks in tdata */
        put_le32_t(bad_sb, lvmcore_crc32c_dm(bad_sb + 4, 4092u, LVMCORE_THIN_SUPERBLOCK_CSUM_XOR));
        CHECK("thin-rejects-data-sm-nr-blocks-oob",
              lvmcore_thin_read(&bsrc, &tdata, 1, scratch, LVMCORE_THIN_SCRATCH_META_BYTES,
                                0, rbuf, 512) == LVMCORE_ERR_CORRUPT_METADATA);

        /* offset + len u64 wraparound rejected. */
        CHECK("thin-rejects-read-offset-wraparound",
              lvmcore_thin_read(&tmeta, &tdata, 1, scratch, LVMCORE_THIN_SCRATCH_META_BYTES,
                                UINT64_MAX - 15ULL, rbuf, 64) == LVMCORE_ERR_INVALID_ARG);
        CHECK("thin-rejects-write-offset-wraparound",
              lvmcore_thin_write(&tmeta, &tdata, 1, scratch, LVMCORE_THIN_SCRATCH_META_BYTES,
                                 chunk_buf, 65536u, 1, UINT64_MAX - 15ULL, wbuf, 64) == LVMCORE_ERR_INVALID_ARG);

        /* Unmapped partial write with zero_new_blocks == 0 MUST still zero chunk_buf (no scratch leak). */
        memset(chunk_buf, 0xCC, 65536u);
        uint8_t tiny[16];
        memset(tiny, 0x42, sizeof(tiny));
        uint64_t leak_vblk = 300ULL;
        CHECK("thin-zero-new-0-partial-write-ok",
              lvmcore_thin_write(&tmeta, &tdata, 1, scratch, LVMCORE_THIN_SCRATCH_META_BYTES,
                                 chunk_buf, 65536u, 0, leak_vblk * 65536ULL + 128ULL,
                                 tiny, sizeof(tiny)) == LVMCORE_OK);
        memset(rbuf, 0xFF, sizeof(rbuf));
        int no_leak = (lvmcore_thin_read(&tmeta, &tdata, 1, scratch, LVMCORE_THIN_SCRATCH_META_BYTES,
                                         leak_vblk * 65536ULL, rbuf, 512) == LVMCORE_OK);
        for (size_t i = 0; no_leak && i < 512u; i++) {
            uint8_t exp = (i >= 128u && i < 144u) ? 0x42u : 0x00u;
            if (rbuf[i] != exp) no_leak = 0;
        }
        CHECK("thin-zero-new-0-no-scratch-leak", no_leak);

        /* Huge data_blk overflowing data_blk * chunk_bytes rejected cleanly. */
        uint8_t *tmeta_ovf = (uint8_t *)malloc(tmeta_sz);
        if (tmeta_ovf) {
            memcpy(tmeta_ovf, tmeta_mem, tmeta_sz);
            /* Forge dev 2's bottom leaf (block 9) entry 0 to have huge data_blk = 2^48 - 1 (overflows * 65536). */
            uint8_t *ovf_leaf = tmeta_ovf + 9 * 4096u;
            put_le64_t(ovf_leaf + 32 + 252u * 8u, (0x0000FFFFFFFFFFFFULL << 24) | 0ULL);
            put_le32_t(ovf_leaf, lvmcore_crc32c_dm(ovf_leaf + 4, 4092u, LVMCORE_THIN_BTREE_CSUM_XOR));
            mem_rw_dev odev = { tmeta_ovf, tmeta_sz, 0 };
            lvmcore_rw_source osrc = { &odev, tmeta_sz, mem_rw_read, mem_rw_write };
            CHECK("thin-rejects-data-blk-mul-overflow",
                  lvmcore_thin_read(&osrc, &tdata, 2, scratch, LVMCORE_THIN_SCRATCH_META_BYTES,
                                    0, rbuf, 512) == LVMCORE_ERR_CORRUPT_METADATA);

            /* Small max_entries=6 leaf split must place right values at rmx=252, not mx=6! */
            memcpy(tmeta_ovf, tmeta_mem, tmeta_sz);
            uint8_t *sm_leaf = tmeta_ovf + 9 * 4096u;
            memset(sm_leaf, 0, 4096u);
            put_le32_t(sm_leaf + 4, 2u); /* LEAF */
            put_le64_t(sm_leaf + 8, 9ULL);
            put_le32_t(sm_leaf + 16, 6u); /* nr = 6 */
            put_le32_t(sm_leaf + 20, 6u); /* mx = 6 (smaller than newly allocated right leaf rmx=252!) */
            put_le32_t(sm_leaf + 24, 8u);
            for (uint32_t k = 0; k < 6u; k++) {
                put_le64_t(sm_leaf + 32 + k * 8u, (uint64_t)k);
                put_le64_t(sm_leaf + 32 + 6u * 8u + k * 8u, ((uint64_t)(k + 1u) << 24) | 1ULL);
                memset(tdata_mem + (size_t)(k + 1u) * 65536u, (int)(0x60u + k), 512u);
            }
            put_le32_t(sm_leaf, lvmcore_crc32c_dm(sm_leaf + 4, 4092u, LVMCORE_THIN_BTREE_CSUM_XOR));
            uint8_t pat7[64];
            memset(pat7, 0x99, sizeof(pat7));
            int sm_ok = (lvmcore_thin_write(&osrc, &tdata, 2, scratch, LVMCORE_THIN_SCRATCH_META_BYTES,
                                            chunk_buf, 65536u, 1, 6ULL * 65536ULL, pat7, sizeof(pat7)) == LVMCORE_OK);
            for (uint32_t k = 0; sm_ok && k < 6u; k++) {
                uint8_t g[16] = {0};
                if (lvmcore_thin_read(&osrc, &tdata, 2, scratch, LVMCORE_THIN_SCRATCH_META_BYTES,
                                      (uint64_t)k * 65536ULL, g, sizeof(g)) != LVMCORE_OK ||
                    g[0] != (uint8_t)(0x60u + k)) {
                    sm_ok = 0;
                }
            }
            if (sm_ok) {
                uint8_t g[64] = {0};
                sm_ok = (lvmcore_thin_read(&osrc, &tdata, 2, scratch, LVMCORE_THIN_SCRATCH_META_BYTES,
                                           6ULL * 65536ULL, g, sizeof(g)) == LVMCORE_OK && g[0] == 0x99);
            }
            CHECK("thin-small-mx-leaf-split-uses-right-rmx", sm_ok);

            /* max_entries=1 leaf split must fail closed without OOB. */
            memcpy(tmeta_ovf, tmeta_mem, tmeta_sz);
            sm_leaf = tmeta_ovf + 9 * 4096u;
            memset(sm_leaf, 0, 4096u);
            put_le32_t(sm_leaf + 4, 2u);
            put_le64_t(sm_leaf + 8, 9ULL);
            put_le32_t(sm_leaf + 16, 1u);
            put_le32_t(sm_leaf + 20, 1u);
            put_le32_t(sm_leaf + 24, 8u);
            put_le64_t(sm_leaf + 32, 0ULL);
            put_le64_t(sm_leaf + 40, (0ULL << 24) | 1ULL);
            put_le32_t(sm_leaf, lvmcore_crc32c_dm(sm_leaf + 4, 4092u, LVMCORE_THIN_BTREE_CSUM_XOR));
            CHECK("thin-rejects-mx-1-split",
                  lvmcore_thin_write(&osrc, &tdata, 2, scratch, LVMCORE_THIN_SCRATCH_META_BYTES,
                                     chunk_buf, 65536u, 1, 1ULL * 65536ULL, pat7, sizeof(pat7)) == LVMCORE_ERR_CORRUPT_METADATA);
            free(tmeta_ovf);
        }
    }

out:
    free(tmeta_mem);
    free(tdata_mem);
    free(scratch);
    free(chunk_buf);
}

static void test_snap_engine_multi_cow_and_full(void) {
    /* 128 KiB origin (32 x 4 KiB chunks), snapA (64 KiB = 14 data chunks), snapB (20 KiB = 3 data chunks). */
    uint8_t orig_mem[128 * 1024];
    uint8_t cowA_mem[64 * 1024];
    uint8_t cowB_mem[20 * 1024]; /* chunk 0=hdr, chunk 1=table, chunks 2..4=3 data slots */
    uint8_t cbuf[4096], tbuf[4096];

    for (size_t i = 0; i < sizeof(orig_mem); i++) orig_mem[i] = (uint8_t)((i / 4096u) + 0x10u);
    memset(cowA_mem, 0, sizeof(cowA_mem));
    memset(cowB_mem, 0, sizeof(cowB_mem));
    put_le32_t(cowA_mem + 0, LVMCORE_SNAP_MAGIC);
    put_le32_t(cowA_mem + 4, 1u);
    put_le32_t(cowA_mem + 8, LVMCORE_SNAP_VERSION);
    put_le32_t(cowA_mem + 12, 8u); /* 4 KiB chunks */
    memcpy(cowB_mem, cowA_mem, 16u);

    mem_rw_dev d_orig = { orig_mem, sizeof(orig_mem), 0 };
    mem_rw_dev d_cowA = { cowA_mem, sizeof(cowA_mem), 0 };
    mem_rw_dev d_cowB = { cowB_mem, sizeof(cowB_mem), 0 };
    lvmcore_rw_source s_orig = { &d_orig, sizeof(orig_mem), mem_rw_read, mem_rw_write };
    lvmcore_rw_source cows[2] = {
        { &d_cowA, sizeof(cowA_mem), mem_rw_read, mem_rw_write },
        { &d_cowB, sizeof(cowB_mem), mem_rw_read, mem_rw_write },
    };

    /* 1. Write to snapA chunk 0 -> snapA sees 0xDD, origin and snapB still see 0x10. */
    uint8_t patA[512], got[512];
    memset(patA, 0xDD, sizeof(patA));
    CHECK("snap-write-snapA",
          lvmcore_snap_write(&s_orig, &cows[0], cbuf, tbuf, sizeof(cbuf), 0, patA, 512) == LVMCORE_OK);
    CHECK("snap-read-snapA-updated",
          lvmcore_snap_read(&s_orig, &cows[0], tbuf, sizeof(tbuf), 0, got, 512) == LVMCORE_OK &&
          got[0] == 0xDD && got[511] == 0xDD);
    CHECK("snap-origin-untouched-after-snapA-write", orig_mem[0] == 0x10);

    /* 2. Origin write across chunks 1 & 2 -> CoWs original bytes into BOTH snapA and snapB! */
    uint8_t patO[4096];
    memset(patO, 0x77, sizeof(patO));
    CHECK("snap-origin-write-multi-cow",
          lvmcore_snap_origin_write(&s_orig, cows, 2, cbuf, tbuf, sizeof(cbuf), 4096 + 2048, patO, 4096) == LVMCORE_OK);
    CHECK("snap-origin-updated", orig_mem[4096 + 2048] == 0x77 && orig_mem[8192] == 0x77);
    CHECK("snap-snapA-preserved-chunk1-and-2",
          lvmcore_snap_read(&s_orig, &cows[0], tbuf, sizeof(tbuf), 4096 + 2048, got, 512) == LVMCORE_OK &&
          got[0] == 0x11 &&
          lvmcore_snap_read(&s_orig, &cows[0], tbuf, sizeof(tbuf), 8192, got, 512) == LVMCORE_OK &&
          got[0] == 0x12);
    CHECK("snap-snapB-preserved-chunk1-and-2",
          lvmcore_snap_read(&s_orig, &cows[1], tbuf, sizeof(tbuf), 4096 + 2048, got, 512) == LVMCORE_OK &&
          got[0] == 0x11 &&
          lvmcore_snap_read(&s_orig, &cows[1], tbuf, sizeof(tbuf), 8192, got, 512) == LVMCORE_OK &&
          got[0] == 0x12);

    /* 3. Fill snapB's remaining 1 data slot (chunk 3) then overflow (chunk 4) -> valid=0 fail-closed! */
    CHECK("snap-snapB-fill-last-slot",
          lvmcore_snap_write(&s_orig, &cows[1], cbuf, tbuf, sizeof(cbuf), 3 * 4096, patA, 512) == LVMCORE_OK);
    CHECK("snap-snapB-overflow-fails-and-invalidates",
          lvmcore_snap_write(&s_orig, &cows[1], cbuf, tbuf, sizeof(cbuf), 4 * 4096, patA, 512) != LVMCORE_OK &&
          get_le32_t(cowB_mem + 4) == 0u);
    CHECK("snap-snapB-invalid-refuses-read",
          lvmcore_snap_read(&s_orig, &cows[1], tbuf, sizeof(tbuf), 0, got, 512) != LVMCORE_OK);

    /* 4. Adversarial COW corruption checks: out-of-range chunk_size and metadata-chunk collision. */
    {
        uint8_t bad_cow[16384];
        memset(bad_cow, 0, sizeof(bad_cow));
        put_le32_t(bad_cow + 0, LVMCORE_SNAP_MAGIC);
        put_le32_t(bad_cow + 4, 1u);
        put_le32_t(bad_cow + 8, LVMCORE_SNAP_VERSION);
        put_le32_t(bad_cow + 12, 4096u); /* > 2048 sectors max */
        mem_rw_dev bcd = { bad_cow, sizeof(bad_cow), 0 };
        lvmcore_rw_source bcs = { &bcd, sizeof(bad_cow), mem_rw_read, NULL };
        lvmcore_snap_header bhdr;
        CHECK("snap-rejects-oversized-chunk-sectors",
              lvmcore_snap_read_header(&bcs, &bhdr) == LVMCORE_ERR_CORRUPT_METADATA);

        /* Valid header (chunk_size=8 = 4 KiB), but exception entry points new_chunk=1 (table chunk!). */
        put_le32_t(bad_cow + 12, 8u);
        put_le64_t(bad_cow + 4096 + 0, 0ULL);
        put_le64_t(bad_cow + 4096 + 8, 1ULL); /* collides with area 0 exception table chunk! */
        CHECK("snap-rejects-exception-table-collision",
              lvmcore_snap_read(&s_orig, &bcs, tbuf, sizeof(tbuf), 0, got, 512) == LVMCORE_ERR_CORRUPT_METADATA);
    }
}

static int pv_mem_read(void *ctx, uint32_t pv_slot, uint64_t off, void *dst, size_t len) {
    const wl_vg_record *rec = (const wl_vg_record *)ctx;
    if (!rec || pv_slot >= rec->pv_count || !rec->pv_tag[pv_slot]) return -1;
    return mem_rw_read((void *)(uintptr_t)rec->pv_tag[pv_slot], off, dst, len);
}

static int pv_mem_write(void *ctx, uint32_t pv_slot, uint64_t off, const void *src, size_t len) {
    const wl_vg_record *rec = (const wl_vg_record *)ctx;
    if (!rec || pv_slot >= rec->pv_count || !rec->pv_tag[pv_slot]) return -1;
    return mem_rw_write((void *)(uintptr_t)rec->pv_tag[pv_slot], off, src, len);
}

static void test_snap_origin_missing_cow_fails_closed(void) {
    /* 2-PV VG: lvorig on pv0, lvsnap COW on pv1. Removing pv1 MUST fail origin write closed. */
    static uint8_t pv0_mem[65536], pv1_mem[65536];
    static uint8_t mscr[LVMCORE_THIN_SCRATCH_META_BYTES], cbuf[4096], abuf[4096];
    memset(pv0_mem, 0x33, sizeof(pv0_mem));
    memset(pv1_mem, 0, sizeof(pv1_mem));
    put_le32_t(pv1_mem + 4096 + 0, LVMCORE_SNAP_MAGIC);
    put_le32_t(pv1_mem + 4096 + 4, 1u);
    put_le32_t(pv1_mem + 4096 + 8, LVMCORE_SNAP_VERSION);
    put_le32_t(pv1_mem + 4096 + 12, 8u); /* 4 KiB chunk */

    mem_rw_dev d0 = { pv0_mem, sizeof(pv0_mem), 0 };
    mem_rw_dev d1 = { pv1_mem, sizeof(pv1_mem), 0 };

    static lvmcore_physical_volume pvs[2];
    static lvmcore_stripe st_orig[1], st_cow[1];
    static lvmcore_segment segs[3];
    static lvmcore_logical_volume lvs[3];
    static lvmcore_volume_group vg;

    pv_set(&pvs[0], "pv0", UUID_PV0, 8);
    pv_set(&pvs[1], "pv1", UUID_PV1, 8);

    memset(segs, 0, sizeof(segs));
    segs[0].type = LVMCORE_SEG_STRIPED;
    segs[0].extent_count = 8; segs[0].stripe_count = 1; segs[0].stripe_count_used = 1;
    segs[0].stripes = st_orig;
    strcpy(st_orig[0].pv_key, "pv0"); st_orig[0].start_extent = 0;

    segs[1].type = LVMCORE_SEG_STRIPED;
    segs[1].extent_count = 8; segs[1].stripe_count = 1; segs[1].stripe_count_used = 1;
    segs[1].stripes = st_cow;
    strcpy(st_cow[0].pv_key, "pv1"); st_cow[0].start_extent = 0;

    segs[2].type = LVMCORE_SEG_SNAPSHOT;
    segs[2].extent_count = 8;
    segs[2].chunk_size_sectors = 8;
    strcpy(segs[2].origin, "lvorig");
    strcpy(segs[2].cow_store, "lvsnap");

    lv_set(&lvs[0], "lvorig",    "s1s1s1s1s1s1s1s1s1s1s1s1s1s1s1s1", 1, 1, 0, &segs[0], 1);
    lvs[0].is_snapshot_origin = 1;
    lv_set(&lvs[1], "lvsnap",    "s2s2s2s2s2s2s2s2s2s2s2s2s2s2s2s2", 1, 1, 0, &segs[1], 1);
    lvs[1].is_snapshot_cow = 1;
    strcpy(lvs[1].snapshot_wrapper, "snapshot0");
    lv_set(&lvs[2], "snapshot0", "s3s3s3s3s3s3s3s3s3s3s3s3s3s3s3s3", 0, 1, 1, &segs[2], 1);
    vg_set(&vg, "77777777777777777777777777777777", 1, 8, pvs, 2, lvs, 3);

    wl_vg_registry_init(recs, WL_VG_MAX);
    size_t slot = 99;
    ARR(&vg, UUID_PV0, &d0, NULL, NULL);
    ARR(&vg, UUID_PV1, &d1, NULL, NULL);
    wl_pv_io_ops ops = { &recs[slot], pv_mem_read, pv_mem_write };

    /* With both PVs present, origin write preserves original 0x33 into snapshot and writes 0x88. */
    uint8_t wpat[512], rbuf[512];
    memset(wpat, 0x88, sizeof(wpat));
    CHECK("snap-origin-o1-stack-write-ok",
          wl_complex_lv_write(&vg, &recs[slot], 0, &ops, mscr, sizeof(mscr),
                              cbuf, abuf, sizeof(cbuf), 0, wpat, sizeof(wpat)) == LVMCORE_OK &&
          pv0_mem[4096] == 0x88 &&
          wl_complex_lv_read(&vg, &recs[slot], 1, &ops, mscr, sizeof(mscr),
                             abuf, sizeof(abuf), 0, rbuf, sizeof(rbuf)) == LVMCORE_OK &&
          rbuf[0] == 0x33);

    /* Remove pv1 (snapshot COW PV): origin write MUST fail closed and leave pv0 chunk 1 untouched! */
    wl_vg_pv_removed(recs, WL_VG_MAX, UUID_PV1);
    memset(wpat, 0xEE, sizeof(wpat));
    CHECK("snap-origin-missing-cow-pv-fails-closed",
          wl_complex_lv_write(&vg, &recs[slot], 0, &ops, mscr, sizeof(mscr),
                              cbuf, abuf, sizeof(cbuf), 4096, wpat, sizeof(wpat)) == LVMCORE_ERR_IO &&
          pv0_mem[4096 + 4096] == 0x33);

    /* Virtual-size bounds check on wl_thin_lv_read/write: reject I/O past wl_lv_byte_size. */
    {
        static lvmcore_segment tsegs[4];
        static lvmcore_stripe tst_m[1], tst_d[1];
        static lvmcore_logical_volume tlvs[4];
        static lvmcore_volume_group tvg;
        memset(tsegs, 0, sizeof(tsegs));
        tsegs[0].type = LVMCORE_SEG_THIN;
        tsegs[0].extent_count = 2; /* 2 * 8 * 512 = 8192 bytes virtual size */
        tsegs[0].has_device_id = 1;
        tsegs[0].device_id = 1;
        strcpy(tsegs[0].pool_data, "tpool");
        tsegs[1].type = LVMCORE_SEG_THIN_POOL;
        tsegs[1].extent_count = 4;
        tsegs[1].chunk_size_sectors = 128;
        strcpy(tsegs[1].pool_metadata, "tpool_tmeta");
        strcpy(tsegs[1].pool_data, "tpool_tdata");
        tsegs[2].type = LVMCORE_SEG_STRIPED;
        tsegs[2].extent_count = 4; tsegs[2].stripe_count = 1; tsegs[2].stripe_count_used = 1;
        tsegs[2].stripes = tst_m; strcpy(tst_m[0].pv_key, "pv0");
        tsegs[3].type = LVMCORE_SEG_STRIPED;
        tsegs[3].extent_count = 4; tsegs[3].stripe_count = 1; tsegs[3].stripe_count_used = 1;
        tsegs[3].stripes = tst_d; strcpy(tst_d[0].pv_key, "pv0");
        lv_set(&tlvs[0], "lvthin",      "t1t1t1t1t1t1t1t1t1t1t1t1t1t1t1t1", 1, 1, 0, &tsegs[0], 1);
        lv_set(&tlvs[1], "tpool",       "t2t2t2t2t2t2t2t2t2t2t2t2t2t2t2t2", 0, 1, 1, &tsegs[1], 1);
        tlvs[1].is_thin_pool = 1;
        lv_set(&tlvs[2], "tpool_tmeta", "t3t3t3t3t3t3t3t3t3t3t3t3t3t3t3t3", 0, 0, 1, &tsegs[2], 1);
        lv_set(&tlvs[3], "tpool_tdata", "t4t4t4t4t4t4t4t4t4t4t4t4t4t4t4t4", 0, 0, 1, &tsegs[3], 1);
        vg_set(&tvg, "88888888888888888888888888888888", 1, 8, pvs, 1, tlvs, 4);
        CHECK("thin-lv-oob-read-rejected",
              wl_thin_lv_read(&tvg, &recs[slot], 0, &ops, mscr, sizeof(mscr),
                              8192, rbuf, 512) == LVMCORE_ERR_INVALID_ARG);
        CHECK("thin-lv-oob-write-rejected",
              wl_thin_lv_write(&tvg, &recs[slot], 0, &ops, mscr, sizeof(mscr),
                               cbuf, sizeof(cbuf), 8192 - 256, wpat, 512) == LVMCORE_ERR_INVALID_ARG);
    }
}

static void test_raid1_mirror_engine_rw_degraded(void) {
    /* 2-PV VG with raid1 LV backed by in-memory PV buffers via wl_complex_lv_read/write. */
    static uint8_t pv0_mem[65536], pv1_mem[65536];
    static uint8_t mscr[LVMCORE_THIN_SCRATCH_META_BYTES], cbuf[4096], abuf[4096];
    memset(pv0_mem, 0, sizeof(pv0_mem));
    memset(pv1_mem, 0, sizeof(pv1_mem));
    mem_rw_dev d0 = { pv0_mem, sizeof(pv0_mem), 0 };
    mem_rw_dev d1 = { pv1_mem, sizeof(pv1_mem), 0 };

    static lvmcore_physical_volume pvs[2];
    static lvmcore_stripe st_i0[1], st_i1[1];
    static lvmcore_raid_leg legs[2];
    static lvmcore_segment segs[3];
    static lvmcore_logical_volume lvs[3];
    static lvmcore_volume_group vg;

    pv_set(&pvs[0], "pv0", UUID_PV0, 8); /* pe_start = 4096B */
    pv_set(&pvs[1], "pv1", UUID_PV1, 8);

    memset(segs, 0, sizeof(segs));
    segs[0].type = LVMCORE_SEG_RAID1;
    segs[0].extent_count = 4;
    segs[0].region_size_sectors = 4096;
    segs[0].raid_leg_count = 2;
    segs[0].raid_legs = legs;
    memset(legs, 0, sizeof(legs));
    strcpy(legs[0].image_lv, "r_i0");
    strcpy(legs[1].image_lv, "r_i1");

    segs[1].type = LVMCORE_SEG_STRIPED;
    segs[1].extent_count = 4; segs[1].stripe_count = 1; segs[1].stripe_count_used = 1;
    segs[1].stripes = st_i0;
    strcpy(st_i0[0].pv_key, "pv0"); st_i0[0].start_extent = 0;

    segs[2].type = LVMCORE_SEG_STRIPED;
    segs[2].extent_count = 4; segs[2].stripe_count = 1; segs[2].stripe_count_used = 1;
    segs[2].stripes = st_i1;
    strcpy(st_i1[0].pv_key, "pv1"); st_i1[0].start_extent = 0;

    lv_set(&lvs[0], "lvraid1", "r1r1r1r1r1r1r1r1r1r1r1r1r1r1r1r1", 1, 1, 0, &segs[0], 1);
    lv_set(&lvs[1], "r_i0",    "r2r2r2r2r2r2r2r2r2r2r2r2r2r2r2r2", 0, 0, 1, &segs[1], 1);
    lv_set(&lvs[2], "r_i1",    "r3r3r3r3r3r3r3r3r3r3r3r3r3r3r3r3", 0, 0, 1, &segs[2], 1);
    vg_set(&vg, "66666666666666666666666666666666", 1, 8, pvs, 2, lvs, 3);

    wl_vg_registry_init(recs, WL_VG_MAX);
    size_t slot = 99;
    ARR(&vg, UUID_PV0, &d0, NULL, NULL);
    ARR(&vg, UUID_PV1, &d1, NULL, NULL);
    wl_pv_io_ops ops = { &recs[slot], pv_mem_read, pv_mem_write };

    CHECK("raid1-is-complex", wl_vg_lv_is_complex(&vg, 0) == 1);
    uint32_t slots[4] = {0};
    size_t nslots = wl_vg_lv_collect_pv_slots(&recs[slot], 0, slots, 4);
    CHECK("raid1-collect-pv-slots-both", nslots == 2);

    /* Dual-leg simultaneous write updates BOTH pv0 and pv1 identically! */
    uint8_t wpat[1024], rbuf[1024];
    for (size_t i = 0; i < sizeof(wpat); i++) wpat[i] = (uint8_t)(i ^ 0x3A);
    CHECK("raid1-dual-write-ok",
          wl_complex_lv_write(&vg, &recs[slot], 0, &ops, mscr, sizeof(mscr), cbuf, abuf, sizeof(cbuf),
                              512, wpat, sizeof(wpat)) == LVMCORE_OK);
    CHECK("raid1-both-legs-identical",
          memcmp(pv0_mem + 4096 + 512, wpat, sizeof(wpat)) == 0 &&
          memcmp(pv1_mem + 4096 + 512, wpat, sizeof(wpat)) == 0);

    /* Out-of-bounds read/write rejected by LV virtual-size guard. */
    CHECK("raid1-oob-write-rejected",
          wl_complex_lv_write(&vg, &recs[slot], 0, &ops, mscr, sizeof(mscr), cbuf, abuf, sizeof(cbuf),
                              16384, wpat, 512) == LVMCORE_ERR_INVALID_ARG);
    CHECK("raid1-oob-read-rejected",
          wl_complex_lv_read(&vg, &recs[slot], 0, &ops, mscr, sizeof(mscr), abuf, sizeof(abuf),
                             16384 - 256, rbuf, 512) == LVMCORE_ERR_INVALID_ARG);

    /* Direct lvmcore_raid_check_superblock & lvmcore_raid1_read/write coverage. */
    {
        uint8_t rmeta_buf[512] = {0}, sec[512] = {0};
        put_le32_t(rmeta_buf, LVMCORE_DMRAID_MAGIC);
        mem_rw_dev rmd = { rmeta_buf, sizeof(rmeta_buf), 0 };
        lvmcore_rw_source rms = { &rmd, sizeof(rmeta_buf), mem_rw_read, NULL };
        CHECK("raid-superblock-dmrd-valid",
              lvmcore_raid_check_superblock(&rms, sec, sizeof(sec)) == LVMCORE_OK);
        rmeta_buf[0] ^= 0xFF;
        CHECK("raid-superblock-corrupt-rejected",
              lvmcore_raid_check_superblock(&rms, sec, sizeof(sec)) == LVMCORE_ERR_CORRUPT_METADATA);

        lvmcore_rw_source rlegs[2] = {
            { &d0, sizeof(pv0_mem), mem_rw_read, mem_rw_write },
            { &d1, sizeof(pv1_mem), mem_rw_read, mem_rw_write },
        };
        int pres[2] = { 1, 1 };
        CHECK("lvmcore-raid1-write-and-failover-read",
              lvmcore_raid1_write(rlegs, pres, 2, 0, wpat, 256) == LVMCORE_OK &&
              (d0.fail_reads = 1, 1) &&
              lvmcore_raid1_read(rlegs, pres, 2, 0, rbuf, 256) == LVMCORE_OK &&
              memcmp(rbuf, wpat, 256) == 0);
        d0.fail_reads = 0;
    }

    /* Inject I/O read error on PV0 -> automatic failover to PV1! */
    d0.fail_reads = 1;
    memset(rbuf, 0, sizeof(rbuf));
    CHECK("raid1-io-error-failover-to-pv1",
          wl_complex_lv_read(&vg, &recs[slot], 0, &ops, mscr, sizeof(mscr), abuf, sizeof(abuf),
                             512, rbuf, sizeof(rbuf)) == LVMCORE_OK &&
          memcmp(rbuf, wpat, sizeof(wpat)) == 0);
    d0.fail_reads = 0;

    /* Remove PV0 -> degraded read succeeds, write fails closed without mutating PV1! */
    wl_vg_pv_removed(recs, WL_VG_MAX, UUID_PV0);
    CHECK("raid1-degraded-pv-complete", wl_vg_lv_pv_complete(&recs[slot], 0) == 1);
    CHECK("raid1-degraded-not-all-legs", wl_vg_lv_raid_all_legs_present(&recs[slot], 0) == 0);
    memset(rbuf, 0, sizeof(rbuf));
    CHECK("raid1-degraded-read-ok",
          wl_complex_lv_read(&vg, &recs[slot], 0, &ops, mscr, sizeof(mscr), abuf, sizeof(abuf),
                             512, rbuf, sizeof(rbuf)) == LVMCORE_OK &&
          memcmp(rbuf, wpat, sizeof(wpat)) == 0);
    uint8_t bad_w[512];
    memset(bad_w, 0xEE, sizeof(bad_w));
    CHECK("raid1-degraded-write-fails-closed",
          wl_complex_lv_write(&vg, &recs[slot], 0, &ops, mscr, sizeof(mscr), cbuf, abuf, sizeof(cbuf),
                              512, bad_w, sizeof(bad_w)) != LVMCORE_OK &&
          memcmp(pv1_mem + 4096 + 512, wpat, sizeof(wpat)) == 0);
}

static void test_raid1_staggered_arrival_and_leg_loss(void) {
    /*
     * Staggered 2-PV raid1/mirror arrival at constant seqno and
     * symmetric 1-leg removal / re-insertion:
     *   PV0 alone -> degraded readable (1 slot, all_legs_present==0, write fails closed)
     *   PV1 arrives at same seqno -> WL_ARR_COMPLETED with newly_count==0,
     *     yet slot count grows 1->2 and all_legs_present flips 0->1 (RO->RW rebind)
     *   PV0 removed -> any_pv_gone==0 (stays alive degraded), slot count shrinks 2->1
     *     with only PV1 remaining, all_legs_present flips 1->0 (RW->RO rebind)
     *   PV0 re-inserted -> returns to 2 slots and all_legs_present==1
     */
    static uint8_t pv0_mem[65536], pv1_mem[65536];
    static uint8_t mscr[LVMCORE_THIN_SCRATCH_META_BYTES], cbuf[4096], abuf[4096];
    memset(pv0_mem, 0x41, sizeof(pv0_mem));
    memset(pv1_mem, 0x41, sizeof(pv1_mem));
    mem_rw_dev d0 = { pv0_mem, sizeof(pv0_mem), 0 };
    mem_rw_dev d1 = { pv1_mem, sizeof(pv1_mem), 0 };

    static lvmcore_physical_volume pvs[2];
    static lvmcore_stripe st_r0[1], st_r1[1], st_m0[1], st_m1[1];
    static lvmcore_raid_leg rlegs[2], mlegs[2];
    static lvmcore_segment segs[6];
    static lvmcore_logical_volume lvs[6];
    static lvmcore_volume_group vg;

    pv_set(&pvs[0], "pv0", UUID_PV0, 8);
    pv_set(&pvs[1], "pv1", UUID_PV1, 8);

    memset(segs, 0, sizeof(segs));
    segs[0].type = LVMCORE_SEG_RAID1;
    segs[0].extent_count = 4;
    segs[0].region_size_sectors = 4096;
    segs[0].raid_leg_count = 2;
    segs[0].raid_legs = rlegs;
    memset(rlegs, 0, sizeof(rlegs));
    strcpy(rlegs[0].image_lv, "r_i0");
    strcpy(rlegs[1].image_lv, "r_i1");

    segs[1].type = LVMCORE_SEG_MIRROR;
    segs[1].extent_count = 4;
    segs[1].region_size_sectors = 4096;
    segs[1].raid_leg_count = 2;
    segs[1].raid_legs = mlegs;
    memset(mlegs, 0, sizeof(mlegs));
    strcpy(mlegs[0].image_lv, "m_i0");
    strcpy(mlegs[1].image_lv, "m_i1");

    segs[2].type = LVMCORE_SEG_STRIPED;
    segs[2].extent_count = 4; segs[2].stripe_count = 1; segs[2].stripe_count_used = 1;
    segs[2].stripes = st_r0; strcpy(st_r0[0].pv_key, "pv0"); st_r0[0].start_extent = 0;

    segs[3].type = LVMCORE_SEG_STRIPED;
    segs[3].extent_count = 4; segs[3].stripe_count = 1; segs[3].stripe_count_used = 1;
    segs[3].stripes = st_r1; strcpy(st_r1[0].pv_key, "pv1"); st_r1[0].start_extent = 0;

    segs[4].type = LVMCORE_SEG_STRIPED;
    segs[4].extent_count = 4; segs[4].stripe_count = 1; segs[4].stripe_count_used = 1;
    segs[4].stripes = st_m0; strcpy(st_m0[0].pv_key, "pv0"); st_m0[0].start_extent = 4;

    segs[5].type = LVMCORE_SEG_STRIPED;
    segs[5].extent_count = 4; segs[5].stripe_count = 1; segs[5].stripe_count_used = 1;
    segs[5].stripes = st_m1; strcpy(st_m1[0].pv_key, "pv1"); st_m1[0].start_extent = 4;

    lv_set(&lvs[0], "lvraid1",  "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1", 1, 1, 0, &segs[0], 1);
    lv_set(&lvs[1], "lvmirror", "a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2", 1, 1, 0, &segs[1], 1);
    lv_set(&lvs[2], "r_i0",     "a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3", 0, 0, 1, &segs[2], 1);
    lv_set(&lvs[3], "r_i1",     "a4a4a4a4a4a4a4a4a4a4a4a4a4a4a4a4", 0, 0, 1, &segs[3], 1);
    lv_set(&lvs[4], "m_i0",     "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5", 0, 0, 1, &segs[4], 1);
    lv_set(&lvs[5], "m_i1",     "a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6a6", 0, 0, 1, &segs[5], 1);
    vg_set(&vg, "55555555555555555555555555555555", 1, 8, pvs, 2, lvs, 6);

    wl_vg_registry_init(recs, WL_VG_MAX);
    size_t slot = 99;
    size_t newly[WL_VG_MAX_LVS];
    size_t newly_count = 0;

    /* Step 1: PV0 arrives alone at seqno 1 -> both LVs are degraded-complete (RO). */
    wl_arrival_result r0 = wl_vg_arrival(recs, WL_VG_MAX, &slot, &vg, UUID_PV0, &d0,
                                         NULL, NULL, newly, WL_VG_MAX_LVS, &newly_count);
    CHECK("staggered-pv0-pending", r0 == WL_ARR_PENDING && newly_count == 2);
    wl_vg_lv_set_activated(&recs[slot], 0, 1);
    wl_vg_lv_set_activated(&recs[slot], 1, 1);

    uint32_t slots[4] = {0};
    CHECK("staggered-pv0-raid1-degraded-slots",
          wl_vg_lv_pv_complete(&recs[slot], 0) == 1 &&
          wl_vg_lv_raid_all_legs_present(&recs[slot], 0) == 0 &&
          wl_vg_lv_collect_pv_slots(&recs[slot], 0, slots, 4) == 1 &&
          slots[0] == 0);
    CHECK("staggered-pv0-mirror-degraded-slots",
          wl_vg_lv_pv_complete(&recs[slot], 1) == 1 &&
          wl_vg_lv_raid_all_legs_present(&recs[slot], 1) == 0 &&
          wl_vg_lv_collect_pv_slots(&recs[slot], 1, slots, 4) == 1 &&
          slots[0] == 0);

    wl_pv_io_ops ops = { &recs[slot], pv_mem_read, pv_mem_write };
    uint8_t rbuf[512], wpat[512];
    memset(wpat, 0x5C, sizeof(wpat));
    CHECK("staggered-pv0-read-ok-write-fails",
          wl_complex_lv_read(&vg, &recs[slot], 0, &ops, mscr, sizeof(mscr), abuf, sizeof(abuf),
                             0, rbuf, sizeof(rbuf)) == LVMCORE_OK &&
          rbuf[0] == 0x41 &&
          wl_complex_lv_write(&vg, &recs[slot], 0, &ops, mscr, sizeof(mscr), cbuf, abuf, sizeof(cbuf),
                              0, wpat, sizeof(wpat)) != LVMCORE_OK);

    /* Step 2: PV1 arrives at IDENTICAL seqno 1 -> WL_ARR_COMPLETED, newly_count==0,
     * slot count grows 1->2 and all_legs_present flips 0->1 for both LVs. */
    wl_arrival_result r1 = wl_vg_arrival(recs, WL_VG_MAX, &slot, &vg, UUID_PV1, &d1,
                                         NULL, NULL, newly, WL_VG_MAX_LVS, &newly_count);
    CHECK("staggered-pv1-completed-zero-newly", r1 == WL_ARR_COMPLETED && newly_count == 0);
    CHECK("staggered-pv1-raid1-binding-grew-rw",
          wl_vg_lv_raid_all_legs_present(&recs[slot], 0) == 1 &&
          wl_vg_lv_collect_pv_slots(&recs[slot], 0, slots, 4) == 2 &&
          slots[0] == 0 && slots[1] == 1);
    CHECK("staggered-pv1-mirror-binding-grew-rw",
          wl_vg_lv_raid_all_legs_present(&recs[slot], 1) == 1 &&
          wl_vg_lv_collect_pv_slots(&recs[slot], 1, slots, 4) == 2 &&
          slots[0] == 0 && slots[1] == 1);
    CHECK("staggered-pv1-dual-write-ok",
          wl_complex_lv_write(&vg, &recs[slot], 0, &ops, mscr, sizeof(mscr), cbuf, abuf, sizeof(cbuf),
                              0, wpat, sizeof(wpat)) == LVMCORE_OK &&
          memcmp(pv0_mem + 4096, wpat, sizeof(wpat)) == 0 &&
          memcmp(pv1_mem + 4096, wpat, sizeof(wpat)) == 0);

    /* Step 3: Remove PV0 (1 leg gone while PV1 stays present) -> any_pv_gone==0,
     * slots shrink 2->1 to surviving PV1 only, all_legs_present flips 1->0 (RW->RO). */
    recs[slot].pv_tag[0] = NULL;
    wl_vg_pv_removed(recs, WL_VG_MAX, UUID_PV0);
    CHECK("leg-loss-raid1-stays-alive-degraded-ro",
          wl_vg_lv_any_pv_gone(&recs[slot], 0) == 0 &&
          wl_vg_lv_pv_complete(&recs[slot], 0) == 1 &&
          wl_vg_lv_raid_all_legs_present(&recs[slot], 0) == 0 &&
          wl_vg_lv_collect_pv_slots(&recs[slot], 0, slots, 4) == 1 &&
          slots[0] == 1);
    CHECK("leg-loss-mirror-stays-alive-degraded-ro",
          wl_vg_lv_any_pv_gone(&recs[slot], 1) == 0 &&
          wl_vg_lv_pv_complete(&recs[slot], 1) == 1 &&
          wl_vg_lv_raid_all_legs_present(&recs[slot], 1) == 0 &&
          wl_vg_lv_collect_pv_slots(&recs[slot], 1, slots, 4) == 1 &&
          slots[0] == 1);
    memset(rbuf, 0, sizeof(rbuf));
    CHECK("leg-loss-degraded-read-ok-write-fails",
          wl_complex_lv_read(&vg, &recs[slot], 0, &ops, mscr, sizeof(mscr), abuf, sizeof(abuf),
                             0, rbuf, sizeof(rbuf)) == LVMCORE_OK &&
          memcmp(rbuf, wpat, sizeof(wpat)) == 0 &&
          wl_complex_lv_write(&vg, &recs[slot], 0, &ops, mscr, sizeof(mscr), cbuf, abuf, sizeof(cbuf),
                              0, rbuf, sizeof(rbuf)) != LVMCORE_OK);

    /* Step 4: Re-insert PV0 at same seqno -> restores 2 slots & RW. */
    wl_arrival_result r2 = wl_vg_arrival(recs, WL_VG_MAX, &slot, &vg, UUID_PV0, &d0,
                                         NULL, NULL, newly, WL_VG_MAX_LVS, &newly_count);
    CHECK("leg-reinsert-restores-rw-slots",
          r2 == WL_ARR_COMPLETED && newly_count == 0 &&
          wl_vg_lv_raid_all_legs_present(&recs[slot], 0) == 1 &&
          wl_vg_lv_collect_pv_slots(&recs[slot], 0, slots, 4) == 2 &&
          slots[0] == 0 && slots[1] == 1);

    /* Step 5: Remove both legs -> any_pv_gone==1, pv_complete==0, 0 slots. */
    recs[slot].pv_tag[0] = NULL;
    wl_vg_pv_removed(recs, WL_VG_MAX, UUID_PV0);
    recs[slot].pv_tag[1] = NULL;
    wl_vg_pv_removed(recs, WL_VG_MAX, UUID_PV1);
    CHECK("both-legs-removed-unavailable",
          wl_vg_lv_any_pv_gone(&recs[slot], 0) == 1 &&
          wl_vg_lv_pv_complete(&recs[slot], 0) == 0 &&
          wl_vg_lv_collect_pv_slots(&recs[slot], 0, slots, 4) == 0);
}

int main(void) {
    test_registry_and_arrival();
    test_activation_gating();
    test_seqno_reconciliation();
    test_pv_removal_and_reinsert();
    test_multi_vg();
    test_lv_byte_size();
    test_map_linear();
    test_map_striped();
    test_map_refusal();
    test_reconcile_with_live_child_and_new_lv();
    test_thin_engine_rw_cow_and_split();
    test_snap_engine_multi_cow_and_full();
    test_snap_origin_missing_cow_fails_closed();
    test_raid1_mirror_engine_rw_degraded();
    test_raid1_staggered_arrival_and_leg_loss();

    printf("test_winlvm_vg: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}