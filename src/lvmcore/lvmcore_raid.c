/*
 * lvmcore_raid.c — Freestanding C11 raid1 & mirror block engine.
 */
#include "lvmcore/raid.h"

static uint32_t r_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

lvmcore_status lvmcore_raid1_read(const lvmcore_rw_source *legs,
                                  const int *leg_present,
                                  size_t leg_count,
                                  uint64_t offset,
                                  void *dst,
                                  size_t len) {
    if (!legs || !leg_present || leg_count == 0 || (!dst && len > 0)) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (len == 0) return LVMCORE_OK;

    for (size_t i = 0; i < leg_count; i++) {
        if (!leg_present[i] || !legs[i].read) continue;
        if (offset > legs[i].size || (uint64_t)len > legs[i].size - offset) continue;
        if (legs[i].read(legs[i].impl, offset, dst, len) == 0) {
            return LVMCORE_OK;
        }
    }
    return LVMCORE_ERR_IO;
}

lvmcore_status lvmcore_raid1_write(const lvmcore_rw_source *legs,
                                   const int *leg_present,
                                   size_t leg_count,
                                   uint64_t offset,
                                   const void *src,
                                   size_t len) {
    if (!legs || !leg_present || leg_count == 0 || (!src && len > 0)) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (len == 0) return LVMCORE_OK;

    /* Fail closed before writing any leg if any leg is missing or out-of-bounds. */
    for (size_t i = 0; i < leg_count; i++) {
        if (!leg_present[i] || !legs[i].write) {
            return LVMCORE_ERR_IO;
        }
        if (offset > legs[i].size || (uint64_t)len > legs[i].size - offset) {
            return LVMCORE_ERR_IO;
        }
    }

    for (size_t i = 0; i < leg_count; i++) {
        if (legs[i].write(legs[i].impl, offset, src, len) != 0) {
            return LVMCORE_ERR_IO;
        }
    }
    return LVMCORE_OK;
}

lvmcore_status lvmcore_raid_check_superblock(const lvmcore_rw_source *rmeta,
                                             uint8_t *sector_buf,
                                             size_t sector_buf_cap) {
    if (!rmeta || !rmeta->read || !sector_buf || sector_buf_cap < 512u) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    if (rmeta->size < 512u) return LVMCORE_ERR_IO;
    if (rmeta->read(rmeta->impl, 0, sector_buf, 512u) != 0) {
        return LVMCORE_ERR_IO;
    }
    if (r_le32(sector_buf) != LVMCORE_DMRAID_MAGIC) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }
    return LVMCORE_OK;
}
