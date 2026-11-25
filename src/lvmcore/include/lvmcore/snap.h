/*
 * snap.h — Freestanding C11 Linux dm-snapshot & dm-snapshot-origin engine.
 *
 * Implements the persistent exception store (magic 0x70416e53, version 1):
 *   - Chunk 0: disk_header (magic, valid, version, chunk_size in 512B sectors)
 *   - Area a: exception table at chunk 1 + a * (exceptions_per_area + 1)
 *             holding (uint64_t old_chunk, uint64_t new_chunk) terminated by
 *             new_chunk == 0; data chunks at 2 + a * (exceptions_per_area + 1) + d.
 *
 * Zero dynamic allocation; caller supplies scratch buffers; stack <= 512B.
 */
#ifndef LVMCORE_SNAP_H
#define LVMCORE_SNAP_H

#include "lvmcore/block_io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LVMCORE_SNAP_MAGIC   0x70416e53u /* "SnAp" little-endian */
#define LVMCORE_SNAP_VERSION 1u

typedef struct lvmcore_snap_header {
    uint32_t magic;
    uint32_t valid;
    uint32_t version;
    uint32_t chunk_size_sectors;
} lvmcore_snap_header;

/* Read and validate persistent snapshot header from COW chunk 0. */
lvmcore_status lvmcore_snap_read_header(const lvmcore_rw_source *cow,
                                        lvmcore_snap_header *hdr_out);

/* Read [offset, offset + len) from a snapshot LV (overlaying COW onto origin). */
lvmcore_status lvmcore_snap_read(const lvmcore_rw_source *origin,
                                 const lvmcore_rw_source *cow,
                                 uint8_t *table_buf,
                                 size_t table_buf_cap,
                                 uint64_t offset,
                                 void *dst,
                                 size_t len);

/*
 * Write [offset, offset + len) to a snapshot LV.
 * Allocates COW chunks for unmapped target chunks; if COW runs out of space,
 * marks the COW header valid=0 and returns LVMCORE_ERR_IO.
 */
lvmcore_status lvmcore_snap_write(const lvmcore_rw_source *origin,
                                  const lvmcore_rw_source *cow,
                                  uint8_t *chunk_buf,
                                  uint8_t *table_buf,
                                  size_t buf_cap,
                                  uint64_t offset,
                                  const void *src,
                                  size_t len);

/*
 * Preserve pre-write origin chunks [offset, offset + len) into one attached
 * snapshot COW store without mutating the origin LV.
 */
lvmcore_status lvmcore_snap_origin_preserve_one(const lvmcore_rw_source *origin,
                                                const lvmcore_rw_source *cow,
                                                uint8_t *chunk_buf,
                                                uint8_t *table_buf,
                                                size_t buf_cap,
                                                uint64_t offset,
                                                size_t len);

/*
 * Write [offset, offset + len) to a snapshot-origin LV.
 * Preserves pre-write origin chunks into every valid attached snapshot's COW
 * store before mutating the origin LV. Any snapshot whose COW fills up is
 * cleanly invalidated (valid=0) while the origin write still completes.
 */
lvmcore_status lvmcore_snap_origin_write(const lvmcore_rw_source *origin,
                                         const lvmcore_rw_source *cows,
                                         size_t cow_count,
                                         uint8_t *chunk_buf,
                                         uint8_t *table_buf,
                                         size_t buf_cap,
                                         uint64_t offset,
                                         const void *src,
                                         size_t len);

#ifdef __cplusplus
}
#endif

#endif /* LVMCORE_SNAP_H */
