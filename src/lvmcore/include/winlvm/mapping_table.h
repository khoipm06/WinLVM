#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace winlvm {

// ============================================================================
// Versioned Mapping Table ABI for User-to-Kernel Communication
//
// This is the stable contract between the activation service and the kernel
// driver. All structures are packed and byte-aligned for cross-process safety.
// ============================================================================

constexpr std::uint32_t kMappingTableVersion = 1;
constexpr std::uint32_t kMappingTableMagic = 0x4C564D4D;  // "LVMM" in little-endian
constexpr std::size_t kMaxVgNameLength = 127;
constexpr std::size_t kMaxLvNameLength = 127;
constexpr std::size_t kMaxSegmentCount = 512;  // Reasonable upper limit for segments

// GUID-like UUID representation (128-bit)
#pragma pack(push, 1)
struct MappingUuid {
  std::uint8_t bytes[16];

  MappingUuid() = default;

  // Compare UUIDs
  bool operator==(const MappingUuid& other) const {
    return std::equal(bytes, bytes + 16, other.bytes);
  }
  bool operator!=(const MappingUuid& other) const { return !(*this == other); }
};
#pragma pack(pop)

// ============================================================================
// Segment Types
// ============================================================================
enum class MappingSegmentType : std::uint32_t {
  kLinear = 1,    // Single PV, contiguous extent mapping
  kStriped = 2,   // Multi-PV striped (v2+)
};

// ============================================================================
// Linear Segment: Maps an LV range to a single PV range
// ============================================================================
struct MappingLinearSegment {
  std::uint64_t lv_start_bytes;      // Logical LV start
  std::uint64_t length_bytes;        // Byte count (multiple of logical sector size)
  MappingUuid pv_uuid;               // Backing PV UUID
  std::uint64_t pv_start_bytes;      // Physical offset within PV
};

// ============================================================================
// Striped Segment Member: Single PV in striped set (v2+, reserved)
// ============================================================================
struct MappingStripeMember {
  MappingUuid pv_uuid;
  std::uint64_t pv_start_bytes;
};

// ============================================================================
// Striped Segment: Multi-PV striped layout (v2+, reserved)
// ============================================================================
struct MappingStripedSegment {
  std::uint64_t lv_start_bytes;      // Logical LV start
  std::uint64_t length_bytes;
  std::uint32_t stripe_count;        // Number of PVs
  std::uint32_t stripe_size_bytes;   // Data per stripe
  std::array<MappingStripeMember, 16> members;  // Member array (v1 uses max 1)
};

// ============================================================================
// Segment Union: Discriminated union of segment types
// ============================================================================
union MappingSegmentData {
  MappingLinearSegment linear;
  MappingStripedSegment striped;
};

// ============================================================================
// Single Segment Record
// ============================================================================
struct MappingSegment {
  MappingSegmentType type;           // Segment type (linear, striped, etc.)
  std::uint32_t _reserved_1;         // Padding for alignment
  MappingSegmentData data;            // Discriminated data
};

// ============================================================================
// Physical Volume Reference in Mapping Table
// ============================================================================
struct MappingPvHandle {
  MappingUuid pv_uuid;
  std::uint64_t pv_size_bytes;
  std::uint32_t logical_sector_size;   // Usually 512 or 4096
  std::uint32_t physical_sector_size;  // Usually 512, 4096, or 8192
  // Driver-specific reference/handle populated by driver (e.g., handle index)
  std::uint64_t driver_handle;
};

// ============================================================================
// Mapping Table Header and Complete ABI Structure
// ============================================================================
#pragma pack(push, 1)
struct MappingTableHeader {
  std::uint32_t magic;                // kMappingTableMagic validation
  std::uint32_t version;              // kMappingTableVersion
  std::uint32_t flags;                // Activation flags (read-only, force, etc.)
  std::uint32_t _reserved_1;

  std::uint64_t total_size_bytes;     // Size of entire structure
  MappingUuid vg_uuid;                // Volume group UUID
  MappingUuid lv_uuid;                // Logical volume UUID

  char vg_name[kMaxVgNameLength + 1];        // NUL-terminated VG name
  char lv_name[kMaxLvNameLength + 1];        // NUL-terminated LV name
  std::uint64_t lv_size_bytes;               // Total LV size

  std::uint32_t exposed_logical_sector_size;   // Sector size exposed to Windows
  std::uint32_t exposed_physical_sector_size;
  std::uint32_t segment_count;                 // Number of segments following
  std::uint32_t pv_handle_count;               // Number of PVs

  // Offsets (relative to start of structure) for variable-length sections
  std::uint64_t segments_offset;              // Relative offset to segments array
  std::uint64_t pv_handles_offset;            // Relative offset to PV handles array
};
#pragma pack(pop)

// ============================================================================
// Activation Flags
// ============================================================================
constexpr std::uint32_t kMappingFlagReadOnly = 0x0001;       // Read-only activation
constexpr std::uint32_t kMappingFlagForce = 0x0002;          // Force activation (ignore warnings)
constexpr std::uint32_t kMappingFlagShared = 0x0004;         // Shared read-only (v2+)

// ============================================================================
// Helper: Safe layout validation
// ============================================================================
inline bool validate_mapping_table_header(const MappingTableHeader& hdr) {
  return hdr.magic == kMappingTableMagic && hdr.version == kMappingTableVersion &&
         hdr.total_size_bytes > sizeof(MappingTableHeader) &&
         hdr.segment_count > 0 && hdr.segment_count <= kMaxSegmentCount &&
         hdr.pv_handle_count > 0;
}

}  // namespace winlvm
