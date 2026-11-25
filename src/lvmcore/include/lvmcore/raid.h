/*
 * raid.h — Freestanding C11 raid1 & mirror block engine.
 *
 * Reads select the first present healthy leg with automatic failover to
 * surviving legs when a PV is missing or returns an I/O error.
 * Writes mirror identically across all legs and fail closed (zero partial
 * writes) if any leg is missing, keeping dm-raid superblocks clean.
 */
#ifndef LVMCORE_RAID_H
#define LVMCORE_RAID_H

#include "lvmcore/block_io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LVMCORE_DMRAID_MAGIC 0x64526D44u /* "DmRd" little-endian */

/* Read [offset, offset + len) from raid1/mirror with automatic leg failover. */
lvmcore_status lvmcore_raid1_read(const lvmcore_rw_source *legs,
                                  const int *leg_present,
                                  size_t leg_count,
                                  uint64_t offset,
                                  void *dst,
                                  size_t len);

/*
 * Write [offset, offset + len) simultaneously across all raid1/mirror legs.
 * Fails closed with LVMCORE_ERR_IO if any leg is absent so legs never diverge.
 */
lvmcore_status lvmcore_raid1_write(const lvmcore_rw_source *legs,
                                   const int *leg_present,
                                   size_t leg_count,
                                   uint64_t offset,
                                   const void *src,
                                   size_t len);

/* Verify DmRd superblock magic on a raid1 _rmeta_N leg (sector 0). */
lvmcore_status lvmcore_raid_check_superblock(const lvmcore_rw_source *rmeta,
                                             uint8_t *sector_buf,
                                             size_t sector_buf_cap);

#ifdef __cplusplus
}
#endif

#endif /* LVMCORE_RAID_H */
