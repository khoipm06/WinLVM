/*
 * lvmcore_snap.c — Freestanding C11 Linux dm-snapshot & snapshot-origin engine.
 *
 * Non-recursive, zero dynamic allocations, stack frames <= 512 bytes.
 */
#include "lvmcore/snap.h"

#if defined(__GNUC__) || defined(__clang__)
#define SNAP_NOINLINE __attribute__((noinline))
#else
#define SNAP_NOINLINE
#endif

static void sn_zero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = 0;
}

static void sn_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static uint32_t sn_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t sn_le64(const uint8_t *p) {
    return (uint64_t)sn_le32(p) | ((uint64_t)sn_le32(p + 4) << 32);
}

static void sn_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void sn_put_le64(uint8_t *p, uint64_t v) {
    sn_put_le32(p, (uint32_t)(v & 0xffffffffu));
    sn_put_le32(p + 4, (uint32_t)(v >> 32));
}

SNAP_NOINLINE lvmcore_status lvmcore_snap_read_header(const lvmcore_rw_source *cow,
                                                      lvmcore_snap_header *hdr_out) {
    if (!cow || !cow->read || !hdr_out) return LVMCORE_ERR_INVALID_ARG;
    sn_zero(hdr_out, sizeof(*hdr_out));
    if (cow->size < 512u) return LVMCORE_ERR_CORRUPT_METADATA;

    uint8_t sec[16];
    if (cow->read(cow->impl, 0, sec, sizeof(sec)) != 0) {
        return LVMCORE_ERR_IO;
    }
    hdr_out->magic = sn_le32(sec);
    hdr_out->valid = sn_le32(sec + 4);
    hdr_out->version = sn_le32(sec + 8);
    hdr_out->chunk_size_sectors = sn_le32(sec + 12);

    if (hdr_out->magic != LVMCORE_SNAP_MAGIC) return LVMCORE_ERR_CORRUPT_METADATA;
    if (hdr_out->version != LVMCORE_SNAP_VERSION) return LVMCORE_ERR_UNSUPPORTED;
    if (hdr_out->chunk_size_sectors < 8u || hdr_out->chunk_size_sectors > 2048u ||
        (hdr_out->chunk_size_sectors & (hdr_out->chunk_size_sectors - 1u)) != 0) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }
    return LVMCORE_OK;
}

SNAP_NOINLINE static lvmcore_status snap_invalidate(const lvmcore_rw_source *cow,
                                                    const lvmcore_snap_header *hdr) {
    if (!cow || !cow->write || !hdr) return LVMCORE_ERR_IO;
    uint8_t sec[16];
    sn_put_le32(sec, hdr->magic);
    sn_put_le32(sec + 4, 0u); /* valid = 0 */
    sn_put_le32(sec + 8, hdr->version);
    sn_put_le32(sec + 12, hdr->chunk_size_sectors);
    return (cow->write(cow->impl, 0, sec, sizeof(sec)) == 0) ? LVMCORE_OK : LVMCORE_ERR_IO;
}

typedef struct {
    int      found;
    uint64_t new_chunk;
    uint64_t free_area;
    uint64_t free_idx;
    uint64_t free_data_chunk;
    int      cow_full;
} snap_lookup_res;

/*
 * Scan exception areas on `cow` for `old_chunk`.
 * Also reports the first free exception slot and whether the COW store is full.
 */
SNAP_NOINLINE static lvmcore_status snap_scan_exceptions(const lvmcore_rw_source *cow,
                                                         const lvmcore_snap_header *hdr,
                                                         uint64_t old_chunk,
                                                         uint8_t *table_buf,
                                                         size_t table_buf_cap,
                                                         snap_lookup_res *res) {
    sn_zero(res, sizeof(*res));
    uint64_t chunk_bytes = (uint64_t)hdr->chunk_size_sectors * 512ULL;
    if (chunk_bytes < 512ULL || chunk_bytes > (uint64_t)table_buf_cap) {
        return LVMCORE_ERR_SCRATCH_EXHAUSTED;
    }
    uint64_t exc_per_area = chunk_bytes / 16ULL;
    uint64_t total_cow_chunks = cow->size / chunk_bytes;
    uint64_t stride = exc_per_area + 1ULL;

    for (uint64_t area = 0;; area++) {
        uint64_t tbl_chunk = 1ULL + area * stride;
        if (tbl_chunk >= total_cow_chunks) {
            res->cow_full = 1;
            return LVMCORE_OK;
        }
        if (cow->read(cow->impl, tbl_chunk * chunk_bytes, table_buf, (size_t)chunk_bytes) != 0) {
            return LVMCORE_ERR_IO;
        }
        for (uint64_t idx = 0; idx < exc_per_area; idx++) {
            uint64_t e_old = sn_le64(table_buf + idx * 16ULL);
            uint64_t e_new = sn_le64(table_buf + idx * 16ULL + 8ULL);
            if (e_new == 0) {
                uint64_t data_chunk = tbl_chunk + 1ULL + idx;
                if (data_chunk >= total_cow_chunks) {
                    res->cow_full = 1;
                } else {
                    res->free_area = area;
                    res->free_idx = idx;
                    res->free_data_chunk = data_chunk;
                }
                return LVMCORE_OK;
            }
            if (e_new >= total_cow_chunks || ((e_new - 1ULL) % stride) == 0) {
                return LVMCORE_ERR_CORRUPT_METADATA;
            }
            if (e_old == old_chunk) {
                res->found = 1;
                res->new_chunk = e_new;
                return LVMCORE_OK;
            }
        }
    }
}

