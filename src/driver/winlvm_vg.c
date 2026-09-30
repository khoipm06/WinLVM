/*
 * winlvm_vg.c — multi-VG registry + PV arrival state machine (host-clean core).
 *
 * Freestanding C11: no WDM, no CRT, no allocation — all state lives in the
 * caller-owned record array (the kernel glue embeds it in the bus FDO
 * extension, so a record is an array slot, never an individually freed
 * allocation — the v22 UAF class is killed at the root).
 *
 * State transitions (WL_TRACE-compatible logging is the glue's job; the
 * core is silent and deterministic):
 *
 *   PV:   MISSING -> PRESENT   (wl_vg_arrival)
 *         PRESENT -> GONE      (wl_vg_pv_removed)
 *         GONE    -> PRESENT   (re-insert; same record object, view kept —
 *                               the v22 "re-insert needs bus restart"
 *                               limitation is lifted for stale-free records)
 *   VG:   INCOMPLETE <-> COMPLETE (per-PV arrival/removal recomputes)
 *         dead=1 when every declared PV is GONE
 *
 * Arrival outcomes: see wl_arrival_result in winlvm_vg.h.
 *
 * View ownership (the glue's arena discipline):
 *   - CREATE (first PV of a VG) and RECONCILED (swap) consume `view`.
 *   - DUPLICATE / PENDING-to-existing / STALE_SEQNO / REJECTED leave `view`
 *     owned by the CALLER (glue frees its arena).
 *   - RECONCILED is only committed when refs == 0 (no live child PDOs) or
 *     the UPDATE_LV seam answered WL_UPDATE_OK (W3 contract: the seam has
 *     already retargeted every live child to the new view's indices).
 *     TODO-W3: UPDATE_LV does not exist yet — the kernel stub returns
 *     WL_UPDATE_UNAVAILABLE and the OLD view keeps serving (stale=1).
 *     Never fake success; never free a record/view under live children.
 */
#include "winlvm_vg.h"

/* ---- freestanding helpers (no CRT) --------------------------------- */

