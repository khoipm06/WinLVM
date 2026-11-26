/*
 * winlvm_map.c — Streaming LV-byte to (PV slot, byte offset, length) mapper,
 *                complex-LV PV completeness / backing slot collector, and
 *                sub-LV read/write bridge (freestanding C11).
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
        if (wl_str_eq(r->pv_uuid[i], pv_uuid, WL_VG_UUID_LEN)) return (int)i;
    }
    return -1;
}

int wl_find_lv_index(const lvmcore_volume_group *vg, const char *name) {
    if (!vg || !vg->lvs || !name || !name[0]) return -1;
    for (size_t i = 0; i < vg->lv_count; i++) {
        if (wl_str_eq(vg->lvs[i].name, name, sizeof(vg->lvs[i].name))) return (int)i;
    }
    return -1;
}

enum wl_sublv_check_mode { WL_CHK_COMPLETE = 0, WL_CHK_ALL_GONE = 1, WL_CHK_ANY_GONE = 2 };

static int wl_sublv_check(const wl_vg_record *r, size_t lv_index, int mode) {
    int fail_val = (mode == WL_CHK_ANY_GONE) ? 1 : 0;
    if (!r || !r->view || (mode == WL_CHK_ANY_GONE && r->dead) ||
        !r->view->lvs || !r->view->pvs || lv_index >= r->view->lv_count) {
        return fail_val;
    }
    const lvmcore_logical_volume *lv = &r->view->lvs[lv_index];
    if (lv->segment_count == 0 || !lv->segments) return fail_val;
    int checked = 0;
    for (size_t s = 0; s < lv->segment_count; s++) {
        const lvmcore_segment *seg = &lv->segments[s];
        if (mode == WL_CHK_COMPLETE) {
            if (seg->type != LVMCORE_SEG_STRIPED || seg->extent_count == 0 ||
                seg->stripe_count < 1 || seg->stripe_count_used != seg->stripe_count || !seg->stripes)
                return 0;
        } else if (seg->stripe_count_used == 0 || !seg->stripes) {
            return fail_val;
        }
        for (size_t t = 0; t < seg->stripe_count_used; t++) {
            int vp = wl_view_pv_index(r->view, seg->stripes[t].pv_key);
            if (vp < 0) return fail_val;
            if (mode != WL_CHK_ALL_GONE && r->view->pvs[vp].missing) return fail_val;
            int slot = wl_rec_pv_slot(r, r->view->pvs[vp].id);
            if (slot < 0) return fail_val;
            if (mode == WL_CHK_COMPLETE && r->pv_state[slot] != WL_PV_PRESENT) return 0;
            if (mode == WL_CHK_ALL_GONE && r->pv_state[slot] != WL_PV_GONE) return 0;
            if (mode == WL_CHK_ANY_GONE && r->pv_state[slot] == WL_PV_GONE) return 1;
            checked = 1;
        }
    }
    if (mode == WL_CHK_ANY_GONE) return 0;
    return (mode == WL_CHK_ALL_GONE) ? checked : 1;
}

int wl_sublv_pv_complete(const wl_vg_record *r, size_t lv_index) {
    return wl_sublv_check(r, lv_index, WL_CHK_COMPLETE);
}
int wl_sublv_all_gone(const wl_vg_record *r, size_t lv_index) {
    return wl_sublv_check(r, lv_index, WL_CHK_ALL_GONE);
}
int wl_sublv_any_pv_gone(const wl_vg_record *r, size_t lv_index) {
    return wl_sublv_check(r, lv_index, WL_CHK_ANY_GONE);
}

static void wl_append_sublv_slots(const wl_vg_record *r, size_t lv_index,
                                  uint32_t *slots_out, size_t slots_cap, size_t *count) {
    if (!r || !r->view || lv_index >= r->view->lv_count) return;
    const lvmcore_logical_volume *lv = &r->view->lvs[lv_index];
    for (size_t s = 0; s < lv->segment_count; s++) {
        const lvmcore_segment *seg = &lv->segments[s];
        if (seg->type != LVMCORE_SEG_STRIPED || !seg->stripes) continue;
        for (size_t t = 0; t < seg->stripe_count_used; t++) {
            int vp = wl_view_pv_index(r->view, seg->stripes[t].pv_key);
            if (vp < 0) continue;
            int slot = wl_rec_pv_slot(r, r->view->pvs[vp].id);
            if (slot < 0 || r->pv_state[slot] != WL_PV_PRESENT) continue;
            int dup = 0;
            for (size_t k = 0; k < *count; k++) {
                if (slots_out[k] == (uint32_t)slot) { dup = 1; break; }
            }
            if (!dup && *count < slots_cap) {
                slots_out[(*count)++] = (uint32_t)slot;
            }
        }
    }
}

size_t wl_vg_lv_collect_pv_slots(const wl_vg_record *r, size_t lv_index,
                                 uint32_t *slots_out, size_t slots_cap) {
    if (!r || !r->view || !slots_out || slots_cap == 0 || lv_index >= r->view->lv_count) return 0;
    const lvmcore_volume_group *v = r->view;
    const lvmcore_logical_volume *lv = &v->lvs[lv_index];
    size_t n = 0;

    if (lv->is_snapshot_cow && lv->snapshot_wrapper[0]) {
        int wi = wl_find_lv_index(v, lv->snapshot_wrapper);
        if (wi >= 0 && v->lvs[wi].segment_count > 0) {
            int oi = wl_find_lv_index(v, v->lvs[wi].segments[0].origin);
            if (oi >= 0) wl_append_sublv_slots(r, (size_t)oi, slots_out, slots_cap, &n);
        }
        wl_append_sublv_slots(r, lv_index, slots_out, slots_cap, &n);
        return n;
    }
    if (lv->segment_count == 0 || !lv->segments) return 0;

    lvmcore_seg_type t = lv->segments[0].type;
    if (t == LVMCORE_SEG_STRIPED) {
        wl_append_sublv_slots(r, lv_index, slots_out, slots_cap, &n);
        if (lv->is_snapshot_origin) {
            for (size_t i = 0; i < v->lv_count; i++) {
                if (v->lvs[i].segment_count > 0 &&
                    v->lvs[i].segments[0].type == LVMCORE_SEG_SNAPSHOT &&
                    wl_str_eq(v->lvs[i].segments[0].origin, lv->name, sizeof(lv->name))) {
                    int ci = wl_find_lv_index(v, v->lvs[i].segments[0].cow_store);
                    if (ci >= 0) wl_append_sublv_slots(r, (size_t)ci, slots_out, slots_cap, &n);
                }
            }
        }
    } else if (t == LVMCORE_SEG_THIN) {
        int pi = wl_find_lv_index(v, lv->segments[0].pool_data);
        if (pi >= 0 && v->lvs[pi].segment_count > 0) {
            int tm = wl_find_lv_index(v, v->lvs[pi].segments[0].pool_metadata);
            int td = wl_find_lv_index(v, v->lvs[pi].segments[0].pool_data);
            if (tm >= 0) wl_append_sublv_slots(r, (size_t)tm, slots_out, slots_cap, &n);
            if (td >= 0) wl_append_sublv_slots(r, (size_t)td, slots_out, slots_cap, &n);
        }
    } else if (t == LVMCORE_SEG_SNAPSHOT) {
        int oi = wl_find_lv_index(v, lv->segments[0].origin);
        int ci = wl_find_lv_index(v, lv->segments[0].cow_store);
        if (oi >= 0) wl_append_sublv_slots(r, (size_t)oi, slots_out, slots_cap, &n);
        if (ci >= 0) wl_append_sublv_slots(r, (size_t)ci, slots_out, slots_cap, &n);
    } else if (t == LVMCORE_SEG_RAID1 || t == LVMCORE_SEG_MIRROR) {
        for (size_t s = 0; s < lv->segment_count; s++) {
            const lvmcore_segment *seg = &lv->segments[s];
            for (size_t l = 0; l < seg->raid_leg_count; l++) {
                int ii = wl_find_lv_index(v, seg->raid_legs[l].image_lv);
                int mi = wl_find_lv_index(v, seg->raid_legs[l].meta_lv);
                if (ii >= 0) wl_append_sublv_slots(r, (size_t)ii, slots_out, slots_cap, &n);
                if (mi >= 0) wl_append_sublv_slots(r, (size_t)mi, slots_out, slots_cap, &n);
            }
            if (seg->mirror_log[0]) {
                int li = wl_find_lv_index(v, seg->mirror_log);
                if (li >= 0) wl_append_sublv_slots(r, (size_t)li, slots_out, slots_cap, &n);
            }
        }
    }
    return n;
}

int wl_vg_lv_raid_all_legs_present(const wl_vg_record *r, size_t lv_index) {
    if (!r || !r->view || lv_index >= r->view->lv_count) return 0;
    const lvmcore_volume_group *v = r->view;
    const lvmcore_logical_volume *lv = &v->lvs[lv_index];
    if (lv->segment_count == 0 || !lv->segments) return 0;
    for (size_t s = 0; s < lv->segment_count; s++) {
        const lvmcore_segment *seg = &lv->segments[s];
        if ((seg->type != LVMCORE_SEG_RAID1 && seg->type != LVMCORE_SEG_MIRROR) ||
            seg->raid_leg_count == 0 || !seg->raid_legs) {
            return 0;
        }
        for (size_t l = 0; l < seg->raid_leg_count; l++) {
            int ii = wl_find_lv_index(v, seg->raid_legs[l].image_lv);
            if (ii < 0 || !wl_sublv_pv_complete(r, (size_t)ii)) return 0;
        }
    }
    return 1;
}

static int wl_lv_pair_sublvs(const lvmcore_volume_group *v, size_t lv_index,
                             int *sub0, int *sub1) {
    const lvmcore_logical_volume *lv = &v->lvs[lv_index];
    *sub0 = -1;
    *sub1 = -1;
    if (lv->is_snapshot_cow && lv->snapshot_wrapper[0]) {
        int wi = wl_find_lv_index(v, lv->snapshot_wrapper);
        if (wi < 0 || v->lvs[wi].segment_count == 0) return -1;
        *sub0 = wl_find_lv_index(v, v->lvs[wi].segments[0].origin);
        *sub1 = (int)lv_index;
        return (*sub0 >= 0) ? 2 : -1;
    }
    if (lv->segment_count == 0 || !lv->segments) return -1;
    lvmcore_seg_type t = lv->segments[0].type;
    if (t == LVMCORE_SEG_STRIPED) {
        *sub0 = (int)lv_index;
        return 1;
    }
    if (t == LVMCORE_SEG_THIN) {
        int pi = wl_find_lv_index(v, lv->segments[0].pool_data);
        if (pi < 0 || v->lvs[pi].segment_count == 0) return -1;
        *sub0 = wl_find_lv_index(v, v->lvs[pi].segments[0].pool_metadata);
        *sub1 = wl_find_lv_index(v, v->lvs[pi].segments[0].pool_data);
        return (*sub0 >= 0 && *sub1 >= 0) ? 2 : -1;
    }
    if (t == LVMCORE_SEG_SNAPSHOT) {
        *sub0 = wl_find_lv_index(v, lv->segments[0].origin);
        *sub1 = wl_find_lv_index(v, lv->segments[0].cow_store);
        return (*sub0 >= 0 && *sub1 >= 0) ? 2 : -1;
    }
    if (t == LVMCORE_SEG_RAID1 || t == LVMCORE_SEG_MIRROR) return 0;
    return -1;
}

int wl_vg_lv_pv_complete(const wl_vg_record *r, size_t lv_index) {
    if (!r || !r->view || !r->view->lvs || !r->view->pvs || lv_index >= r->view->lv_count) return 0;
    const lvmcore_volume_group *v = r->view;
    const lvmcore_logical_volume *lv = &v->lvs[lv_index];
    if (!wl_vg_lv_child_eligible(lv)) return 0;

    int s0 = -1, s1 = -1;
    int kind = wl_lv_pair_sublvs(v, lv_index, &s0, &s1);
    if (kind == 1) return wl_sublv_pv_complete(r, (size_t)s0);
    if (kind == 2) return (wl_sublv_pv_complete(r, (size_t)s0) && wl_sublv_pv_complete(r, (size_t)s1)) ? 1 : 0;
    if (kind != 0) return 0;

    /* Degraded read support: PV-complete when every raid1/mirror segment has >= 1 present leg. */
    for (size_t s = 0; s < lv->segment_count; s++) {
        const lvmcore_segment *seg = &lv->segments[s];
        int ok_leg = 0;
        for (size_t l = 0; l < seg->raid_leg_count; l++) {
            int ii = wl_find_lv_index(v, seg->raid_legs[l].image_lv);
            if (ii >= 0 && wl_sublv_pv_complete(r, (size_t)ii)) { ok_leg = 1; break; }
        }
        if (!ok_leg) return 0;
    }
    return 1;
}