/*
 * Append exception (old_chunk -> free_data_chunk) at (free_area, free_idx).
 * Zeroes the next area's exception table chunk when filling the last slot of an area.
 */
SNAP_NOINLINE static lvmcore_status snap_record_exception(const lvmcore_rw_source *cow,
                                                          const lvmcore_snap_header *hdr,
                                                          const snap_lookup_res *slot,
                                                          uint64_t old_chunk,
                                                          uint8_t *table_buf,
                                                          size_t table_buf_cap) {
    uint64_t chunk_bytes = (uint64_t)hdr->chunk_size_sectors * 512ULL;
    if (chunk_bytes > (uint64_t)table_buf_cap) return LVMCORE_ERR_SCRATCH_EXHAUSTED;
    uint64_t total_cow_chunks = cow->size / chunk_bytes;
    uint64_t exc_per_area = chunk_bytes / 16ULL;
    uint64_t stride = exc_per_area + 1ULL;
    if (slot->free_area > (UINT64_MAX - 1ULL) / stride) return LVMCORE_ERR_OVERFLOW;
    uint64_t tbl_chunk = 1ULL + slot->free_area * stride;

    /* If we are filling the final slot of this area, zero the next area's table chunk first. */
    if (slot->free_idx + 1ULL == exc_per_area && slot->free_area < (UINT64_MAX - 1ULL) / stride) {
        uint64_t next_tbl = 1ULL + (slot->free_area + 1ULL) * stride;
        if (next_tbl < total_cow_chunks) {
            sn_zero(table_buf, (size_t)chunk_bytes);
            if (cow->write(cow->impl, next_tbl * chunk_bytes, table_buf, (size_t)chunk_bytes) != 0) {
                return LVMCORE_ERR_IO;
            }
        }
    }

    if (cow->read(cow->impl, tbl_chunk * chunk_bytes, table_buf, (size_t)chunk_bytes) != 0) {
        return LVMCORE_ERR_IO;
    }
    sn_put_le64(table_buf + slot->free_idx * 16ULL, old_chunk);
    sn_put_le64(table_buf + slot->free_idx * 16ULL + 8ULL, slot->free_data_chunk);
    if (slot->free_idx + 1ULL < exc_per_area) {
        sn_put_le64(table_buf + (slot->free_idx + 1ULL) * 16ULL, 0ULL);
        sn_put_le64(table_buf + (slot->free_idx + 1ULL) * 16ULL + 8ULL, 0ULL);
    }
    if (cow->write(cow->impl, tbl_chunk * chunk_bytes, table_buf, (size_t)chunk_bytes) != 0) {
        return LVMCORE_ERR_IO;
    }
    return LVMCORE_OK;
}

