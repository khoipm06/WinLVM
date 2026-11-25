/*
 * winlvm_snap.c — Portable dm-snapshot & dm-snapshot-origin LV adapter.
 *
 * Supports both classic LVM2 snapshot0 wrappers (where the COW LV carries
 * the user-visible snapshot name) and direct visible snapshot LV segments,
 * plus multi-snapshot CoW preservation on snapshot-origin writes.
 */
#ifdef _KERNEL_MODE
#include "winlvm.h"
#else
#include "winlvm_vg.h"
#endif
#include "lvmcore/snap.h"

typedef struct {
    const lvmcore_volume_group *vg;
    const wl_vg_record         *rec;
    size_t                      lv_index;
    const wl_pv_io_ops         *ops;
} wl_snap_sublv;

static int snap_sublv_read(void *impl, uint64_t off, void *buf, size_t len) {
    wl_snap_sublv *a = (wl_snap_sublv *)impl;
    return wl_sublv_read(a->vg, a->rec, a->lv_index, a->ops, off, buf, len);
}

static int snap_sublv_write(void *impl, uint64_t off, const void *buf, size_t len) {
    wl_snap_sublv *a = (wl_snap_sublv *)impl;
    return wl_sublv_write(a->vg, a->rec, a->lv_index, a->ops, off, buf, len);
}

static uint64_t snap_sublv_size(const lvmcore_volume_group *vg, size_t lv_index) {
    if (!vg || !vg->lvs || lv_index >= vg->lv_count) return 0;
    uint64_t exts = lvmcore_lv_extent_count(&vg->lvs[lv_index]);
    if (exts == UINT64_MAX || vg->extent_size_sectors == 0) return 0;
    if (exts > UINT64_MAX / vg->extent_size_sectors) return 0;
    uint64_t secs = exts * vg->extent_size_sectors;
    if (secs > UINT64_MAX / 512ULL) return 0;
    return secs * 512ULL;
}

