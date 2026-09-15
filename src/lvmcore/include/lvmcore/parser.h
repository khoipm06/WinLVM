/*
 * parser.h — freestanding LVM2 metadata parser.
 *
 * Implements:
 *   1. LABELONE scanner + PV header parse
 *   2. mda_header parse + CRC validation
 *   3. Ring-buffer text extraction (with wrap around ring boundary)
 *   4. Iterative text metadata parser constructing canonical VolumeGroup
 *   5. Read semantics: slot 0 vs slot 1 PRECOMMITTED and multi-PV max-seqno reconciliation.
 *
 * All allocations occur in the caller-supplied lvmcore_arena.
 * Strictly non-recursive; per-function stack frames <= 512 bytes.
 */
#ifndef LVMCORE_PARSER_H
#define LVMCORE_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "lvmcore/types.h"
#include "lvmcore/arena.h"
#include "lvmcore/crc32.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Step 1: Scan for LABELONE and parse PV header.
 * Allocates data_areas and metadata_areas arrays in `arena`.
 */
lvmcore_status lvmcore_read_pv_label(lvmcore_byte_source *src,
                                     lvmcore_arena *arena,
                                     lvmcore_pv_label *label_out,
                                     lvmcore_err *err);

/*
 * Step 2: Read and validate MDA header for a given metadata area.
 * Allocates temporary sector buffer in `arena`.
 */
lvmcore_status lvmcore_read_mda_header(lvmcore_byte_source *src,
                                       const lvmcore_disk_area *area,
                                       lvmcore_arena *arena,
                                       lvmcore_mda_header *mda_out,
                                       lvmcore_err *err);

/*
 * Step 3: Extract metadata text from MDA ring-buffer following wrap.
 * If flags contain LVMCORE_READ_FLAG_PRECOMMITTED, slot 1 (precommitted)
 * is selected if present and valid.
 * Output text is allocated in `arena` and NUL-terminated.
 */
lvmcore_status lvmcore_read_metadata_text(lvmcore_byte_source *src,
                                          const lvmcore_disk_area *area,
                                          const lvmcore_mda_header *mda,
                                          uint32_t flags,
                                          lvmcore_arena *arena,
                                          char **text_out,
                                          size_t *text_len_out,
                                          bool *is_precommitted_out,
                                          lvmcore_err *err);

/*
 * Step 4: Parse LVM2 text metadata into a VolumeGroup structure.
 * Allocates PV, LV, segment, and stripe arrays in `arena`.
 */
lvmcore_status lvmcore_parse_vg_text(const char *text,
                                     size_t text_len,
                                     lvmcore_arena *arena,
                                     lvmcore_volume_group *vg_out,
                                     lvmcore_err *err);

/*
 * High-level entry point: Read Volume Group from a single PV byte source.
 * Discovers label, iterates metadata areas, extracts text, parses VG,
 * and validates that label PV UUID matches metadata PV UUID.
 */
lvmcore_status lvmcore_read_vg(lvmcore_byte_source *src,
                               uint32_t flags,
                               lvmcore_arena *arena,
                               lvmcore_volume_group *vg_out,
                               lvmcore_err *err);

/*
 * Multi-PV entry point / max-seqno reconciliation:
 * Given multiple PV byte sources, reads the VG from each, reconciles across
 * PVs (max seqno wins; flags LVMCORE_VG_FLAG_SEQNO_MISMATCH if seqnos differ),
 * and returns the reconciled volume group.
 */
lvmcore_status lvmcore_read_vg_multi(lvmcore_byte_source **sources,
                                     size_t source_count,
                                     uint32_t flags,
                                     lvmcore_arena *arena,
                                     lvmcore_volume_group *vg_out,
                                     lvmcore_err *err);

/* Helper to convert status code to human-readable string. */
const char *lvmcore_status_string(lvmcore_status status);

/* Helper to find a PV by key e.g. "pv0" */
const lvmcore_physical_volume *lvmcore_vg_find_pv(const lvmcore_volume_group *vg,
                                                  const char *key);

/* Helper to find a PV by raw UUID */
const lvmcore_physical_volume *lvmcore_vg_find_pv_by_uuid(const lvmcore_volume_group *vg,
                                                          const char *uuid);

/* Helper to find an LV by name */
const lvmcore_logical_volume *lvmcore_vg_find_lv(const lvmcore_volume_group *vg,
                                                 const char *name);

/* Helper to compute total extents in an LV */
uint64_t lvmcore_lv_extent_count(const lvmcore_logical_volume *lv);

#ifdef __cplusplus
}
#endif

#endif /* LVMCORE_PARSER_H */
