/*
 * winlvm_vg.c — Multi-VG registry and PV arrival state machine (freestanding C11).
 *
 * All state lives in the caller-owned record array (zero heap allocation,
 * no WDM or CRT dependencies). Records are array slots reclaimed only on
 * bus removal or inactive-record reset.
 */
#include "winlvm_vg.h"

static void wl_zero(void *p, size_t n) {
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

static int wl_str_eq(const char *a, const char *b, size_t cap) {
    for (size_t i = 0; i < cap; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

static int wl_uuid_eq(const char *a, const char *b) {
    return wl_str_eq(a, b, WL_VG_UUID_LEN);
}

static void wl_uuid_cpy(char dst[WL_VG_UUID_LEN], const char *src) {
    size_t i = 0;
    for (; i + 1 < WL_VG_UUID_LEN && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void wl_name_cpy(char dst[WL_VG_NAME_LEN], const char *src) {
    size_t i = 0;
    for (; i + 1 < WL_VG_NAME_LEN && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int wl_add_ovf(uint64_t a, uint64_t b, uint64_t *r) {
    *r = a + b;
    return *r < a;
}

static int wl_mul_ovf(uint64_t a, uint64_t b, uint64_t *r) {
    if (b != 0 && a > UINT64_MAX / b) return 1;
    *r = a * b;
    return 0;
}

static int wl_view_pv_index(const lvmcore_volume_group *vg, const char *key) {
    for (size_t i = 0; i < vg->pv_count; i++) {
        if (wl_str_eq(vg->pvs[i].key, key, sizeof(vg->pvs[i].key))) return (int)i;
    }
    return -1;
}

static int wl_rec_pv_slot(const wl_vg_record *r, const char *pv_uuid) {
    for (size_t i = 0; i < r->pv_count; i++) {
        if (wl_uuid_eq(r->pv_uuid[i], pv_uuid)) return (int)i;
    }
    return -1;
}

static int wl_slot_free(const wl_vg_record *r) {
    return r->vg_uuid[0] == '\0';
}

void wl_vg_registry_init(wl_vg_record *recs, size_t count) {
    if (!recs) return;
    wl_zero(recs, count * sizeof(*recs));
}

int wl_vg_find(const wl_vg_record *recs, size_t count, const char *vg_uuid) {
    if (!recs || !vg_uuid || !vg_uuid[0]) return -1;
    for (size_t i = 0; i < count; i++) {
        if (!wl_slot_free(&recs[i]) && wl_uuid_eq(recs[i].vg_uuid, vg_uuid)) {
            return (int)i;
        }
    }
    return -1;
}

void wl_vg_ref(wl_vg_record *r) {
    if (r && r->refs < 0xFFFFFFFFu) r->refs++;
}

void wl_vg_unref(wl_vg_record *r) {
    if (r && r->refs > 0) r->refs--;
}

int wl_vg_lv_child_eligible(const lvmcore_logical_volume *lv) {
    return lv && lv->visible && lv->mappable && !lv->internal;
}

uint64_t wl_lv_byte_size(const lvmcore_volume_group *vg, size_t lv_index) {
    if (!vg || !vg->lvs || lv_index >= vg->lv_count) return 0;
    const lvmcore_logical_volume *lv = &vg->lvs[lv_index];
    if (lv->segment_count == 0 || !lv->segments) return 0;
    uint64_t total = 0;
    for (size_t s = 0; s < lv->segment_count; s++) {
        uint64_t extents, bytes;
        if (wl_mul_ovf(lv->segments[s].extent_count, vg->extent_size_sectors, &extents)) {
            return 0;
        }
        if (wl_mul_ovf(extents, 512ULL, &bytes)) return 0;
        if (wl_add_ovf(total, bytes, &total)) return 0;
    }
    return total;
}

int wl_vg_lv_pv_complete(const wl_vg_record *r, size_t lv_index) {
    if (!r || !r->view || !r->view->lvs || !r->view->pvs || lv_index >= r->view->lv_count) {
        return 0;
    }
    const lvmcore_logical_volume *lv = &r->view->lvs[lv_index];
    if (!wl_vg_lv_child_eligible(lv)) return 0;
    if (lv->segment_count == 0 || !lv->segments) return 0;

    for (size_t s = 0; s < lv->segment_count; s++) {
        const lvmcore_segment *seg = &lv->segments[s];
        if (seg->type != LVMCORE_SEG_STRIPED) return 0;
        if (seg->extent_count == 0 || seg->stripe_count < 1 ||
            seg->stripe_count_used != seg->stripe_count || !seg->stripes) {
            return 0;
        }
        for (size_t t = 0; t < seg->stripe_count_used; t++) {
            int vp = wl_view_pv_index(r->view, seg->stripes[t].pv_key);
            if (vp < 0 || r->view->pvs[vp].missing) return 0;
            int slot = wl_rec_pv_slot(r, r->view->pvs[vp].id);
            if (slot < 0 || r->pv_state[slot] != WL_PV_PRESENT) return 0;
        }
    }
    return 1;
}

int wl_vg_lv_all_gone(const wl_vg_record *r, size_t lv_index) {
    if (!r || !r->view || !r->view->lvs || !r->view->pvs || lv_index >= r->view->lv_count) {
        return 0;
    }
    const lvmcore_logical_volume *lv = &r->view->lvs[lv_index];
    if (lv->segment_count == 0 || !lv->segments) return 0;
    int checked = 0;
    for (size_t s = 0; s < lv->segment_count; s++) {
        const lvmcore_segment *seg = &lv->segments[s];
        if (seg->stripe_count_used == 0 || !seg->stripes) return 0;
        for (size_t t = 0; t < seg->stripe_count_used; t++) {
            int vp = wl_view_pv_index(r->view, seg->stripes[t].pv_key);
            if (vp < 0) return 0;
            int slot = wl_rec_pv_slot(r, r->view->pvs[vp].id);
            if (slot < 0 || r->pv_state[slot] != WL_PV_GONE) return 0;
            checked = 1;
        }
    }
    return checked;
}

int wl_vg_lv_any_pv_gone(const wl_vg_record *r, size_t lv_index) {
    if (!r || !r->view || r->dead || !r->view->lvs || !r->view->pvs ||
        lv_index >= r->view->lv_count) {
        return 1;
    }
    const lvmcore_logical_volume *lv = &r->view->lvs[lv_index];
    if (lv->segment_count == 0 || !lv->segments) return 1;
    for (size_t s = 0; s < lv->segment_count; s++) {
        const lvmcore_segment *seg = &lv->segments[s];
        if (seg->stripe_count_used == 0 || !seg->stripes) return 1;
        for (size_t t = 0; t < seg->stripe_count_used; t++) {
            int vp = wl_view_pv_index(r->view, seg->stripes[t].pv_key);
            if (vp < 0 || r->view->pvs[vp].missing) return 1;
            int slot = wl_rec_pv_slot(r, r->view->pvs[vp].id);
            if (slot < 0 || r->pv_state[slot] == WL_PV_GONE) return 1;
        }
    }
    return 0;
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

static void wl_recompute_vg_state(wl_vg_record *r) {
    size_t present = 0, gone = 0;
    for (size_t i = 0; i < r->pv_count; i++) {
        if (r->pv_state[i] == WL_PV_PRESENT) present++;
        else if (r->pv_state[i] == WL_PV_GONE) gone++;
    }
    r->state = (present == r->pv_count) ? WL_VG_COMPLETE : WL_VG_INCOMPLETE;
    r->dead = (r->pv_count > 0 && present == 0 && gone == r->pv_count) ? 1 : 0;
}

static void wl_collect_newly(wl_vg_record *r, size_t *out, size_t cap, size_t *n) {
    *n = 0;
    for (size_t i = 0; i < r->view->lv_count && i < WL_VG_MAX_LVS; i++) {
        if (*n >= cap) return;
        if (!wl_vg_lv_activated(r, i) && wl_vg_lv_pv_complete(r, i)) {
            out[(*n)++] = i;
        }
    }
}

static int wl_lv_geometry_changed(const lvmcore_volume_group *old,
                                  const lvmcore_volume_group *newv,
                                  size_t old_index) {
    const lvmcore_logical_volume *a = &old->lvs[old_index];
    const lvmcore_logical_volume *b = NULL;
    for (size_t i = 0; i < newv->lv_count; i++) {
        if (wl_uuid_eq(newv->lvs[i].id, a->id)) {
            b = &newv->lvs[i];
            break;
        }
    }
    if (!b) return 1;
    if (old->read_only != newv->read_only || a->read_only != b->read_only) return 1;
    if (a->segment_count != b->segment_count) return 1;
    if (a->mappable != b->mappable || a->visible != b->visible ||
        a->internal != b->internal) {
        return 1;
    }
    for (size_t s = 0; s < a->segment_count; s++) {
        const lvmcore_segment *sa = &a->segments[s];
        const lvmcore_segment *sb = &b->segments[s];
        if (sa->start_extent != sb->start_extent ||
            sa->extent_count != sb->extent_count ||
            sa->type != sb->type ||
            sa->stripe_count != sb->stripe_count ||
            sa->stripe_size_sectors != sb->stripe_size_sectors ||
            sa->stripe_count_used != sb->stripe_count_used) {
            return 1;
        }
        if ((sa->stripe_count_used > 0) && (!sa->stripes || !sb->stripes)) return 1;
        for (size_t t = 0; t < sa->stripe_count_used; t++) {
            if (!wl_str_eq(sa->stripes[t].pv_key, sb->stripes[t].pv_key,
                           sizeof(sa->stripes[t].pv_key)) ||
                sa->stripes[t].start_extent != sb->stripes[t].start_extent) {
                return 1;
            }
        }
    }
    return 0;
}

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

static void wl_remap_lv_activated(wl_vg_record *r,
                                  const lvmcore_volume_group *old_view,
                                  const lvmcore_volume_group *new_view) {
    unsigned char next_act[(WL_VG_MAX_LVS + 7) / 8];
    wl_zero(next_act, sizeof(next_act));
    if (old_view && old_view->lvs && new_view && new_view->lvs) {
        for (size_t i = 0; i < old_view->lv_count && i < WL_VG_MAX_LVS; i++) {
            if (!wl_vg_lv_activated(r, i)) continue;
            for (size_t k = 0; k < new_view->lv_count && k < WL_VG_MAX_LVS; k++) {
                if (wl_uuid_eq(old_view->lvs[i].id, new_view->lvs[k].id)) {
                    next_act[k >> 3] |= (unsigned char)(1u << (k & 7));
                    break;
                }
            }
        }
    }
    for (size_t b = 0; b < sizeof(next_act); b++) {
        r->lv_activated[b] = next_act[b];
    }
}

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
        !pv_uuid[0] || !view->id[0] || !view->pvs) {
        return WL_ARR_REJECTED;
    }
    if (view->pv_count == 0 || view->pv_count > WL_VG_MAX_PVS ||
        view->lv_count > WL_VG_MAX_LVS) {
        return WL_ARR_REJECTED;
    }

    int view_pv = -1;
    for (size_t i = 0; i < view->pv_count; i++) {
        if (wl_uuid_eq(view->pvs[i].id, pv_uuid)) {
            if (view_pv >= 0) return WL_ARR_REJECTED;
            view_pv = (int)i;
        }
    }
    if (view_pv < 0) return WL_ARR_REJECTED;

    int slot = wl_vg_find(recs, count, view->id);
    wl_vg_record *r;

    if (slot < 0) {
        size_t free_slot = count;
        for (size_t i = 0; i < count; i++) {
            if (wl_slot_free(&recs[i])) {
                free_slot = i;
                break;
            }
        }
        if (free_slot == count) return WL_ARR_REJECTED;

        r = &recs[free_slot];
        wl_uuid_cpy(r->vg_uuid, view->id);
        wl_name_cpy(r->vg_name, view->name);
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
        r->view = view;
        wl_zero(r->lv_activated, sizeof(r->lv_activated));
        wl_recompute_vg_state(r);
        if (newly_out && newly_count) {
            wl_collect_newly(r, newly_out, newly_cap, newly_count);
        }
        *slot_out = free_slot;
        return (r->state == WL_VG_COMPLETE) ? WL_ARR_COMPLETED : WL_ARR_PENDING;
    }

    r = &recs[slot];
    {
        int pv_slot = wl_rec_pv_slot(r, pv_uuid);
        if (pv_slot >= 0 && r->pv_state[pv_slot] == WL_PV_PRESENT &&
            view->seqno <= r->seqno) {
            return WL_ARR_DUPLICATE;
        }
    }

    if (view->seqno > r->seqno) {
        if (r->refs == 0) {
            r->view = view;
            r->seqno = view->seqno;
            wl_name_cpy(r->vg_name, view->name);
            wl_rec_rebuild(r, view, pv_uuid, instance_tag);
            wl_zero(r->lv_activated, sizeof(r->lv_activated));
            r->stale = 0;
            wl_recompute_vg_state(r);
            if (newly_out && newly_count) {
                wl_collect_newly(r, newly_out, newly_cap, newly_count);
            }
            *slot_out = (size_t)slot;
            return WL_ARR_RECONCILED;
        }

        if (seam) {
            int all_ok = 1;
            for (size_t i = 0; i < r->view->lv_count && i < WL_VG_MAX_LVS; i++) {
                if (!wl_vg_lv_activated(r, i)) continue;
                if (wl_lv_geometry_changed(r->view, view, i)) {
                    if (seam(seam_ctx, r->view->lvs[i].id) != WL_UPDATE_OK) {
                        all_ok = 0;
                        break;
                    }
                }
            }
            if (all_ok) {
                const lvmcore_volume_group *old_view = r->view;
                wl_remap_lv_activated(r, old_view, view);
                r->view = view;
                r->seqno = view->seqno;
                wl_name_cpy(r->vg_name, view->name);
                r->stale = 0;
                wl_rec_rebuild(r, view, pv_uuid, instance_tag);
                wl_recompute_vg_state(r);
                if (newly_out && newly_count) {
                    wl_collect_newly(r, newly_out, newly_cap, newly_count);
                }
                *slot_out = (size_t)slot;
                return WL_ARR_RECONCILED;
            }
        }

        r->stale = 1;
        int arriving_slot = wl_rec_pv_slot(r, pv_uuid);
        if (arriving_slot >= 0) {
            r->pv_state[arriving_slot] = WL_PV_PRESENT;
            r->pv_tag[arriving_slot] = instance_tag;
            wl_recompute_vg_state(r);
            if (newly_out && newly_count) {
                wl_collect_newly(r, newly_out, newly_cap, newly_count);
            }
        }
        *slot_out = (size_t)slot;
        return WL_ARR_STALE_SEQNO;
    }

    {
        int pv_slot = wl_rec_pv_slot(r, pv_uuid);
        if (pv_slot < 0) return WL_ARR_REJECTED;
        r->pv_state[pv_slot] = WL_PV_PRESENT;
        r->pv_tag[pv_slot] = instance_tag;
        wl_recompute_vg_state(r);
        if (newly_out && newly_count) {
            wl_collect_newly(r, newly_out, newly_cap, newly_count);
        }
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
            wl_recompute_vg_state(&recs[i]);
        }
        return;
    }
}