static int snap_str_eq(const char *a, const char *b, size_t cap) {
    for (size_t i = 0; i < cap; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

static lvmcore_status resolve_snap_pair(const lvmcore_volume_group *vg,
                                        size_t lv_index,
                                        size_t *origin_idx,
                                        size_t *cow_idx) {
    if (!vg || !vg->lvs || lv_index >= vg->lv_count) return LVMCORE_ERR_INVALID_ARG;
    const lvmcore_logical_volume *lv = &vg->lvs[lv_index];
    if (lv->is_snapshot_cow && lv->snapshot_wrapper[0]) {
        int wi = wl_find_lv_index(vg, lv->snapshot_wrapper);
        if (wi < 0 || vg->lvs[wi].segment_count == 0 ||
            vg->lvs[wi].segments[0].type != LVMCORE_SEG_SNAPSHOT) {
            return LVMCORE_ERR_CORRUPT_METADATA;
        }
        int oi = wl_find_lv_index(vg, vg->lvs[wi].segments[0].origin);
        if (oi < 0) return LVMCORE_ERR_CORRUPT_METADATA;
        *origin_idx = (size_t)oi;
        *cow_idx = lv_index;
        return LVMCORE_OK;
    }
    if (lv->segment_count > 0 && lv->segments && lv->segments[0].type == LVMCORE_SEG_SNAPSHOT) {
        int oi = wl_find_lv_index(vg, lv->segments[0].origin);
        int ci = wl_find_lv_index(vg, lv->segments[0].cow_store);
        if (oi < 0 || ci < 0) return LVMCORE_ERR_CORRUPT_METADATA;
        *origin_idx = (size_t)oi;
        *cow_idx = (size_t)ci;
        return LVMCORE_OK;
    }
    return LVMCORE_ERR_UNSUPPORTED;
}

lvmcore_status wl_snap_lv_read(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                               size_t lv_index, const wl_pv_io_ops *ops,
                               uint8_t *table_buf, size_t table_buf_cap,
                               uint64_t offset, void *dst, size_t len) {
    size_t oi = 0, ci = 0;
    lvmcore_status st = resolve_snap_pair(vg, lv_index, &oi, &ci);
    if (st != LVMCORE_OK) return st;

    uint64_t orig_sz = snap_sublv_size(vg, oi);
    if (orig_sz == 0 || offset > orig_sz || (uint64_t)len > orig_sz - offset) {
        return LVMCORE_ERR_INVALID_ARG;
    }

    wl_snap_sublv o_ad = { vg, rec, oi, ops };
    wl_snap_sublv c_ad = { vg, rec, ci, ops };
    lvmcore_rw_source o_src = { &o_ad, orig_sz, snap_sublv_read, NULL };
    lvmcore_rw_source c_src = { &c_ad, snap_sublv_size(vg, ci), snap_sublv_read, NULL };

    return lvmcore_snap_read(&o_src, &c_src, table_buf, table_buf_cap, offset, dst, len);
}

lvmcore_status wl_snap_lv_write(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                                size_t lv_index, const wl_pv_io_ops *ops,
                                uint8_t *chunk_buf, uint8_t *table_buf, size_t buf_cap,
                                uint64_t offset, const void *src, size_t len) {
    size_t oi = 0, ci = 0;
    lvmcore_status st = resolve_snap_pair(vg, lv_index, &oi, &ci);
    if (st != LVMCORE_OK) return st;

    uint64_t orig_sz = snap_sublv_size(vg, oi);
    if (orig_sz == 0 || offset > orig_sz || (uint64_t)len > orig_sz - offset) {
        return LVMCORE_ERR_INVALID_ARG;
    }

    wl_snap_sublv o_ad = { vg, rec, oi, ops };
    wl_snap_sublv c_ad = { vg, rec, ci, ops };
    lvmcore_rw_source o_src = { &o_ad, orig_sz, snap_sublv_read, NULL };
    lvmcore_rw_source c_src = { &c_ad, snap_sublv_size(vg, ci), snap_sublv_read, snap_sublv_write };

    return lvmcore_snap_write(&o_src, &c_src, chunk_buf, table_buf, buf_cap, offset, src, len);
}

lvmcore_status wl_snap_origin_lv_write(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                                       size_t lv_index, const wl_pv_io_ops *ops,
                                       uint8_t *chunk_buf, uint8_t *table_buf, size_t buf_cap,
                                       uint64_t offset, const void *src, size_t len) {
    if (!vg || !rec || !ops || !vg->lvs || lv_index >= vg->lv_count || (!src && len > 0)) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (len == 0) return LVMCORE_OK;

    uint64_t orig_sz = snap_sublv_size(vg, lv_index);
    if (orig_sz == 0 || offset > orig_sz || (uint64_t)len > orig_sz - offset) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    const lvmcore_logical_volume *orig = &vg->lvs[lv_index];

    /* Fail closed before mutating origin if any attached snapshot COW PV is missing. */
    for (size_t i = 0; i < vg->lv_count; i++) {
        const lvmcore_logical_volume *cand = &vg->lvs[i];
        if (cand->segment_count == 0 || !cand->segments) continue;
        if (cand->segments[0].type != LVMCORE_SEG_SNAPSHOT) continue;
        if (!snap_str_eq(cand->segments[0].origin, orig->name, sizeof(orig->name))) continue;
        int ci = wl_find_lv_index(vg, cand->segments[0].cow_store);
        if (ci < 0 || !wl_sublv_pv_complete(rec, (size_t)ci)) {
            return LVMCORE_ERR_IO;
        }
    }

    wl_snap_sublv o_ad = { vg, rec, lv_index, ops };
    lvmcore_rw_source o_src = { &o_ad, orig_sz, snap_sublv_read, snap_sublv_write };

    /* Preserve pre-write origin chunks into every attached snapshot with O(1) stack. */
    for (size_t i = 0; i < vg->lv_count; i++) {
        const lvmcore_logical_volume *cand = &vg->lvs[i];
        if (cand->segment_count == 0 || !cand->segments) continue;
        if (cand->segments[0].type != LVMCORE_SEG_SNAPSHOT) continue;
        if (!snap_str_eq(cand->segments[0].origin, orig->name, sizeof(orig->name))) continue;
        int ci = wl_find_lv_index(vg, cand->segments[0].cow_store);
        if (ci < 0) return LVMCORE_ERR_IO;

        wl_snap_sublv c_ad = { vg, rec, (size_t)ci, ops };
        lvmcore_rw_source c_src = {
            &c_ad, snap_sublv_size(vg, (size_t)ci), snap_sublv_read, snap_sublv_write
        };
        lvmcore_status st = lvmcore_snap_origin_preserve_one(
            &o_src, &c_src, chunk_buf, table_buf, buf_cap, offset, len);
        if (st != LVMCORE_OK) return st;
    }

    return (wl_sublv_write(vg, rec, lv_index, ops, offset, src, len) == 0)
               ? LVMCORE_OK : LVMCORE_ERR_IO;
}
