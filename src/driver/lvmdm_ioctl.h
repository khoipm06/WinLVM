#pragma once

#ifndef WINLVM_DRIVER_IOCTL_H
#define WINLVM_DRIVER_IOCTL_H

#if !defined(_KERNEL_MODE) && !defined(_NTDDK_)
#include <winioctl.h>
#include <winnt.h>
#endif

// ============================================================================
// IOCTL Definitions for winlvm Kernel Driver (lvmdm.sys)
// ============================================================================

#define WINLVM_DEVICE_NAME L"\\\\.\\lvmdm"
#define WINLVM_DEVICE_SYMLINK L"\\DosDevices\\lvmdm"
#define WINLVM_DEVICE_NAME_BASE L"lvmdm"

// Device control codes for LVM mapping operations
#define IOCTL_LVMDM_CREATE_MAPPING \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define IOCTL_LVMDM_DELETE_MAPPING \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define IOCTL_LVMDM_QUERY_MAPPING \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_DATA)

#define IOCTL_LVMDM_LIST_MAPPINGS \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_DATA)

// ============================================================================
// Mapping Table ABI Constants
//
// Keep these layout-compatible with src/lvmcore/include/winlvm/mapping_table.h.
// This C header is intentionally duplicated at the kernel boundary so the
// driver does not depend on user-mode C++ headers or LVM parser code.
// ============================================================================

#define LVMDM_MAPPING_TABLE_MAGIC 0x4C564D4DUL
#define LVMDM_MAPPING_TABLE_VERSION 1UL
#define LVMDM_MAX_VG_NAME_LENGTH 127UL
#define LVMDM_MAX_LV_NAME_LENGTH 127UL
#define LVMDM_MAX_SEGMENT_COUNT 512UL
#define LVMDM_MAX_PV_HANDLE_COUNT 512UL
#define LVMDM_MAX_SOURCE_PATH_LENGTH 511UL
#define LVMDM_MAPPING_FLAG_READONLY 0x0001UL
#define LVMDM_MAPPING_FLAG_FORCE 0x0002UL
#define LVMDM_MAPPING_FLAG_SHARED 0x0004UL

typedef struct _LVMDM_MAPPING_UUID {
  UINT8 bytes[16];
} LVMDM_MAPPING_UUID;

typedef enum _LVMDM_MAPPING_SEGMENT_TYPE {
  LvmdmSegmentLinear = 1,
  LvmdmSegmentStriped = 2
} LVMDM_MAPPING_SEGMENT_TYPE;

typedef struct _LVMDM_MAPPING_LINEAR_SEGMENT {
  UINT64 lv_start_bytes;
  UINT64 length_bytes;
  LVMDM_MAPPING_UUID pv_uuid;
  UINT64 pv_start_bytes;
} LVMDM_MAPPING_LINEAR_SEGMENT;

typedef struct _LVMDM_MAPPING_STRIPE_MEMBER {
  LVMDM_MAPPING_UUID pv_uuid;
  UINT64 pv_start_bytes;
} LVMDM_MAPPING_STRIPE_MEMBER;

typedef struct _LVMDM_MAPPING_STRIPED_SEGMENT {
  UINT64 lv_start_bytes;
  UINT64 length_bytes;
  UINT32 stripe_count;
  UINT32 stripe_size_bytes;
  LVMDM_MAPPING_STRIPE_MEMBER members[16];
} LVMDM_MAPPING_STRIPED_SEGMENT;

typedef union _LVMDM_MAPPING_SEGMENT_DATA {
  LVMDM_MAPPING_LINEAR_SEGMENT linear;
  LVMDM_MAPPING_STRIPED_SEGMENT striped;
} LVMDM_MAPPING_SEGMENT_DATA;

typedef struct _LVMDM_MAPPING_SEGMENT {
  UINT32 type;
  UINT32 reserved_1;
  LVMDM_MAPPING_SEGMENT_DATA data;
} LVMDM_MAPPING_SEGMENT;

typedef struct _LVMDM_MAPPING_PV_HANDLE {
  LVMDM_MAPPING_UUID pv_uuid;
  CHAR source_path[LVMDM_MAX_SOURCE_PATH_LENGTH + 1];
  UINT64 source_size_bytes;
  UINT64 pv_base_offset_bytes;
  UINT64 pv_size_bytes;
  UINT32 logical_sector_size;
  UINT32 physical_sector_size;
  UINT64 driver_handle;
} LVMDM_MAPPING_PV_HANDLE;

#pragma pack(push, 1)
typedef struct _LVMDM_MAPPING_TABLE_HEADER {
  UINT32 magic;
  UINT32 version;
  UINT32 flags;
  UINT32 reserved_1;
  UINT64 total_size_bytes;
  LVMDM_MAPPING_UUID vg_uuid;
  LVMDM_MAPPING_UUID lv_uuid;
  CHAR vg_name[LVMDM_MAX_VG_NAME_LENGTH + 1];
  CHAR lv_name[LVMDM_MAX_LV_NAME_LENGTH + 1];
  UINT64 lv_size_bytes;
  UINT32 exposed_logical_sector_size;
  UINT32 exposed_physical_sector_size;
  UINT32 segment_count;
  UINT32 pv_handle_count;
  UINT64 segments_offset;
  UINT64 pv_handles_offset;
} LVMDM_MAPPING_TABLE_HEADER;
#pragma pack(pop)

// ============================================================================
// Input/Output Buffer Structures for IOCTLs
// ============================================================================

// CREATE_MAPPING input: Binary mapping table (see mapping_table.h)
// Output: mapping_id (UINT64)

typedef struct _LVMDM_DELETE_MAPPING_INPUT {
  UINT64 mapping_id;
} LVMDM_DELETE_MAPPING_INPUT, *PLVMDM_DELETE_MAPPING_INPUT;

typedef struct _LVMDM_QUERY_MAPPING_INPUT {
  UINT64 mapping_id;
} LVMDM_QUERY_MAPPING_INPUT, *PLVMDM_QUERY_MAPPING_INPUT;

typedef struct _LVMDM_QUERY_MAPPING_OUTPUT {
  UINT64 mapping_id;
  UINT32 status;  // 0 = active, 1 = inactive, 2 = error
  UINT32 reserved;
  UINT64 io_count;
  UINT64 mapping_table_size;
} LVMDM_QUERY_MAPPING_OUTPUT, *PLVMDM_QUERY_MAPPING_OUTPUT;

typedef struct _LVMDM_LIST_MAPPINGS_OUTPUT {
  UINT32 count;
  UINT32 reserved;
  // Followed by array of UINT64 mapping_ids
} LVMDM_LIST_MAPPINGS_OUTPUT, *PLVMDM_LIST_MAPPINGS_OUTPUT;

#endif  // WINLVM_DRIVER_IOCTL_H
