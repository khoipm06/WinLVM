#pragma once

#include <winioctl.h>
#include <winnt.h>
#include <cstdint>

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
// Input/Output Buffer Structures for IOCTLs
// ============================================================================

// CREATE_MAPPING input: Binary mapping table (see mapping_table.h)
// Output: mapping_id (UINT64)

typedef struct _LVMDM_DELETE_MAPPING_INPUT {
  UINT64 mapping_id;
} LVMDM_DELETE_MAPPING_INPUT;

typedef struct _LVMDM_QUERY_MAPPING_INPUT {
  UINT64 mapping_id;
} LVMDM_QUERY_MAPPING_INPUT;

typedef struct _LVMDM_QUERY_MAPPING_OUTPUT {
  UINT64 mapping_id;
  UINT32 status;  // 0 = active, 1 = inactive, 2 = error
  UINT64 io_count;
} LVMDM_QUERY_MAPPING_OUTPUT;

typedef struct _LVMDM_LIST_MAPPINGS_OUTPUT {
  UINT32 count;
  // Followed by array of UINT64 mapping_ids
} LVMDM_LIST_MAPPINGS_OUTPUT;

#endif  // WINLVM_DRIVER_IOCTL_H
