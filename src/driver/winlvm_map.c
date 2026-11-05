/*
 * winlvm_map.c — Streaming LV-byte to (PV slot, byte offset, length) mapper.
 *
 * Freestanding C11 without WDM or CRT dependencies. Validates segment
 * contiguity, stripe descriptors, and PV liveness; any inconsistency fails
 * closed so callers never issue partial or misaligned transfers.
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

void wl_map_begin(wl_map_iter *it, const lvmcore_volume_group *vg,
                  const wl_vg_record *rec, size_t lv_index,
                  uint64_t lv_byte, uint64_t len) {
    wl_zero(it, sizeof(*it));
    if (!vg || !rec || !vg->lvs || !vg->pvs || lv_index >= vg->lv_count) {
        it->err = 1;
        return;
    }
    const lvmcore_logical_volume *lv = &vg->lvs[lv_index];
    if (!lv->mappable || lv->internal || lv->segment_count == 0 || !lv->segments) {
        it->err = 1;
        return;
    }

    /* Validate contiguous, non-overlapping segment coverage starting at extent 0. */
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

    it->vg = vg;
    it->rec = rec;
    it->lv_index = lv_index;

    uint64_t total = wl_lv_byte_size(vg, lv_index);
    if (total == 0) {
        it->err = 1;
        return;
    }
    if (lv_byte >= total || len == 0) return;

    uint64_t bytes_per_ext;
    if (wl_mul_ovf(vg->extent_size_sectors, 512ULL, &bytes_per_ext) ||
        bytes_per_ext == 0) {
        it->err = 1;
        return;
    }

    uint64_t pos = lv_byte;
    size_t seg = 0;
    while (seg < lv->segment_count) {
        uint64_t seg_bytes;
        if (wl_mul_ovf(lv->segments[seg].extent_count, bytes_per_ext, &seg_bytes)) {
            it->err = 1;
            return;
        }
        if (pos < seg_bytes) {
            it->seg_pos = pos;
            break;
        }
        pos -= seg_bytes;
        seg++;
    }
    if (seg >= lv->segment_count) {
        it->err = 1;
        return;
    }
    it->seg = seg;
    it->remaining = len;
}

int wl_map_next(wl_map_iter *it, uint32_t *pv_index, uint64_t *pv_byte,
                uint64_t *piece_len) {
    if (!it || it->err) return -1;
    if (it->remaining == 0) return 0;

    const lvmcore_volume_group *vg = it->vg;
    const wl_vg_record *rec = it->rec;
    if (!vg || !rec || !rec->view || !vg->lvs || !vg->pvs ||
        it->lv_index >= vg->lv_count) {
        it->err = 1;
        return -1;
    }
    const lvmcore_logical_volume *lv = &vg->lvs[it->lv_index];
    if (!lv->segments || it->seg >= lv->segment_count) {
        it->err = 1;
        return -1;
    }
    const lvmcore_segment *seg = &lv->segments[it->seg];
    if (seg->type != LVMCORE_SEG_STRIPED || !seg->stripes ||
        seg->stripe_count < 1 || seg->stripe_count_used != seg->stripe_count) {
        it->err = 1;
        return -1;
    }

    uint64_t bytes_per_ext;
    if (wl_mul_ovf(vg->extent_size_sectors, 512ULL, &bytes_per_ext) ||
        bytes_per_ext == 0) {
        it->err = 1;
        return -1;
    }
    uint64_t seg_bytes;
    if (wl_mul_ovf(seg->extent_count, bytes_per_ext, &seg_bytes)) {
        it->err = 1;
        return -1;
    }
    if (seg_bytes <= it->seg_pos) {
        it->err = 1;
        return -1;
    }
    uint64_t seg_left = seg_bytes - it->seg_pos;

#define WL_RESOLVE(stripe_var, out_slot, out_vp)                         \
    do {                                                                 \
        int vp_ = wl_view_pv_index(vg, (stripe_var).pv_key);             \
        if (vp_ < 0 || vg->pvs[vp_].missing) {                           \
            it->err = 1;                                                 \
            return -1;                                                   \
        }                                                                \
        int slot_ = wl_rec_pv_slot(rec, vg->pvs[vp_].id);                \
        if (slot_ < 0 || rec->pv_state[slot_] != WL_PV_PRESENT) {        \
            it->err = 1;                                                 \
            return -1;                                                   \
        }                                                                \
        (out_slot) = (uint32_t)slot_;                                    \
        (out_vp) = (size_t)vp_;                                          \
    } while (0)

    uint64_t piece;
    if (seg->stripe_count == 1) {
        uint32_t slot;
        size_t vp;
        WL_RESOLVE(seg->stripes[0], slot, vp);

        uint64_t pe_bytes, ext_bytes, base;
        if (wl_mul_ovf(vg->pvs[vp].pe_start, 512ULL, &pe_bytes) ||
            wl_mul_ovf(seg->stripes[0].start_extent, bytes_per_ext, &ext_bytes) ||
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
        if (it->seg_pos >= seg_bytes) {
            it->seg++;
            it->seg_pos = 0;
        }
        return 1;
    }

    if (seg->stripe_count > WL_VG_MAX_PVS) {
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
    uint64_t c = it->seg_pos / chunk_bytes;
    uint64_t within = it->seg_pos % chunk_bytes;
    uint64_t chunk_left = chunk_bytes - within;
    uint64_t dev = c % n;
    uint64_t k = c / n;

    uint32_t slot;
    size_t vp;
    WL_RESOLVE(seg->stripes[dev], slot, vp);

    uint64_t pe_bytes, ext_bytes, run_bytes, dev_off;
    if (wl_mul_ovf(vg->pvs[vp].pe_start, 512ULL, &pe_bytes) ||
        wl_mul_ovf(seg->stripes[dev].start_extent, bytes_per_ext, &ext_bytes) ||
        wl_mul_ovf(k, chunk_bytes, &run_bytes) ||
        wl_add_ovf(pe_bytes, ext_bytes, &dev_off) ||
        wl_add_ovf(dev_off, run_bytes, &dev_off) ||
        wl_add_ovf(dev_off, within, &dev_off)) {
        it->err = 1;
        return -1;
    }

    piece = (it->remaining < seg_left) ? it->remaining : seg_left;
    if (piece > chunk_left) piece = chunk_left;

    *pv_index = slot;
    *pv_byte = dev_off;
    *piece_len = piece;
    it->seg_pos += piece;
    it->remaining -= piece;
    if (it->seg_pos >= seg_bytes) {
        it->seg++;
        it->seg_pos = 0;
    }
    return 1;
#undef WL_RESOLVE
}