int wl_vg_lv_all_gone(const wl_vg_record *r, size_t lv_index) {
    if (!r || !r->view || !r->view->lvs || lv_index >= r->view->lv_count) return 0;
    const lvmcore_volume_group *v = r->view;
    const lvmcore_logical_volume *lv = &v->lvs[lv_index];

    int s0 = -1, s1 = -1;
    int kind = wl_lv_pair_sublvs(v, lv_index, &s0, &s1);
    if (kind == 1) return wl_sublv_all_gone(r, (size_t)s0);
    if (kind == 2) return (wl_sublv_all_gone(r, (size_t)s0) && wl_sublv_all_gone(r, (size_t)s1)) ? 1 : 0;
    if (kind != 0) return 0;

    for (size_t s = 0; s < lv->segment_count; s++) {
        for (size_t l = 0; l < lv->segments[s].raid_leg_count; l++) {
            int ii = wl_find_lv_index(v, lv->segments[s].raid_legs[l].image_lv);
            if (ii < 0 || !wl_sublv_all_gone(r, (size_t)ii)) return 0;
        }
    }
    return 1;
}

int wl_vg_lv_any_pv_gone(const wl_vg_record *r, size_t lv_index) {
    if (!r || !r->view || r->dead || !r->view->lvs || lv_index >= r->view->lv_count) return 1;
    const lvmcore_volume_group *v = r->view;

    int s0 = -1, s1 = -1;
    int kind = wl_lv_pair_sublvs(v, lv_index, &s0, &s1);
    if (kind == 1) return wl_sublv_any_pv_gone(r, (size_t)s0);
    if (kind == 2) return (wl_sublv_any_pv_gone(r, (size_t)s0) || wl_sublv_any_pv_gone(r, (size_t)s1)) ? 1 : 0;
    if (kind == 0) return wl_vg_lv_pv_complete(r, lv_index) ? 0 : 1;
    return 1;
}

