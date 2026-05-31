#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace winlvm {

inline constexpr std::uint64_t kSectorSize = 512;
inline constexpr std::uint32_t kJsonSchemaVersion = 1;

struct Diagnostic {
  std::string severity;
  std::string code;
  std::string message;
};

struct InputImage {
  std::string source_type;
  std::string path;
  std::uint64_t size_bytes = 0;
  std::vector<std::uint64_t> candidate_pv_base_offsets;
};

struct MetadataRawLocation {
  std::uint64_t offset_bytes = 0;
  std::uint64_t size_bytes = 0;
  std::uint32_t checksum = 0;
  std::uint32_t actual_checksum = 0;
  std::uint32_t flags = 0;
  bool checksum_valid = false;
  bool ignored = false;
  std::string text_sha256_placeholder;
};

struct MetadataCopy {
  std::string pv_uuid;
  std::string source_path;
  std::uint64_t pv_base_offset_bytes = 0;
  std::uint64_t mda_offset_bytes = 0;
  std::uint64_t mda_size_bytes = 0;
  bool header_checksum_valid = false;
  bool magic_valid = false;
  bool selected = false;
  std::string rejected_reason;
  std::string vg_name;
  std::string vg_uuid;
  std::uint64_t seqno = 0;
  std::vector<MetadataRawLocation> raw_locations;
  std::string text;
};

struct PhysicalVolume {
  std::string name;
  std::string uuid;
  std::string source_path;
  std::uint64_t source_size_bytes = 0;
  std::uint64_t pv_base_offset_bytes = 0;
  std::uint64_t label_offset_bytes = 0;
  std::uint64_t device_size_bytes = 0;
  std::uint64_t pe_start_bytes = 0;
  std::uint64_t pe_count = 0;
  std::vector<std::uint64_t> metadata_area_offsets;
};

struct Segment {
  std::uint64_t start_extent = 0;
  std::uint64_t extent_count = 0;
  std::string type;
  std::string pv_name;
  std::string pv_uuid;
  std::uint64_t pv_start_extent = 0;
  std::uint64_t lv_start_bytes = 0;
  std::uint64_t length_bytes = 0;
  std::uint64_t pv_start_bytes = 0;
};

struct LogicalVolume {
  std::string name;
  std::string uuid;
  std::string vg_name;
  std::uint64_t size_bytes = 0;
  bool visible = true;
  bool supported = true;
  std::string unsupported_reason;
  std::vector<std::string> status;
  std::vector<Segment> segments;
};

struct VolumeGroup {
  std::string name;
  std::string uuid;
  std::uint64_t seqno = 0;
  std::uint64_t extent_size_sectors = 0;
  std::uint64_t extent_size_bytes = 0;
  std::vector<std::string> status;
  std::vector<PhysicalVolume> pvs;
  std::vector<LogicalVolume> lvs;
};

struct MappingRange {
  std::string vg_name;
  std::string lv_name;
  std::string lv_uuid;
  std::uint64_t lv_start_bytes = 0;
  std::uint64_t length_bytes = 0;
  std::string pv_uuid;
  std::uint64_t pv_start_bytes = 0;
  std::string source_path;
  std::uint64_t source_offset_bytes = 0;
};

struct ScanResult {
  std::vector<InputImage> inputs;
  std::vector<PhysicalVolume> pvs;
  std::vector<MetadataCopy> metadata_copies;
  std::vector<VolumeGroup> vgs;
  std::vector<MappingRange> mappings;
  std::vector<Diagnostic> diagnostics;
};

}  // namespace winlvm