static void wl_zero(void *p, size_t n) {
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

/* UUID compare: both NUL-terminated, at most 32 chars + NUL. */
static int wl_uuid_eq(const char *a, const char *b) {
    for (size_t i = 0; i < WL_VG_UUID_LEN; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1; /* both filled all 33 bytes identically */
}

static void wl_uuid_cpy(char dst[WL_VG_UUID_LEN], const char *src) {
    size_t i = 0;
    for (; i + 1 < WL_VG_UUID_LEN && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int wl_add_ovf(uint64_t a, uint64_t b, uint64_t *r) {
    *r = a + b;
    return *r < a; /* 1 = overflow */
}

static int wl_mul_ovf(uint64_t a, uint64_t b, uint64_t *r) {
    if (b != 0 && a > UINT64_MAX / b) return 1;
    *r = a * b;
    return 0;
}

/* View index of the PV named by an LVM2 key ("pv0"), or -1. */
static int wl_view_pv_index(const lvmcore_volume_group *vg, const char *key) {
    for (size_t i = 0; i < vg->pv_count; i++)
        if (wl_uuid_eq(vg->pvs[i].key, key)) return (int)i;
    return -1;
}

/* Record slot of a PV UUID, or -1. */
static int wl_rec_pv_slot(const wl_vg_record *r, const char *pv_uuid) {
    for (size_t i = 0; i < r->pv_count; i++)
        if (wl_uuid_eq(r->pv_uuid[i], pv_uuid)) return (int)i;
    return -1;
}

/* Slot is free iff it has never been used (empty key). */
static int wl_slot_free(const wl_vg_record *r) {
    return r->vg_uuid[0] == '\0';
}

/* ---- registry ------------------------------------------------------ */

void wl_vg_registry_init(wl_vg_record *recs, size_t count) {
    if (!recs) return;
    wl_zero(recs, count * sizeof(*recs));
}

int wl_vg_find(const wl_vg_record *recs, size_t count, const char *vg_uuid) {
    if (!recs || !vg_uuid || !vg_uuid[0]) return -1;
    for (size_t i = 0; i < count; i++)
        if (!wl_slot_free(&recs[i]) && wl_uuid_eq(recs[i].vg_uuid, vg_uuid))
            return (int)i;
    return -1;
}

void wl_vg_ref(wl_vg_record *r) {
    if (r && r->refs < 0xFFFFFFFFu) r->refs++;
}

void wl_vg_unref(wl_vg_record *r) {
    if (r && r->refs > 0) r->refs--;
}

/* ---- per-LV queries ------------------------------------------------ */

int wl_vg_lv_child_eligible(const lvmcore_logical_volume *lv) {
    return lv && lv->visible && lv->mappable && !lv->internal;
}

uint64_t wl_lv_byte_size(const lvmcore_volume_group *vg, size_t lv_index) {
    if (!vg || lv_index >= vg->lv_count) return 0;
    uint64_t total = 0;
    for (size_t s = 0; s < vg->lvs[lv_index].segment_count; s++) {
        uint64_t extents, bytes;
        if (wl_mul_ovf(vg->lvs[lv_index].segments[s].extent_count,
                       vg->extent_size_sectors, &extents))
            return 0;
        if (wl_mul_ovf(extents, 512ULL, &bytes)) return 0;
        if (wl_add_ovf(total, bytes, &total)) return 0;
    }
    return total;
}

/* 1 iff EVERY PV referenced by the LV's segments is PRESENT. */
int wl_vg_lv_pv_complete(const wl_vg_record *r, size_t lv_index) {
    if (!r || !r->view || lv_index >= r->view->lv_count) return 0;
    const lvmcore_logical_volume *lv = &r->view->lvs[lv_index];
    if (!wl_vg_lv_child_eligible(lv)) return 0;
    for (size_t s = 0; s < lv->segment_count; s++) {
        const lvmcore_segment *seg = &lv->segments[s];
        if (seg->type != LVMCORE_SEG_STRIPED) return 0; /* fail closed */
        for (size_t t = 0; t < seg->stripe_count_used; t++) {
            int vp = wl_view_pv_index(r->view, seg->stripes[t].pv_key);
            if (vp < 0) return 0;
            int slot = wl_rec_pv_slot(r, r->view->pvs[vp].id);
            if (slot < 0 || r->pv_state[slot] != WL_PV_PRESENT) return 0;
        }
    }
    return 1;
}

/* 1 iff EVERY PV referenced by the LV's segments is GONE (drop the child). */
int wl_vg_lv_all_gone(const wl_vg_record *r, size_t lv_index) {
    if (!r || !r->view || lv_index >= r->view->lv_count) return 0;
    const lvmcore_logical_volume *lv = &r->view->lvs[lv_index];
    for (size_t s = 0; s < lv->segment_count; s++) {
        const lvmcore_segment *seg = &lv->segments[s];
        for (size_t t = 0; t < seg->stripe_count_used; t++) {
            int vp = wl_view_pv_index(r->view, seg->stripes[t].pv_key);
            if (vp < 0) return 0;
            int slot = wl_rec_pv_slot(r, r->view->pvs[vp].id);
            if (slot < 0 || r->pv_state[slot] != WL_PV_GONE) return 0;
        }
    }
    return 1;
}

int wl_vg_lv_activated(const wl_vg_record *r, size_t lv_index) {
    if (!r || lv_index >= WL_VG_MAX_LVS) return 0;
    return (r->lv_activated[lv_index >> 3] >> (lv_index & 7)) & 1;
}

void wl_vg_lv_set_activated(wl_vg_record *r, size_t lv_index, int on) {
    if (!r || lv_index >= WL_VG_MAX_LVS) return;
    unsigned char mask = (unsigned char)(1u << (lv_index & 7));
    if (on) r->lv_activated[lv_index >> 3] |= mask;
    else    r->lv_activated[lv_index >> 3] &= (unsigned char)~mask;
}

/* Recompute VG-level state after a PV state change. */
static void wl_recompute_vg_state(wl_vg_record *r) {
    size_t present = 0, gone = 0;
    for (size_t i = 0; i < r->pv_count; i++) {
        if (r->pv_state[i] == WL_PV_PRESENT) present++;
        else if (r->pv_state[i] == WL_PV_GONE) gone++;
    }
    r->state = (present == r->pv_count) ? WL_VG_COMPLETE : WL_VG_INCOMPLETE;
    r->dead = (r->pv_count > 0 && present == 0 && gone == r->pv_count) ? 1 : 0;
}

/* Append LV indices that JUST became activatable: eligible (visible,
 * mappable, non-internal), PV-complete, and no child yet. Never touches
 * the activated bits — the glue claims them under its own lock discipline
 * before creating PDOs. */
static void wl_collect_newly(wl_vg_record *r, size_t *out, size_t cap,
                             size_t *n) {
    *n = 0;
    for (size_t i = 0; i < r->view->lv_count && i < WL_VG_MAX_LVS; i++) {
        if (*n >= cap) return;
        if (!wl_vg_lv_activated(r, i) && wl_vg_lv_pv_complete(r, i))
            out[(*n)++] = i;
    }
}

/* Did the LV (by UUID) change geometry between views? Drives the UPDATE_LV
 * seam on RECONCILED: any activated child whose geometry changed must be
 * retargeted through W3's UPDATE_LV rather than torn down. */
static int wl_lv_geometry_changed(const lvmcore_volume_group *old,
                                  const lvmcore_volume_group *newv,
                                  size_t old_index) {
    const lvmcore_logical_volume *a = &old->lvs[old_index];
    const lvmcore_logical_volume *b = NULL;
    for (size_t i = 0; i < newv->lv_count; i++)
        if (wl_uuid_eq(newv->lvs[i].id, a->id)) { b = &newv->lvs[i]; break; }
    if (!b) return 1; /* LV vanished in the new view — geometry changed */
    if (a->segment_count != b->segment_count) return 1;
    if (a->mappable != b->mappable || a->visible != b->visible ||
        a->internal != b->internal)
        return 1;
    for (size_t s = 0; s < a->segment_count; s++) {
        const lvmcore_segment *sa = &a->segments[s];
        const lvmcore_segment *sb = &b->segments[s];
        if (sa->start_extent != sb->start_extent ||
            sa->extent_count != sb->extent_count ||
            sa->type != sb->type ||
            sa->stripe_count != sb->stripe_count ||
            sa->stripe_size_sectors != sb->stripe_size_sectors ||
            sa->stripe_count_used != sb->stripe_count_used)
            return 1;
        for (size_t t = 0; t < sa->stripe_count_used; t++) {
            if (!wl_uuid_eq(sa->stripes[t].pv_key, sb->stripes[t].pv_key) ||
                sa->stripes[t].start_extent != sb->stripes[t].start_extent)
                return 1;
        }
    }
    return 0;
}

/* Rebuild the record's PV table from a winner view, preserving per-PV
 * state of disks already tracked. Also marks the arriving PV PRESENT. */
static void wl_rec_rebuild(wl_vg_record *r,
                           const lvmcore_volume_group *view,
                           const char *pv_uuid, const void *instance_tag) {
    char new_uuid[WL_VG_MAX_PVS][WL_VG_UUID_LEN];
    wl_pv_state new_state[WL_VG_MAX_PVS];
    const void *new_tag[WL_VG_MAX_PVS];
    for (size_t i = 0; i < view->pv_count; i++) {
        wl_uuid_cpy(new_uuid[i], view->pvs[i].id);
        int old_slot = wl_rec_pv_slot(r, view->pvs[i].id);
        if (old_slot >= 0) {
            new_state[i] = r->pv_state[old_slot];
            new_tag[i] = r->pv_tag[old_slot];
        } else {
            new_state[i] = WL_PV_MISSING;
            new_tag[i] = NULL;
        }
    }
    r->pv_count = view->pv_count;
    for (size_t i = 0; i < view->pv_count; i++) {
        wl_uuid_cpy(r->pv_uuid[i], new_uuid[i]);
        r->pv_state[i] = new_state[i];
        r->pv_tag[i] = new_tag[i];
    }
    int arriving_slot = wl_rec_pv_slot(r, pv_uuid);
    if (arriving_slot >= 0) {
        r->pv_state[arriving_slot] = WL_PV_PRESENT;
        r->pv_tag[arriving_slot] = instance_tag;
    }
}

/* ---- arrival state machine ----------------------------------------- */

wl_arrival_result wl_vg_arrival(wl_vg_record *recs, size_t count,
                                size_t *slot_out,
                                const lvmcore_volume_group *view,
                                const char *pv_uuid,
                                const void *instance_tag,
                                wl_lv_update_fn seam, void *seam_ctx,
                                size_t *newly_out, size_t newly_cap,
                                size_t *newly_count) {
    if (newly_count) *newly_count = 0;
    if (!recs || !count || !slot_out || !view || !pv_uuid ||
        !pv_uuid[0] || !view->id[0] || !view->pvs)
        return WL_ARR_REJECTED;

    /* Fail closed on pathological metadata (R5). */
    if (view->pv_count == 0 || view->pv_count > WL_VG_MAX_PVS ||
        view->lv_count > WL_VG_MAX_LVS)
        return WL_ARR_REJECTED;

    /* The arriving PV must be declared in its own metadata. */
    int view_pv = -1;
    for (size_t i = 0; i < view->pv_count; i++) {
        if (wl_uuid_eq(view->pvs[i].id, pv_uuid)) {
            if (view_pv >= 0) return WL_ARR_REJECTED; /* duplicate UUID */
            view_pv = (int)i;
        }
    }
    if (view_pv < 0) return WL_ARR_REJECTED;

    int slot = wl_vg_find(recs, count, view->id);
    wl_vg_record *r;

    if (slot < 0) {
        /* ---------------- first arrival: create the record ------------- */
        size_t free_slot = count;
        for (size_t i = 0; i < count; i++)
            if (wl_slot_free(&recs[i])) { free_slot = i; break; }
        if (free_slot == count) return WL_ARR_REJECTED; /* registry full */

        r = &recs[free_slot];
        wl_uuid_cpy(r->vg_uuid, view->id);
        for (size_t i = 0; i < sizeof(r->vg_name) && i < sizeof(view->name);
             i++)
            r->vg_name[i] = view->name[i];
        r->seqno = view->seqno;
        r->state = WL_VG_INCOMPLETE;
        r->stale = 0;
        r->dead = 0;
        r->refs = 0;
        r->pv_count = view->pv_count;
        for (size_t i = 0; i < view->pv_count; i++) {
            wl_uuid_cpy(r->pv_uuid[i], view->pvs[i].id);
            r->pv_state[i] = WL_PV_MISSING;
            r->pv_tag[i] = NULL;
        }
        r->pv_state[view_pv] = WL_PV_PRESENT;
        r->pv_tag[view_pv] = instance_tag;
        r->view = view; /* ownership moves to the record */
        wl_zero(r->lv_activated, sizeof(r->lv_activated));
        wl_recompute_vg_state(r);
        if (newly_out && newly_count)
            wl_collect_newly(r, newly_out, newly_cap, newly_count);
        *slot_out = free_slot;
        return (r->state == WL_VG_COMPLETE) ? WL_ARR_COMPLETED : WL_ARR_PENDING;
    }

    r = &recs[slot];

    /* ---------------- duplicate-arrival dedupe ---------------- */
    /* Same PV UUID already PRESENT with same/lower seqno: no-op.
     * Higher-seqno arrivals must not be dropped as duplicates: they carry
     * updated VG metadata that must go through reconciliation below. */
    {
        int pv_slot = wl_rec_pv_slot(r, pv_uuid);
        if (pv_slot >= 0 && r->pv_state[pv_slot] == WL_PV_PRESENT &&
            view->seqno <= r->seqno)
            return WL_ARR_DUPLICATE;
    }

    /* ---------------- seqno reconciliation on LATE arrival ------------- */
    if (view->seqno > r->seqno) {
        /* A higher-seqno view arrived after the record went active: the
         * pinned map is stale. Policy: re-target from the max-seqno winner.
         * Never free a view under live child PDOs. */
        if (r->refs == 0) {
            /* No live children: safe to swap the pinned view in place. */
            const lvmcore_volume_group *old = r->view;
            r->view = view; /* ownership moves to the record */
            r->seqno = view->seqno;
            for (size_t i = 0; i < sizeof(r->vg_name) &&
                 i < sizeof(view->name); i++)
                r->vg_name[i] = view->name[i];
            /* Rebuild the PV table from the winner, preserving the state
             * of physical disks that are still here. */
            wl_rec_rebuild(r, view, pv_uuid, instance_tag);
            /* No children existed, so activation bits are dead weight —
             * clear them; collect_newly re-derives from the winner. */
            wl_zero(r->lv_activated, sizeof(r->lv_activated));
            r->stale = 0;
            wl_recompute_vg_state(r);
            if (newly_out && newly_count)
                wl_collect_newly(r, newly_out, newly_cap, newly_count);
            (void)old; /* the GLUE frees the old arena (refs == 0) */
            *slot_out = (size_t)slot;
            return WL_ARR_RECONCILED;
        }

        /* Live children pin the old view. Children whose geometry changed
         * must go through UPDATE_LV (W3) — never torn down, never faked. */
        if (seam) {
            /* Ask the seam to retarget every changed activated child. It
             * returns WL_UPDATE_OK only after the children now reference
             * the NEW view's indices (W3 contract). */
            int all_ok = 1;
            for (size_t i = 0; i < r->view->lv_count && i < WL_VG_MAX_LVS;
                 i++) {
                if (!wl_vg_lv_activated(r, i)) continue;
                if (wl_lv_geometry_changed(r->view, view, i)) {
                    if (seam(seam_ctx, r->view->lvs[i].id) !=
                        WL_UPDATE_OK) {
                        all_ok = 0;
                        break;
                    }
                }
            }
            if (all_ok) {
                const lvmcore_volume_group *old = r->view;
                r->view = view; /* ownership moves to the record */
                r->seqno = view->seqno;
                r->stale = 0;
                /* PV table rebuild — identical policy to the refs==0
                 * branch (children were retargeted by the seam). */
                wl_rec_rebuild(r, view, pv_uuid, instance_tag);
                wl_recompute_vg_state(r);
                if (newly_out && newly_count)
                    wl_collect_newly(r, newly_out, newly_cap, newly_count);
                (void)old; /* GLUE frees the old arena after seam-OK swap */
                *slot_out = (size_t)slot;
                return WL_ARR_RECONCILED;
            }
        }

        /* Seam absent or refused: keep the OLD view serving, mark stale.
         * The higher-seqno view is NOT consumed — the caller frees it. */
        r->stale = 1;
        int arriving_slot = wl_rec_pv_slot(r, pv_uuid);
        if (arriving_slot >= 0) {
            /* The disk IS a member of the serving view: register presence
             * there (an LV may just have become complete). */
            r->pv_state[arriving_slot] = WL_PV_PRESENT;
            r->pv_tag[arriving_slot] = instance_tag;
            wl_recompute_vg_state(r);
            if (newly_out && newly_count)
                wl_collect_newly(r, newly_out, newly_cap, newly_count);
        }
        *slot_out = (size_t)slot;
        return WL_ARR_STALE_SEQNO;
    }

    /* ---------------- normal (out-of-order or re-insert) arrival ------- */
    {
        int pv_slot = wl_rec_pv_slot(r, pv_uuid);
        if (pv_slot < 0)
            return WL_ARR_REJECTED; /* foreign to the current metadata */
        r->pv_state[pv_slot] = WL_PV_PRESENT;
        r->pv_tag[pv_slot] = instance_tag;
        wl_recompute_vg_state(r);
        if (newly_out && newly_count)
            wl_collect_newly(r, newly_out, newly_cap, newly_count);
        *slot_out = (size_t)slot;
        return (r->state == WL_VG_COMPLETE) ? WL_ARR_COMPLETED : WL_ARR_PENDING;
    }
}

void wl_vg_pv_removed(wl_vg_record *recs, size_t count, const char *pv_uuid) {
    if (!recs || !pv_uuid || !pv_uuid[0]) return;
    for (size_t i = 0; i < count; i++) {
        if (wl_slot_free(&recs[i])) continue;
        int slot = wl_rec_pv_slot(&recs[i], pv_uuid);
        if (slot < 0) continue;
        if (recs[i].pv_state[slot] == WL_PV_PRESENT) {
            recs[i].pv_state[slot] = WL_PV_GONE;
            /* Tag keeps identifying the disk (re-insert matching), but the
             * backing is gone — the glue invalidates its PDO state via the
             * returned slot information in its own notification path. */
            wl_recompute_vg_state(&recs[i]);
        }
        return; /* PV UUIDs are globally unique — first hit is the only */
    }
}

/* ---- LV byte mapping ----------------------------------------------- */

void wl_map_begin(wl_map_iter *it, const lvmcore_volume_group *vg,
                  const wl_vg_record *rec, size_t lv_index,
                  uint64_t lv_byte, uint64_t len) {
    wl_zero(it, sizeof(*it));
    if (!vg || !rec || lv_index >= vg->lv_count) { it->err = 1; return; }
    const lvmcore_logical_volume *lv = &vg->lvs[lv_index];
    if (!lv->mappable || lv->internal) { it->err = 1; return; }

    it->vg = vg;
    it->rec = rec;
    it->lv_index = lv_index;

    uint64_t total = wl_lv_byte_size(vg, lv_index);
    if (lv_byte >= total || len == 0) return; /* exhausted at once */

    /* Fast-forward to the segment containing lv_byte. */
    uint64_t bytes_per_ext;
    if (wl_mul_ovf(vg->extent_size_sectors, 512ULL, &bytes_per_ext)) {
        it->err = 1;
        return;
    }
    uint64_t pos = lv_byte;
    size_t seg = 0;
    while (seg < lv->segment_count) {
        uint64_t extents;
        if (wl_mul_ovf(lv->segments[seg].extent_count, bytes_per_ext,
                       &extents)) { it->err = 1; return; }
        if (pos < extents) { it->seg_pos = pos; break; }
        pos -= extents;
        seg++;
    }
    if (seg >= lv->segment_count) { it->err = 1; return; }
    it->seg = seg;
    it->remaining = len;
}

int wl_map_next(wl_map_iter *it, uint32_t *pv_index, uint64_t *pv_byte,
                uint64_t *piece_len) {
    if (!it) return -1;
    if (it->err) return -1;
    if (it->remaining == 0) return 0;

    const lvmcore_volume_group *vg = it->vg;
    const wl_vg_record *rec = it->rec;
    if (!vg || !rec || !rec->view || it->lv_index >= vg->lv_count) {
        it->err = 1;
        return -1;
    }
    const lvmcore_logical_volume *lv = &vg->lvs[it->lv_index];
    if (it->seg >= lv->segment_count) { it->err = 1; return -1; }
    const lvmcore_segment *seg = &lv->segments[it->seg];

    uint64_t bytes_per_ext;
    if (wl_mul_ovf(vg->extent_size_sectors, 512ULL, &bytes_per_ext)) {
        it->err = 1;
        return -1;
    }
    uint64_t seg_extents;
    if (wl_mul_ovf(seg->extent_count, bytes_per_ext, &seg_extents)) {
        it->err = 1;
        return -1;
    }
    if (seg_extents <= it->seg_pos) { it->err = 1; return -1; }
    uint64_t seg_left = seg_extents - it->seg_pos;

    /* Resolve a stripe to (record PV slot, view PV index). Terminal fail
     * (return -1) when the PV is not PRESENT: an IRP spanning PVs with a
     * dead PV must fail WHOLE — never a partial completion. */
#define WL_RESOLVE(stripe_var, out_slot, out_vp)                          \
    do {                                                                  \
        int vp_ = wl_view_pv_index(vg, (stripe_var).pv_key);              \
        if (vp_ < 0) { it->err = 1; return -1; }                          \
        int slot_ = wl_rec_pv_slot(rec, vg->pvs[vp_].id);                 \
        if (slot_ < 0 || rec->pv_state[slot_] != WL_PV_PRESENT) {         \
            it->err = 1;                                                  \
            return -1;                                                     \
        }                                                                 \
        (out_slot) = (uint32_t)slot_;                                     \
        (out_vp) = (size_t)vp_;                                            \
    } while (0)

    uint64_t piece;
    if (seg->stripe_count <= 1) {
        /* Linear: the segment's extents are ONE contiguous run on one PV
         * (LVM2 guarantees contiguity per linear segment — dmsetup shows
         * one target per segment). Per-PV pe_start (heterogeneous safe). */
        uint32_t slot;
        size_t vp;
        WL_RESOLVE(seg->stripes[0], slot, vp);
        /* pe_start sectors -> bytes + extent offset + position: */
        uint64_t pe_bytes, ext_bytes, base;
        if (wl_mul_ovf(vg->pvs[vp].pe_start, 512ULL, &pe_bytes) ||
            wl_mul_ovf(seg->stripes[0].start_extent, bytes_per_ext,
                       &ext_bytes) ||
            wl_add_ovf(pe_bytes, ext_bytes, &base) ||
            wl_add_ovf(base, it->seg_pos, &base)) {
            it->err = 1;
            return -1;
        }
        piece = (it->remaining < seg_left) ? it->remaining : seg_left;
        *pv_index = slot;
        *pv_byte = base;
        *piece_len = piece;
        it->seg_pos += piece;
        it->remaining -= piece;
        if (it->seg_pos >= seg_extents) {
            it->seg++;
            it->seg_pos = 0;
        }
        return 1;
    }

    /* Striped (>=2): dm semantics — logical chunk c (chunk_bytes each) on
     * dev (c % N) at dev-relative chunk index c / N, i.e. byte offset
     * pe_start + start_extent*ext + (c/N)*chunk + within. LVM2 allocates
     * each stripe's extents contiguously, so the stripe run on each PV is
     * one contiguous byte range. */
    if (seg->stripe_count_used != seg->stripe_count ||
        seg->stripe_count > WL_VG_MAX_PVS) {
        it->err = 1;
        return -1;
    }
    uint64_t chunk_bytes;
    if (wl_mul_ovf(seg->stripe_size_sectors, 512ULL, &chunk_bytes) ||
        chunk_bytes == 0) {
        it->err = 1;
        return -1;
    }
    uint64_t n = seg->stripe_count;
    uint64_t c = it->seg_pos / chunk_bytes;      /* absolute chunk index */
    uint64_t within = it->seg_pos % chunk_bytes; /* offset inside chunk */
    uint64_t chunk_left = chunk_bytes - within;
    uint64_t dev = c % n;                        /* stripe index */
    uint64_t k = c / n;                          /* chunk index ON that dev */

    uint32_t slot;
    size_t vp;
    WL_RESOLVE(seg->stripes[dev], slot, vp);

    uint64_t pe_bytes, ext_bytes, run_bytes, dev_off;
    if (wl_mul_ovf(vg->pvs[vp].pe_start, 512ULL, &pe_bytes) ||
        wl_mul_ovf(seg->stripes[dev].start_extent, bytes_per_ext,
                   &ext_bytes) ||
        wl_mul_ovf(k, chunk_bytes, &run_bytes) ||
        wl_add_ovf(pe_bytes, ext_bytes, &dev_off) ||
        wl_add_ovf(dev_off, run_bytes, &dev_off) ||
        wl_add_ovf(dev_off, within, &dev_off)) {
        it->err = 1;
        return -1;
    }

    piece = (it->remaining < seg_left) ? it->remaining : seg_left;
    if (piece > chunk_left) piece = chunk_left; /* cut at chunk boundary */

    *pv_index = slot;
    *pv_byte = dev_off;
    *piece_len = piece;
    it->seg_pos += piece;
    it->remaining -= piece;
    if (it->seg_pos >= seg_extents) {
        it->seg++;
        it->seg_pos = 0;
    }
    return 1;
}
