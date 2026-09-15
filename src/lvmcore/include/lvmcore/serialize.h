/*
 * serialize.h — freestanding C11 LVM2 metadata serializer & commit engine.
 *
 * Provides:
 *   1. LVM2 text metadata serializer for volume groups.
 *   2. Circular MDA ring buffer placement engine (_next_rlocn_offset analog).
 *   3. Two-slot commit engine (precommit in slot 1, commit in slot 0).
 *   4. Raw MDA header binary serialization with CRC32 calculation.
 *
 * All functions are freestanding C11: zero dynamic memory allocation,
 * strict stack frame limits (<= 512 bytes), non-recursive, bounds-checked.
 */
#ifndef LVMCORE_SERIALIZE_H
#define LVMCORE_SERIALIZE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "lvmcore/types.h"
#include "lvmcore/crc32.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Aliases matching prompt and C11 naming conventions */
typedef lvmcore_volume_group lvmcore_vg_t;
typedef lvmcore_status       lvmcore_status_t;
typedef lvmcore_mda_header   lvmcore_mda_header_t;
typedef lvmcore_raw_locn     lvmcore_raw_locn_t;

/* Default alignment for metadata ring placement (512 bytes / 1 sector) */
#define LVMCORE_MDA_DEFAULT_ALIGNMENT 512u

/*
 * Serialize an in-memory volume group into LVM2 text metadata format.
 * Writes formatted text into caller-supplied out_buf (capacity out_cap).
 * The serialized text is NUL-terminated.
 *
 * On success, *out_len is set to total bytes written including the trailing NUL,
 * matching LVM2's rlocn.size convention.
 *
 * Returns:
 *   LVMCORE_OK on success.
 *   LVMCORE_ERR_INVALID_ARG if any required pointer is NULL.
 *   LVMCORE_ERR_OVERFLOW or LVMCORE_ERR_SCRATCH_EXHAUSTED if out_cap is insufficient.
 */
lvmcore_status lvmcore_serialize_vg(const lvmcore_volume_group *vg,
                                    char *out_buf,
                                    size_t out_cap,
                                    size_t *out_len);

/*
 * Compute next byte offset in the MDA ring buffer for writing new metadata.
 * Mirrors LVM2's _next_rlocn_offset algorithm:
 *   - Begins after the end of the previous committed metadata (slot 0),
 *     aligned to `alignment` (typically 512).
 *   - If no previous committed metadata exists, starts at LVMCORE_MDA_HEADER_SIZE.
 *   - If next offset or payload would exceed ring boundaries or leave fewer
 *     than `alignment` bytes at the end of the area, wraps back to
 *     LVMCORE_MDA_HEADER_SIZE.
 *   - Validates that the payload fits within the MDA maximum allowable size.
 *
 * Returns:
 *   LVMCORE_OK on success (*next_offset_out set).
 *   LVMCORE_ERR_INVALID_ARG on NULL pointer or invalid alignment / sizes.
 *   LVMCORE_ERR_OVERFLOW if payload_size exceeds the allowable metadata capacity.
 */
lvmcore_status lvmcore_mda_next_offset(const lvmcore_mda_header *mdah,
                                       uint64_t mda_size,
                                       uint64_t payload_size,
                                       uint64_t alignment,
                                       uint64_t *next_offset_out);

/*
 * Prepare updated MDA header for precommit (Step 1 of two-slot commit):
 *   - Slot 0 (committed) is preserved from cur_mdah->raw_locns[0].
 *   - Slot 1 (precommitted) is populated with (new_offset, new_size, new_crc, flags=0).
 *
 * cur_mdah and new_mdah may point to the same object.
 *
 * Returns:
 *   LVMCORE_OK on success.
 *   LVMCORE_ERR_INVALID_ARG on NULL pointer or invalid parameters.
 */
lvmcore_status lvmcore_mda_prepare_precommit(const lvmcore_mda_header *cur_mdah,
                                             uint64_t new_offset,
                                             uint64_t new_size,
                                             uint32_t new_crc,
                                             lvmcore_mda_header *new_mdah);

/*
 * Prepare updated MDA header for final commit (Step 2 of two-slot commit):
 *   - Slot 0 (committed) is updated with (new_offset, new_size, new_crc, flags=0).
 *   - Slot 1 (precommitted) is cleared (offset=0, size=0, checksum=0, flags=0).
 *
 * cur_mdah and new_mdah may point to the same object.
 *
 * Returns:
 *   LVMCORE_OK on success.
 *   LVMCORE_ERR_INVALID_ARG on NULL pointer or invalid parameters.
 */
lvmcore_status lvmcore_mda_prepare_commit(const lvmcore_mda_header *cur_mdah,
                                          uint64_t new_offset,
                                          uint64_t new_size,
                                          uint32_t new_crc,
                                          lvmcore_mda_header *new_mdah);

/*
 * Serialize an lvmcore_mda_header structure into on-disk 512-byte sector format:
 *   - Magic: " LVM2 x[5A%r0N*>" (16 bytes)
 *   - Version: 1 (uint32)
 *   - Start: mdah->start (uint64)
 *   - Size: mdah->size (uint64)
 *   - Slot 0 descriptor: offset (uint64), size (uint64), checksum (uint32), flags (uint32)
 *   - Slot 1 descriptor: offset (uint64), size (uint64), checksum (uint32), flags (uint32)
 *   - Padding: zeros up to 512 bytes
 *   - Checksum: CRC32 of bytes 4..511 placed at bytes 0..3 (little-endian uint32).
 *
 * Returns:
 *   LVMCORE_OK on success.
 *   LVMCORE_ERR_INVALID_ARG if sector_buf is NULL or sector_cap < 512.
 */
lvmcore_status lvmcore_mda_header_write(const lvmcore_mda_header *mdah,
                                        uint8_t *sector_buf,
                                        size_t sector_cap);

#ifdef __cplusplus
}
#endif

#endif /* LVMCORE_SERIALIZE_H */