SNAP_NOINLINE lvmcore_status lvmcore_snap_read(const lvmcore_rw_source *origin,
                                               const lvmcore_rw_source *cow,
                                               uint8_t *table_buf,
                                               size_t table_buf_cap,
                                               uint64_t offset,
                                               void *dst,
                                               size_t len) {
    if (!origin || !origin->read || !cow || !cow->read || !table_buf || (!dst && len > 0)) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (len == 0) return LVMCORE_OK;
    if (offset > origin->size || (uint64_t)len > origin->size - offset) {
        return LVMCORE_ERR_INVALID_ARG;
    }

    lvmcore_snap_header hdr;
    lvmcore_status st = lvmcore_snap_read_header(cow, &hdr);
    if (st != LVMCORE_OK) return st;
    if (hdr.valid != 1u) return LVMCORE_ERR_IO;

    uint64_t chunk_bytes = (uint64_t)hdr.chunk_size_sectors * 512ULL;
    uint8_t *out = (uint8_t *)dst;
    uint64_t cur = offset;
    size_t rem = len;

    while (rem > 0) {
        uint64_t chunk = cur / chunk_bytes;
        uint64_t intra = cur % chunk_bytes;
        uint64_t avail = chunk_bytes - intra;
        size_t step = ((uint64_t)rem < avail) ? rem : (size_t)avail;

        snap_lookup_res res;
        st = snap_scan_exceptions(cow, &hdr, chunk, table_buf, table_buf_cap, &res);
        if (st != LVMCORE_OK) return st;

        if (res.found) {
            uint64_t cow_off = res.new_chunk * chunk_bytes + intra;
            if (cow_off > cow->size || (uint64_t)step > cow->size - cow_off) {
                return LVMCORE_ERR_CORRUPT_METADATA;
            }
            if (cow->read(cow->impl, cow_off, out, step) != 0) return LVMCORE_ERR_IO;
        } else {
            if (origin->read(origin->impl, cur, out, step) != 0) return LVMCORE_ERR_IO;
        }

        cur += step;
        out += step;
        rem -= step;
    }
    return LVMCORE_OK;
}

SNAP_NOINLINE lvmcore_status lvmcore_snap_write(const lvmcore_rw_source *origin,
                                                const lvmcore_rw_source *cow,
                                                uint8_t *chunk_buf,
                                                uint8_t *table_buf,
                                                size_t buf_cap,
                                                uint64_t offset,
                                                const void *src,
                                                size_t len) {
    if (!origin || !origin->read || !cow || !cow->read || !cow->write ||
        !chunk_buf || !table_buf || (!src && len > 0)) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (len == 0) return LVMCORE_OK;
    if (offset > origin->size || (uint64_t)len > origin->size - offset) {
        return LVMCORE_ERR_INVALID_ARG;
    }

    lvmcore_snap_header hdr;
    lvmcore_status st = lvmcore_snap_read_header(cow, &hdr);
    if (st != LVMCORE_OK) return st;
    if (hdr.valid != 1u) return LVMCORE_ERR_IO;

    uint64_t chunk_bytes = (uint64_t)hdr.chunk_size_sectors * 512ULL;
    if (chunk_bytes > (uint64_t)buf_cap) return LVMCORE_ERR_SCRATCH_EXHAUSTED;

    const uint8_t *in = (const uint8_t *)src;
    uint64_t cur = offset;
    size_t rem = len;

    while (rem > 0) {
        uint64_t chunk = cur / chunk_bytes;
        uint64_t intra = cur % chunk_bytes;
        uint64_t avail = chunk_bytes - intra;
        size_t step = ((uint64_t)rem < avail) ? rem : (size_t)avail;

        snap_lookup_res res;
        st = snap_scan_exceptions(cow, &hdr, chunk, table_buf, buf_cap, &res);
        if (st != LVMCORE_OK) return st;

        if (res.found) {
            uint64_t cow_off = res.new_chunk * chunk_bytes + intra;
            if (cow_off > cow->size || (uint64_t)step > cow->size - cow_off) {
                return LVMCORE_ERR_CORRUPT_METADATA;
            }
            if (cow->write(cow->impl, cow_off, in, step) != 0) return LVMCORE_ERR_IO;
        } else {
            if (res.cow_full) {
                (void)snap_invalidate(cow, &hdr);
                return LVMCORE_ERR_IO;
            }
            uint64_t orig_chunk_off = chunk * chunk_bytes;
            size_t orig_chunk_len = (size_t)chunk_bytes;
            if ((uint64_t)orig_chunk_len > origin->size - orig_chunk_off) {
                sn_zero(chunk_buf, (size_t)chunk_bytes);
                orig_chunk_len = (size_t)(origin->size - orig_chunk_off);
            }
            if (step < (size_t)chunk_bytes) {
                if (origin->read(origin->impl, orig_chunk_off, chunk_buf, orig_chunk_len) != 0) {
                    return LVMCORE_ERR_IO;
                }
            }
            sn_memcpy(chunk_buf + intra, in, step);
            if (cow->write(cow->impl, res.free_data_chunk * chunk_bytes, chunk_buf,
                           (size_t)chunk_bytes) != 0) {
                return LVMCORE_ERR_IO;
            }
            st = snap_record_exception(cow, &hdr, &res, chunk, table_buf, buf_cap);
            if (st != LVMCORE_OK) return st;
        }

        cur += step;
        in += step;
        rem -= step;
    }
    return LVMCORE_OK;
}

