#include "winlvm/device_enum.h"

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <windows.h>
#include <setupapi.h>
#include <winioctl.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "ole32.lib")

namespace winlvm {

namespace {

std::uint32_t le32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8U) |
         (static_cast<std::uint32_t>(p[2]) << 16U) |
         (static_cast<std::uint32_t>(p[3]) << 24U);
}

std::uint64_t le64(const std::uint8_t* p) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(p[i]) << (i * 8U);
  }
  return value;
}

bool is_zero_guid(const std::uint8_t* guid) {
  for (int i = 0; i < 16; ++i) {
    if (guid[i] != 0) {
      return false;
    }
  }
  return true;
}

// ============================================================================
// Windows API Wrappers
// ============================================================================

HANDLE open_disk_device(std::uint32_t disk_number) {
  std::string device_path = "\\\\.\\PhysicalDrive" + std::to_string(disk_number);
  HANDLE handle = CreateFileA(
    device_path.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr
  );
  return handle;
}

std::string get_disk_model(HANDLE disk_handle) {
  STORAGE_DEVICE_DESCRIPTOR device_descriptor{};
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceProperty;
  query.QueryType = PropertyStandardQuery;

  DWORD bytes_returned = 0;
  std::array<std::uint8_t, 512> buffer{};

  if (DeviceIoControl(
        disk_handle,
        IOCTL_STORAGE_QUERY_PROPERTY,
        &query,
        sizeof(query),
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &bytes_returned,
        nullptr)) {
    const STORAGE_DEVICE_DESCRIPTOR* desc =
      reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
    if (desc->ProductIdOffset > 0) {
      const char* product_id = (const char*)buffer.data() + desc->ProductIdOffset;
      return std::string(product_id);
    }
  }
  return "";
}

std::uint64_t get_disk_size(HANDLE disk_handle) {
  GET_LENGTH_INFORMATION length_info{};
  DWORD bytes_returned = 0;

  if (DeviceIoControl(
        disk_handle,
        IOCTL_DISK_GET_LENGTH_INFO,
        nullptr,
        0,
        &length_info,
        sizeof(length_info),
        &bytes_returned,
        nullptr)) {
    return length_info.Length.QuadPart;
  }
  return 0;
}

bool is_removable_media(HANDLE disk_handle) {
  STORAGE_HOTPLUG_INFO hotplug_info{};
  DWORD bytes_returned = 0;

  if (DeviceIoControl(
        disk_handle,
        IOCTL_STORAGE_GET_HOTPLUG_INFO,
        nullptr,
        0,
        &hotplug_info,
        sizeof(hotplug_info),
        &bytes_returned,
        nullptr)) {
    return hotplug_info.MediaRemovable != 0;
  }
  return false;
}

}  // namespace

// ============================================================================
// Public API Implementation
// ============================================================================

Result<std::vector<WindowsPhysicalDisk>> enumerate_physical_disks() {
  std::vector<WindowsPhysicalDisk> disks;

  // Enumerate disks from 0 to 31 (practical limit)
  for (std::uint32_t disk_num = 0; disk_num < 32; ++disk_num) {
    HANDLE disk_handle = open_disk_device(disk_num);
    if (disk_handle == INVALID_HANDLE_VALUE) {
      continue;  // Disk doesn't exist; continue to next
    }

    WindowsPhysicalDisk disk{};
    disk.disk_number = disk_num;
    disk.drive_model = get_disk_model(disk_handle);
    disk.friendly_name = disk.drive_model.empty()
                           ? ("Disk " + std::to_string(disk_num))
                           : disk.drive_model;
    disk.total_size_bytes = get_disk_size(disk_handle);
    disk.removable = is_removable_media(disk_handle);

    // TODO: Set is_system_disk and is_boot_disk via registry/WMI checks
    disk.is_system_disk = false;
    disk.is_boot_disk = false;

    if (disk.total_size_bytes > 0) {
      disks.push_back(disk);
    }

    CloseHandle(disk_handle);
  }

  if (disks.empty()) {
    return Result<std::vector<WindowsPhysicalDisk>>::failure("No physical disks found");
  }

  return Result<std::vector<WindowsPhysicalDisk>>::success(disks);
}

