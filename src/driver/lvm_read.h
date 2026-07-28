/*
 * lvm_read.h — embedded, freestanding LVM2 read path for winlvm.sys.
 *
 * Why this exists separately from src/lvmfmt (C++): the C++ library uses
 * std::string/vector/optional throughout its public API, and MSVC STL
 * cannot be linked into a WDM kernel driver (it hard-pulls the UCRT and
 * requires _HAS_EXCEPTIONS=1 + __CxxFrameHandler3, none of which ntoskrnl
 * provides). winmd and WinBtrfs both maintain separate user-mode and
 * kernel-mode parsers for the same on-disk format; we do the same.
 *
 * This module implements just the subset the driver needs:
 *
 *   1. LABELONE discovery + PV header parse (data-area, metadata-area lists)
 *   2. mda_header parse + CRC validation
 *   3. ring-buffer text extraction (first/wrapped chunks) + CRC validation
 *   4. a restricted text-config parser yielding one VolumeGroup with its PVs
 *      and LVs; unknown segment types are recorded, the LV is marked non-mappable
 *   5. single-PV linear / striped(stripe_count==1) extent -> target map
 *
 * It allocates nothing itself; callers hand it a winlvm_byte_source and
 * fixed-size output buffers. Everything intended for nonpaged pool is
 * allocated by the driver, not here.
 *
 * The behaviour of every function here MUST match the C++ lvmfmt library
 * to the same ground truth — verified by tests/test_driver_parser.c against
 * the committed corpus/expected/dmsetup-table.txt oracles.
 */
#ifndef WINLVM_DRIVER_LVM_READ_H
#define WINLVM_DRIVER_LVM_READ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pool tag driver side uses for metadata buffers. mirrored from the C++
 * library's own kPoolTag constant. */
#define WINLVM_LVM_TAG ((unsigned long)'WMLV')

/* A read-only, randomly-addressable byte source. The driver backs this with
 * IRP_MJ_READ down to the physical disk; the user-mode test backs it with
 * CreateFile/ReadFile. read() returns the number of bytes read, or 0 on
 * short read / failure. */
typedef struct winlvm_byte_source {
    /* opaque handle the impl dereferences in read() */
    void *impl;
    uint64_t size;                       /* total bytes available */
    size_t (*read)(void *impl, uint64_t offset, void *buf, size_t len);
} winlvm_byte_source;

/* On-disk size limits we refuse to exceed. LVM's default MDA is 1 MiB. */
#define WINLVM_MAX_MD_AREAS    64
#define WINLVM_MAX_METADATA_BYTES (1u * 1024 * 1024)
#define WINLVM_LABEL_SCAN_BYTES  8192
#define WINLVM_SECTOR_BYTES       512

/* Label-layer structures. */
typedef struct {
    uint64_t offset;     /* on-disk metadata-area offset, bytes */
    uint64_t size;       /* on-disk metadata-area size, bytes */
} winlvm_disk_area;

typedef struct {
    char     pv_uuid[33];            /* raw, 32 chars + NUL */
    uint64_t device_size;           /* bytes */
    /* Area lists are null-terminated in memory by an (0,0) sentinel entry. */
    winlvm_disk_area data_areas[WINLVM_MAX_MD_AREAS + 1];
    winlvm_disk_area metadata_areas[WINLVM_MAX_MD_AREAS + 1];
} winlvm_pv_label;

/* Committed metadata area header (raw_locn slot 0). */
typedef struct {
    uint64_t commit_offset;   /* absolute on-disk byte offset of the text */
    uint64_t commit_size;     /* text length in bytes, including trailing NUL */
    uint32_t commit_checksum; /* LVM CRC32 over the text bytes */
} winlvm_mda_header;

/* LV features actually used by the read path. */
typedef enum {
    WINLVM_SEG_STRIPED = 0,   /* stripe_count==1 is plain linear */
    WINLVM_SEG_OTHER          /* thin/raid/snapshot/... recorded, LV non-mappable */
} winlvm_seg_type;

/* One stripe inside a segment: target PV key + first extent on that PV. */
typedef struct {
    char     pv_key[64];   /* e.g. "pv0" */
    uint64_t start_extent; /* in extents, on the named PV */
} winlvm_stripe;

typedef struct {
    uint64_t      start_extent;   /* in LV-space extents */
    uint64_t      extent_count;   /* number of extents in this segment */
    winlvm_seg_type type;
    uint64_t      stripe_count;
    winlvm_stripe stripes[8];     /* LVM supports up to a handful in practice */
    size_t        stripe_count_used;
} winlvm_segment;

/* Per-LV in-memory record. */
typedef struct {
    char     name[64];
    char     id[33];
    int      visible;        /* status contains VISIBLE */
    int      mappable;       /* 0 if any segment is not WINLVM_SEG_STRIPED */
    char     unsupported_reason[64];
    winlvm_segment segments[64];
    size_t   segment_count;
} winlvm_logical_volume;

/* Per-PV record. */
typedef struct {
    char     key[64];         /* "pv0" */
    char     id[33];          /* dashed? no — raw UUID like the label */
    char     device[128];     /* "/dev/loopN" in dumps, unused at runtime */
    uint64_t pe_start;        /* sectors */
    uint64_t pe_count;
} winlvm_physical_volume;

/* One volume group, sized for USB thumb drives in practice — a handful of
 * PVs and LVs. The driver refuses pathological counts. */
typedef struct {
    char     name[64];
    char     id[33];
    char     system_id[64];
    uint64_t seqno;
    uint64_t extent_size_sectors;
    winlvm_physical_volume pvs[16];
    size_t   pv_count;
    winlvm_logical_volume lvs[64];
    size_t   lv_count;
} winlvm_volume_group;

/* Error reporting. 0 = OK; non-zero = failure with a short message. */
typedef struct {
    int  failed;
    char message[160];
} winlvm_lvm_err;

/* The high-level entry point: read a VG from a byte source. Returns 0 on
 * success and fills *vg. The caller owns *vg (typically a nonpaged struct). */
int winlvm_read_vg(winlvm_byte_source *src,
                   winlvm_volume_group *vg,
                   winlvm_lvm_err *err);

/* LVM's CRC32. Exported so tests can hit it directly. Poly 0xedb88320,
 * init 0xf597a6cf, reflected, no final XOR. NOT the same as zlib crc32. */
uint32_t winlvm_lvm_crc32(const void *data, size_t len);

#ifdef __cplusplus
}
#endif
#endif