static void wl_map_begin_impl(wl_map_iter *it, const lvmcore_volume_group *vg,
                              const wl_vg_record *rec, size_t lv_index,
                              uint64_t lv_byte, uint64_t len, int allow_internal) {
    wl_zero(it, sizeof(*it));
    if (!vg || !rec || !vg->lvs || !vg->pvs || lv_index >= vg->lv_count) {
        it->err = 1;
        return;
    }
    const lvmcore_logical_volume *lv = &vg->lvs[lv_index];
    if (!allow_internal) {
        if (!lv->mappable || lv->internal || lv->is_snapshot_origin || lv->is_snapshot_cow) {
            it->err = 1;
            return;
        }
    }
    if (lv->segment_count == 0 || !lv->segments) {
        it->err = 1;
        return;
    }

    uint64_t expected_ext = 0;
    for (size_t s = 0; s < lv->segment_count; s++) {
        const lvmcore_segment *seg = &lv->segments[s];
        if (seg->type != LVMCORE_SEG_STRIPED || seg->extent_count == 0 ||
            seg->start_extent != expected_ext || seg->stripe_count < 1 ||
            seg->stripe_count_used != seg->stripe_count || !seg->stripes) {
            it->err = 1;
            return;
        }
        if (wl_add_ovf(expected_ext, seg->extent_count, &expected_ext)) {
            it->err = 1;
            return;
        }
    }

    uint64_t bytes_per_ext;
    if (wl_mul_ovf(vg->extent_size_sectors, 512ULL, &bytes_per_ext) || bytes_per_ext == 0) {
        it->err = 1;
        return;
    }
    uint64_t total;
    if (wl_mul_ovf(expected_ext, bytes_per_ext, &total) || total == 0) {
        it->err = 1;
        return;
    }

    it->vg = vg;
    it->rec = rec;
    it->lv_index = lv_index;
    if (lv_byte >= total || len == 0) return;

    uint64_t pos = lv_byte;
    size_t seg = 0;
    while (seg < lv->segment_count) {
        uint64_t seg_bytes;
        if (wl_mul_ovf(lv->segments[seg].extent_count, bytes_per_ext, &seg_bytes)) {
            it->err = 1;
            return;
        }
        if (pos < seg_bytes) { it->seg_pos = pos; break; }
        pos -= seg_bytes;
        seg++;
    }
    if (seg >= lv->segment_count) { it->err = 1; return; }
    it->seg = seg;
    it->remaining = len;
}

