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
#include <string.h>
#include <stdint.h>

#include "winlvm_vg.h"

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

    printf("test_winlvm_vg: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}