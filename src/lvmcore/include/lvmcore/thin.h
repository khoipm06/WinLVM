/*
 * thin.h — Freestanding C11 Linux dm-thin v2 on-disk block engine.
 *
 * Implements superblock validation, 2-level data_mapping B-tree lookup/update
 * with leaf/internal splits and shared-node CoW shadowing, 1-level
 * device_details lookup/update, data & metadata 2-bit space-map allocation
 * and refcounting (including >= 3 overflow refcount B-trees), and exact
 * Linux dm-persistent-data CRC32c checksums.
 *
 * Zero dynamic allocation; caller supplies scratch buffers; stack <= 512B.
 */
#ifndef LVMCORE_THIN_H
#define LVMCORE_THIN_H

#include "lvmcore/block_io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LVMCORE_THIN_META_BLOCK_SIZE     4096u
#define LVMCORE_THIN_SUPERBLOCK_MAGIC    27022010ULL
#define LVMCORE_THIN_VERSION             2u
#define LVMCORE_THIN_SCRATCH_META_BLOCKS 12u
#define LVMCORE_THIN_SCRATCH_META_BYTES  (LVMCORE_THIN_SCRATCH_META_BLOCKS * LVMCORE_THIN_META_BLOCK_SIZE)

typedef struct lvmcore_thin_sm_root {
    uint64_t nr_blocks;
    uint64_t nr_allocated;
    uint64_t bitmap_root;
    uint64_t ref_count_root;
} lvmcore_thin_sm_root;

typedef struct lvmcore_thin_superblock {
    uint32_t             flags;
    uint64_t             blocknr;
    uint64_t             magic;
    uint32_t             version;
    uint32_t             time;
    uint64_t             trans_id;
    uint64_t             metadata_snap;
    lvmcore_thin_sm_root data_sm;
    lvmcore_thin_sm_root meta_sm;
    uint64_t             data_mapping_root;
    uint64_t             device_details_root;
    uint32_t             data_block_size;     /* in 512-byte sectors */
    uint32_t             metadata_block_size; /* in 512-byte sectors (always 8) */
    uint64_t             metadata_nr_blocks;
} lvmcore_thin_superblock;

typedef struct lvmcore_thin_device_details {
    uint64_t mapped_blocks;
    uint64_t transaction_id;
    uint32_t creation_time;
    uint32_t snapshotted_time;
} lvmcore_thin_device_details;

/* Read and validate block 0 superblock from tmeta. meta_buf must be >= 4096 bytes. */
lvmcore_status lvmcore_thin_read_superblock(const lvmcore_rw_source *tmeta,
                                            uint8_t *meta_buf,
                                            lvmcore_thin_superblock *sb_out);

/*
 * Read [offset, offset + len) from thin device `dev_id`.
 * Unmapped virtual chunks return zeros; mapped chunks read from `tdata`.
 * `meta_scratch` must be at least LVMCORE_THIN_SCRATCH_META_BYTES (48 KiB).
 */
lvmcore_status lvmcore_thin_read(const lvmcore_rw_source *tmeta,
                                 const lvmcore_rw_source *tdata,
                                 uint64_t dev_id,
                                 uint8_t *meta_scratch,
                                 size_t meta_scratch_cap,
                                 uint64_t offset,
                                 void *dst,
                                 size_t len);

/*
 * Write [offset, offset + len) to thin device `dev_id`.
 * Handles in-place updates for unshared mapped chunks, new block allocation
 * for unmapped chunks, shared B-tree node shadowing, data block CoW for
 * snapshots, B-tree splits, space-map refcounts, and CRC32c updates.
 * `meta_scratch` must be >= LVMCORE_THIN_SCRATCH_META_BYTES (48 KiB).
 * `chunk_buf` must be >= (data_block_size * 512) bytes.
 */
lvmcore_status lvmcore_thin_write(const lvmcore_rw_source *tmeta,
                                  const lvmcore_rw_source *tdata,
                                  uint64_t dev_id,
                                  uint8_t *meta_scratch,
                                  size_t meta_scratch_cap,
                                  uint8_t *chunk_buf,
                                  size_t chunk_buf_cap,
                                  int zero_new_blocks,
                                  uint64_t offset,
                                  const void *src,
                                  size_t len);

#ifdef __cplusplus
}
#endif

#endif /* LVMCORE_THIN_H */