void wl_map_begin(wl_map_iter *it, const lvmcore_volume_group *vg,
                  const wl_vg_record *rec, size_t lv_index,
                  uint64_t lv_byte, uint64_t len) {
    wl_map_begin_impl(it, vg, rec, lv_index, lv_byte, len, 0);
}

void wl_map_begin_raw(wl_map_iter *it, const lvmcore_volume_group *vg,
                      const wl_vg_record *rec, size_t lv_index,
                      uint64_t lv_byte, uint64_t len) {
    wl_map_begin_impl(it, vg, rec, lv_index, lv_byte, len, 1);
}

int wl_map_next(wl_map_iter *it, uint32_t *pv_index, uint64_t *pv_byte,
                uint64_t *piece_len) {
    if (!it || it->err) return -1;
    if (it->remaining == 0) return 0;

    const lvmcore_volume_group *vg = it->vg;
    const wl_vg_record *rec = it->rec;
    if (!vg || !rec || !rec->view || !vg->lvs || !vg->pvs || it->lv_index >= vg->lv_count) {
        it->err = 1;
        return -1;
    }
    const lvmcore_logical_volume *lv = &vg->lvs[it->lv_index];
    if (!lv->segments || it->seg >= lv->segment_count) { it->err = 1; return -1; }
    const lvmcore_segment *seg = &lv->segments[it->seg];
    if (seg->type != LVMCORE_SEG_STRIPED || !seg->stripes ||
        seg->stripe_count < 1 || seg->stripe_count_used != seg->stripe_count) {
        it->err = 1;
        return -1;
    }

    uint64_t bytes_per_ext, seg_bytes;
    if (wl_mul_ovf(vg->extent_size_sectors, 512ULL, &bytes_per_ext) || bytes_per_ext == 0 ||
        wl_mul_ovf(seg->extent_count, bytes_per_ext, &seg_bytes) || seg_bytes <= it->seg_pos) {
        it->err = 1;
        return -1;
    }
    uint64_t seg_left = seg_bytes - it->seg_pos;

#define WL_RESOLVE(stripe_var, out_slot, out_vp)                         \
    do {                                                                 \
        int vp_ = wl_view_pv_index(vg, (stripe_var).pv_key);             \
        if (vp_ < 0 || vg->pvs[vp_].missing) { it->err = 1; return -1; } \
        int slot_ = wl_rec_pv_slot(rec, vg->pvs[vp_].id);                \
        if (slot_ < 0 || rec->pv_state[slot_] != WL_PV_PRESENT) {        \
            it->err = 1; return -1;                                      \
        }                                                                \
        (out_slot) = (uint32_t)slot_;                                    \
        (out_vp) = (size_t)vp_;                                          \
    } while (0)

    uint64_t piece;
    if (seg->stripe_count == 1) {
        uint32_t slot; size_t vp;
        WL_RESOLVE(seg->stripes[0], slot, vp);
        uint64_t pe_bytes, ext_bytes, base;
        if (wl_mul_ovf(vg->pvs[vp].pe_start, 512ULL, &pe_bytes) ||
            wl_mul_ovf(seg->stripes[0].start_extent, bytes_per_ext, &ext_bytes) ||
            wl_add_ovf(pe_bytes, ext_bytes, &base) ||
            wl_add_ovf(base, it->seg_pos, &base)) {
            it->err = 1; return -1;
        }
        piece = (it->remaining < seg_left) ? it->remaining : seg_left;
        *pv_index = slot; *pv_byte = base; *piece_len = piece;
    } else {
        if (seg->stripe_count > WL_VG_MAX_PVS) { it->err = 1; return -1; }
        uint64_t chunk_bytes;
        if (wl_mul_ovf(seg->stripe_size_sectors, 512ULL, &chunk_bytes) || chunk_bytes == 0) {
            it->err = 1; return -1;
        }
        uint64_t c = it->seg_pos / chunk_bytes;
        uint64_t within = it->seg_pos % chunk_bytes;
        uint64_t chunk_left = chunk_bytes - within;
        uint64_t dev = c % seg->stripe_count;
        uint64_t k = c / seg->stripe_count;
        uint32_t slot; size_t vp;
        WL_RESOLVE(seg->stripes[dev], slot, vp);
        uint64_t pe_bytes, ext_bytes, run_bytes, dev_off;
        if (wl_mul_ovf(vg->pvs[vp].pe_start, 512ULL, &pe_bytes) ||
            wl_mul_ovf(seg->stripes[dev].start_extent, bytes_per_ext, &ext_bytes) ||
            wl_mul_ovf(k, chunk_bytes, &run_bytes) ||
            wl_add_ovf(pe_bytes, ext_bytes, &dev_off) ||
            wl_add_ovf(dev_off, run_bytes, &dev_off) ||
            wl_add_ovf(dev_off, within, &dev_off)) {
            it->err = 1; return -1;
        }
        piece = (it->remaining < seg_left) ? it->remaining : seg_left;
        if (piece > chunk_left) piece = chunk_left;
        *pv_index = slot; *pv_byte = dev_off; *piece_len = piece;
    }
    it->seg_pos += piece;
    it->remaining -= piece;
    if (it->seg_pos >= seg_bytes) { it->seg++; it->seg_pos = 0; }
    return 1;
#undef WL_RESOLVE
}

