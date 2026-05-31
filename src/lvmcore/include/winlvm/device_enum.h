#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "winlvm/result.h"

namespace winlvm {

#ifdef _WIN32

// ============================================================================
// Windows-Specific Device Information
// ============================================================================

struct WindowsPhysicalDisk {
  std::uint32_t disk_number;          // Disk 0, 1, 2, ...
  std::string friendly_name;          // "ST500DM002-1ER14C" or "Virtual Disk"
  std::string drive_model;            // Full model string
  std::uint64_t total_size_bytes;     // Disk capacity
  bool removable;                     // Is removable/USB
  bool is_system_disk;                // Windows system disk
  bool is_boot_disk;                  // Boot disk
};

struct WindowsPhysicalPartition {
  std::uint32_t disk_number;
  std::uint32_t partition_number;
  std::uint64_t start_offset_bytes;
  std::uint64_t size_bytes;
  std::string partition_type;         // "MBR", "GPT", etc.
  bool is_mounted;
  std::string mount_point;
};

// ============================================================================
// Device Enumeration API
// ============================================================================

Result<std::vector<WindowsPhysicalDisk>> enumerate_physical_disks();

Result<std::vector<WindowsPhysicalPartition>> enumerate_partitions(
  std::uint32_t disk_number
);

// ============================================================================
// Safety Checks
// ============================================================================

Result<bool> is_system_disk(std::uint32_t disk_number);

Result<bool> is_boot_disk(std::uint32_t disk_number);

Result<bool> disk_has_mounted_volumes(std::uint32_t disk_number);

#endif  // _WIN32

}  // namespace winlvm