SNAP_NOINLINE static lvmcore_status snap_origin_preserve_chunk(const lvmcore_rw_source *origin,
                                                               const lvmcore_rw_source *cow,
                                                               uint64_t chunk_start_byte,
                                                               uint64_t chunk_end_byte,
                                                               uint8_t *chunk_buf,
                                                               uint8_t *table_buf,
                                                               size_t buf_cap) {
    lvmcore_snap_header hdr;
    lvmcore_status st = lvmcore_snap_read_header(cow, &hdr);
    if (st == LVMCORE_OK && hdr.valid == 0u) {
        return LVMCORE_OK; /* Already invalid snapshot does not block origin writes. */
    }
    if (st != LVMCORE_OK) return st;
    if (hdr.valid != 1u) return LVMCORE_ERR_CORRUPT_METADATA;
    uint64_t chunk_bytes = (uint64_t)hdr.chunk_size_sectors * 512ULL;
    if (chunk_bytes > (uint64_t)buf_cap) return LVMCORE_ERR_SCRATCH_EXHAUSTED;

    uint64_t first_chunk = chunk_start_byte / chunk_bytes;
    uint64_t last_chunk = (chunk_end_byte - 1ULL) / chunk_bytes;
    for (uint64_t c = first_chunk; c <= last_chunk; c++) {
        snap_lookup_res res;
        st = snap_scan_exceptions(cow, &hdr, c, table_buf, buf_cap, &res);
        if (st != LVMCORE_OK) return st;
        if (res.found) continue;
        if (res.cow_full) {
            (void)snap_invalidate(cow, &hdr);
            return LVMCORE_OK;
        }
        uint64_t orig_off = c * chunk_bytes;
        size_t orig_len = (size_t)chunk_bytes;
        if ((uint64_t)orig_len > origin->size - orig_off) {
            sn_zero(chunk_buf, (size_t)chunk_bytes);
            orig_len = (size_t)(origin->size - orig_off);
        }
        if (origin->read(origin->impl, orig_off, chunk_buf, orig_len) != 0) {
            return LVMCORE_ERR_IO;
        }
        if (cow->write(cow->impl, res.free_data_chunk * chunk_bytes, chunk_buf,
                       (size_t)chunk_bytes) != 0) {
            return LVMCORE_ERR_IO;
        }
        st = snap_record_exception(cow, &hdr, &res, c, table_buf, buf_cap);
        if (st != LVMCORE_OK) return st;
    }
    return LVMCORE_OK;
}

SNAP_NOINLINE lvmcore_status lvmcore_snap_origin_preserve_one(const lvmcore_rw_source *origin,
                                                              const lvmcore_rw_source *cow,
                                                              uint8_t *chunk_buf,
                                                              uint8_t *table_buf,
                                                              size_t buf_cap,
                                                              uint64_t offset,
                                                              size_t len) {
    if (!origin || !origin->read || !cow || !chunk_buf || !table_buf) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (len == 0) return LVMCORE_OK;
    if (offset > origin->size || (uint64_t)len > origin->size - offset) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    return snap_origin_preserve_chunk(origin, cow, offset, offset + (uint64_t)len,
                                      chunk_buf, table_buf, buf_cap);
}

SNAP_NOINLINE lvmcore_status lvmcore_snap_origin_write(const lvmcore_rw_source *origin,
                                                       const lvmcore_rw_source *cows,
                                                       size_t cow_count,
                                                       uint8_t *chunk_buf,
                                                       uint8_t *table_buf,
                                                       size_t buf_cap,
                                                       uint64_t offset,
                                                       const void *src,
                                                       size_t len) {
    if (!origin || !origin->read || !origin->write || (!src && len > 0)) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (len == 0) return LVMCORE_OK;
    if (offset > origin->size || (uint64_t)len > origin->size - offset) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (cow_count > 0 && (!cows || !chunk_buf || !table_buf)) {
        return LVMCORE_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < cow_count; i++) {
        lvmcore_status st = snap_origin_preserve_chunk(
            origin, &cows[i], offset, offset + (uint64_t)len,
            chunk_buf, table_buf, buf_cap);
        if (st != LVMCORE_OK) return st;
    }

    return (origin->write(origin->impl, offset, src, len) == 0) ? LVMCORE_OK : LVMCORE_ERR_IO;
}