static int wl_sublv_io(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                       size_t lv_index, const wl_pv_io_ops *ops,
                       uint64_t offset, void *rbuf, const void *wbuf, size_t len) {
    int is_write = (wbuf != NULL);
    if (!ops || (is_write ? !ops->write : !ops->read)) return -1;
    if (len == 0) return 0;
    if (!is_write && !rbuf) return -1;
    wl_map_iter it;
    wl_map_begin_raw(&it, vg, rec, lv_index, offset, (uint64_t)len);
    size_t done = 0;
    uint32_t slot = 0; uint64_t pv_off = 0, plen = 0;
    int r;
    while ((r = wl_map_next(&it, &slot, &pv_off, &plen)) == 1) {
        if (slot >= rec->pv_count || rec->pv_state[slot] != WL_PV_PRESENT ||
            plen == 0 || plen > (uint64_t)(len - done)) return -1;
        int io_rc = is_write
            ? ops->write(ops->ctx, slot, pv_off, (const uint8_t *)wbuf + done, (size_t)plen)
            : ops->read(ops->ctx, slot, pv_off, (uint8_t *)rbuf + done, (size_t)plen);
        if (io_rc != 0) return -1;
        done += (size_t)plen;
    }
    return (r == 0 && done == len) ? 0 : -1;
}

int wl_sublv_read(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                  size_t lv_index, const wl_pv_io_ops *ops,
                  uint64_t offset, void *buf, size_t len) {
    return wl_sublv_io(vg, rec, lv_index, ops, offset, buf, NULL, len);
}

int wl_sublv_write(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                   size_t lv_index, const wl_pv_io_ops *ops,
                   uint64_t offset, const void *buf, size_t len) {
    return wl_sublv_io(vg, rec, lv_index, ops, offset, NULL, buf, len);
}
