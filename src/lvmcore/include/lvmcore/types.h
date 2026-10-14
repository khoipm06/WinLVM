/*
 * types.h — canonical freestanding C11 types for lvmcore.
 *
 * All structures in this file are platform-agnostic, freestanding C11,
 * usable in both Windows kernel-mode (winlvm.sys) and user-mode tools.
 * Zero dynamic memory allocation (no malloc/free, no ExAllocatePool).
 */
#ifndef LVMCORE_TYPES_H
#define LVMCORE_TYPES_H

#include <stddef.h>
#if defined(_KERNEL_MODE) && defined(_MSC_VER)
#ifndef WINLVM_KM_STDINT_DEFINED
#define WINLVM_KM_STDINT_DEFINED
typedef signed __int8    int8_t;
typedef signed __int16   int16_t;
typedef signed __int32   int32_t;
typedef signed __int64   int64_t;
typedef unsigned __int8  uint8_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int64 uint64_t;
#ifndef _UINTPTR_T_DEFINED
#define _UINTPTR_T_DEFINED
#if defined(_WIN64)
typedef unsigned __int64 uintptr_t;
#else
typedef unsigned __int32 uintptr_t;
#endif
#endif
#ifndef _INTPTR_T_DEFINED
#define _INTPTR_T_DEFINED
#if defined(_WIN64)
typedef signed __int64   intptr_t;
#else
typedef signed __int32   intptr_t;
#endif
#endif
#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffffu
#endif
#ifndef UINT64_MAX
#define UINT64_MAX 0xffffffffffffffffui64
#endif
#ifndef SIZE_MAX
#if defined(_WIN64)
#define SIZE_MAX   0xffffffffffffffffui64
#else
#define SIZE_MAX   0xffffffffu
#endif
#endif
#endif /* WINLVM_KM_STDINT_DEFINED */
#if !defined(__cplusplus) && !defined(__bool_true_false_are_defined) && \
    (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
#define __bool_true_false_are_defined 1
#if _MSC_VER >= 1800
#define bool _Bool
#else
typedef unsigned char bool;
#endif
#define true  1
#define false 0
#endif
#else
#include <stdint.h>
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Pool tag mirror for metadata buffers. */
#define LVMCORE_TAG ((uint32_t)'WMLV')

/* Size constraints on on-disk structures. */
#define LVMCORE_MAX_MD_AREAS         64
#define LVMCORE_MAX_METADATA_BYTES   (1u * 1024 * 1024)
#define LVMCORE_LABEL_SCAN_BYTES     8192
#define LVMCORE_SECTOR_BYTES         512
#define LVMCORE_LABEL_SIZE           512
#define LVMCORE_MDA_HEADER_SIZE      512

/* Status codes for lvmcore operations. */
typedef enum lvmcore_status {
    LVMCORE_OK                    = 0,
    LVMCORE_ERR_INVALID_ARG       = 1,
    LVMCORE_ERR_NO_LABEL          = 2,
    LVMCORE_ERR_CORRUPT_LABEL     = 3,
    LVMCORE_ERR_CORRUPT_MDA       = 4,
    LVMCORE_ERR_NO_VALID_MDA      = 5,
    LVMCORE_ERR_CORRUPT_METADATA  = 6,
    LVMCORE_ERR_NO_VG             = 7,
    LVMCORE_ERR_SCRATCH_EXHAUSTED = 8,
    LVMCORE_ERR_UNSUPPORTED       = 9,
    LVMCORE_ERR_IO                = 10,
    LVMCORE_ERR_OVERFLOW          = 11
} lvmcore_status;

/* Error reporting container. */
typedef struct lvmcore_err {
    lvmcore_status status;
    int            failed;
    char           message[160];
} lvmcore_err;

/* Read-only, randomly-addressable byte source. */
typedef struct lvmcore_byte_source {
    void   *impl;
    uint64_t size;
    size_t (*read)(void *impl, uint64_t offset, void *buf, size_t len);
} lvmcore_byte_source;

/* Disk area offset and size in bytes, relative to PV start. */
typedef struct lvmcore_disk_area {
    uint64_t offset;
    uint64_t size;
} lvmcore_disk_area;

/* Parsed PV label and header layout. */
typedef struct lvmcore_pv_label {
    uint64_t           label_offset;
    uint64_t           label_sector;
    char               pv_uuid[33]; /* raw 32 characters + NUL */
    uint64_t           device_size;
    size_t             data_area_count;
    lvmcore_disk_area *data_areas;
    size_t             metadata_area_count;
    lvmcore_disk_area *metadata_areas;
} lvmcore_pv_label;

/* Raw location descriptor in mda_header. */
#define LVMCORE_RAW_LOCN_IGNORED 0x00000001u

typedef struct lvmcore_raw_locn {
    uint64_t offset; /* relative to MDA start */
    uint64_t size;   /* length in bytes, including trailing NUL */
    uint32_t checksum;
    uint32_t flags;
} lvmcore_raw_locn;

/* Parsed metadata area header. */
typedef struct lvmcore_mda_header {
    uint64_t         start; /* absolute start byte on PV */
    uint64_t         size;  /* total metadata area size */
    uint32_t         version;
    /* Slot 0 is committed metadata; slot 1 is precommitted metadata. */
    lvmcore_raw_locn raw_locns[2];
} lvmcore_mda_header;

/* Segment type classification. */
typedef enum lvmcore_seg_type {
    LVMCORE_SEG_STRIPED = 0, /* stripe_count == 1 represents linear */
    LVMCORE_SEG_OTHER   = 1  /* thin, raid, snapshot, etc. */
} lvmcore_seg_type;

/* One stripe in a segment: target key ("pv0") + start extent on target. */
typedef struct lvmcore_stripe {
    char     pv_key[64];
    uint64_t start_extent;
} lvmcore_stripe;

/* One segment within a logical volume. */
typedef struct lvmcore_segment {
    uint64_t         start_extent;
    uint64_t         extent_count;
    lvmcore_seg_type type;
    char             seg_type_name[32];
    uint64_t         stripe_count;
    uint64_t         stripe_size_sectors;
    size_t           stripe_count_used;
    lvmcore_stripe  *stripes;
    char             origin[64];
    int              stripe_overflow;
    /*
     * Raw pass-through for non-striped segment types (thin, raid, snapshot):
     * verbatim "key = value" lines from the on-disk text, arena-owned,
     * appended in on-disk order. Re-emitted unchanged by the serializer so
     * LVM2 (vgcfgrestore) accepts segments the canonical model cannot map.
     */
    char            *raw_body;
    size_t           raw_body_len;
} lvmcore_segment;

/* One logical volume record. */
typedef struct lvmcore_logical_volume {
    char             name[64];
    char             id[33]; /* raw 32 characters + NUL */
    int              visible;
    int              read_only; /* 1 if LV status lacks "WRITE", 0 if "WRITE" is present */
    int              mappable;
    int              internal;
    char             unsupported_reason[64];
    size_t           segment_count;
    uint64_t         declared_segment_count;
    lvmcore_segment *segments;
    int              segment_overflow;
} lvmcore_logical_volume;

/* One physical volume record in a volume group. */
typedef struct lvmcore_physical_volume {
    char     key[64]; /* e.g. "pv0" */
    char     id[33];  /* raw 32 characters + NUL */
    char     device[128];
    uint64_t pe_start; /* sectors */
    uint64_t pe_count;
    uint64_t dev_size_sectors;
    int      missing; /* 1 if PV status/flags contains "MISSING", 0 otherwise */
} lvmcore_physical_volume;

/* Volume group flags. */
#define LVMCORE_VG_FLAG_PRECOMMITTED (1u << 0)
#define LVMCORE_VG_FLAG_SEQNO_MISMATCH (1u << 1)

/* One volume group record. */
typedef struct lvmcore_volume_group {
    char                     name[64];
    char                     id[33];
    char                     system_id[64];
    uint64_t                 seqno;
    uint64_t                 extent_size_sectors;
    uint32_t                 flags;
    int                      read_only; /* 1 if VG status lacks "WRITE", 0 if "WRITE" is present */
    size_t                   pv_count;
    lvmcore_physical_volume *pvs;
    size_t                   lv_count;
    lvmcore_logical_volume  *lvs;
    int                      pv_overflow;
    int                      lv_overflow;
} lvmcore_volume_group;

/* Parser options. */
#define LVMCORE_READ_FLAG_DEFAULT      0x00000000u
#define LVMCORE_READ_FLAG_PRECOMMITTED 0x00000001u

#ifdef __cplusplus
}
#endif

#endif /* LVMCORE_TYPES_H */