Result<std::vector<WindowsPhysicalPartition>> enumerate_partitions(
    std::uint32_t disk_number) {
  std::vector<WindowsPhysicalPartition> partitions;

  HANDLE disk_handle = open_disk_device(disk_number);
  if (disk_handle == INVALID_HANDLE_VALUE) {
    return Result<std::vector<WindowsPhysicalPartition>>::failure(
      "Cannot open disk " + std::to_string(disk_number));
  }

  // Read MBR and GPT header (first 2 sectors) to discover partitions.
  DWORD bytes_read = 0;
  std::array<std::uint8_t, 4096> sector_buffer{};

  if (!ReadFile(disk_handle, sector_buffer.data(), 1024, &bytes_read, nullptr) || bytes_read < 1024) {
    CloseHandle(disk_handle);
    return Result<std::vector<WindowsPhysicalPartition>>::failure(
      "Cannot read partition header from disk " + std::to_string(disk_number));
  }

  const char* gpt_sig = "EFI PART";
  const std::uint8_t* sector = sector_buffer.data();
  const bool has_mbr_sig = sector[510] == 0x55 && sector[511] == 0xaa;
  bool is_gpt = std::memcmp(sector + 512, gpt_sig, 8) == 0;

  if (is_gpt) {
    const std::uint8_t* gpt_header = sector + 512;
    const std::uint64_t partition_entries_lba = le64(gpt_header + 72);
    const std::uint32_t partition_entry_count = le32(gpt_header + 80);
    const std::uint32_t partition_entry_size = le32(gpt_header + 84);

    if (partition_entry_count == 0 || partition_entry_size < 128 || partition_entry_size > 4096) {
      CloseHandle(disk_handle);
      return Result<std::vector<WindowsPhysicalPartition>>::failure(
        "Invalid GPT partition table on disk " + std::to_string(disk_number));
    }

    const std::uint64_t entries_offset = partition_entries_lba * 512ULL;
    const std::uint64_t bytes_to_read =
        static_cast<std::uint64_t>(std::min<std::uint32_t>(partition_entry_count, 128)) * partition_entry_size;
    std::vector<std::uint8_t> entries(static_cast<std::size_t>(bytes_to_read));

    LARGE_INTEGER seek_offset{};
    seek_offset.QuadPart = static_cast<LONGLONG>(entries_offset);
    if (!SetFilePointerEx(disk_handle, seek_offset, nullptr, FILE_BEGIN)) {
      CloseHandle(disk_handle);
      return Result<std::vector<WindowsPhysicalPartition>>::failure(
        "Cannot seek to GPT partition entries on disk " + std::to_string(disk_number));
    }

    DWORD gpt_bytes_read = 0;
    if (!ReadFile(disk_handle, entries.data(), static_cast<DWORD>(entries.size()), &gpt_bytes_read, nullptr)) {
      CloseHandle(disk_handle);
      return Result<std::vector<WindowsPhysicalPartition>>::failure(
        "Cannot read GPT partition entries on disk " + std::to_string(disk_number));
    }

    const std::uint32_t parsed_count = std::min<std::uint32_t>(
        partition_entry_count, static_cast<std::uint32_t>(gpt_bytes_read / partition_entry_size));
    for (std::uint32_t i = 0; i < parsed_count; ++i) {
      const std::uint8_t* entry = entries.data() + (static_cast<std::size_t>(i) * partition_entry_size);
      if (is_zero_guid(entry)) {
        continue;
      }
      const std::uint64_t first_lba = le64(entry + 32);
      const std::uint64_t last_lba = le64(entry + 40);
      if (last_lba < first_lba) {
        continue;
      }
      WindowsPhysicalPartition partition{};
      partition.disk_number = disk_number;
      partition.partition_number = i + 1;
      partition.start_offset_bytes = first_lba * 512ULL;
      partition.size_bytes = (last_lba - first_lba + 1ULL) * 512ULL;
      partition.partition_type = "GPT";
      partition.is_mounted = false;
      partition.mount_point = "";
      partitions.push_back(partition);
    }
  } else if (has_mbr_sig) {
    // Parse MBR partition table (offset 446, 4 entries, 16 bytes each)
    for (int i = 0; i < 4; ++i) {
      const std::uint8_t* pte = sector + 446 + (i * 16);
      std::uint8_t partition_type = pte[4];

      if (partition_type == 0) {
        continue;  // Unused partition
      }

      // Decode LBA values (little-endian, avoid unaligned reads)
      std::uint32_t start_lba = le32(pte + 8);
      std::uint32_t size_lba = le32(pte + 12);

      WindowsPhysicalPartition partition{};
      partition.disk_number = disk_number;
      partition.partition_number = i + 1;
      partition.start_offset_bytes = (std::uint64_t)start_lba * 512;
      partition.size_bytes = (std::uint64_t)size_lba * 512;
      partition.partition_type = "MBR";
      partition.is_mounted = false;  // TODO: Check mount points
      partition.mount_point = "";

      if (partition.size_bytes > 0) {
        partitions.push_back(partition);
      }
    }
  }

  CloseHandle(disk_handle);

  if (partitions.empty()) {
    return Result<std::vector<WindowsPhysicalPartition>>::failure(
      "No partitions found on disk " + std::to_string(disk_number));
  }

  return Result<std::vector<WindowsPhysicalPartition>>::success(partitions);
}

Result<bool> is_system_disk(std::uint32_t disk_number) {
  (void)disk_number;
  // TODO: Check via registry (HKLM\SYSTEM\CurrentControlSet\Services\disk)
  // or via WMI (Win32_DiskDrive.Index matching Windows system disk)
  // For now, return false (safe default)
  return Result<bool>::success(false);
}

Result<bool> is_boot_disk(std::uint32_t disk_number) {
  (void)disk_number;
  // TODO: Check via GetDriveLayout and compare with boot loader location
  // For now, return false (safe default)
  return Result<bool>::success(false);
}

Result<bool> disk_has_mounted_volumes(std::uint32_t disk_number) {
  (void)disk_number;
  // TODO: Enumerate mount points and check if any belong to this disk
  // For now, return false (conservative, won't activate mounted disks)
  return Result<bool>::success(false);
}

}  // namespace winlvm

#endif  // _WIN32
