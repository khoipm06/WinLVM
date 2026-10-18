// The LVM object model, built from the parsed config tree.
//
// Design rule: this layer *parses everything and rejects nothing*. A thin
// pool, a RAID set, or a segment type we have never heard of still produces a
// LogicalVolume — flagged as unsupported, with the reason recorded. Deciding
// what to refuse is policy, and policy belongs above the format layer.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lvmfmt {

struct SegmentStripe {
  std::string target;             // a PV key ("pv0") or, for internal LVs, an LV name
  std::uint64_t start_extent = 0;
};

struct Segment {
  std::uint64_t start_extent = 0;
  std::uint64_t extent_count = 0;
  std::string type;               // "striped", "linear", "thin", "raid1", ...
  std::uint64_t stripe_count = 1;
  std::uint64_t stripe_size_sectors = 0;
  std::vector<SegmentStripe> stripes;
};

struct LogicalVolume {
  std::string name;
  std::string id;                 // raw 32-char or canonical dashed UUID
  std::vector<std::string> status;
  std::vector<Segment> segments;

  // Set when this LV cannot be mapped to a device-mapper table by this code.
  bool supported = true;
  std::string unsupported_reason;

  // True for LVM's hidden sub-volumes (RAID images, thin pool data/metadata).
  bool internal = false;

  std::uint64_t extent_count() const;
  bool is_read_only() const;
  bool is_visible() const;
};

struct PhysicalVolume {
  std::string key;                // "pv0" — how segments refer to it
  std::string id;                 // raw 32-char or canonical dashed UUID
  std::string device;             // Device hint from metadata; informational only.
  std::uint64_t pe_start_sectors = 0;
  std::uint64_t pe_count = 0;
  std::uint64_t dev_size_sectors = 0;
  std::vector<std::string> status;

  bool is_missing() const;
  bool is_allocatable() const;
};

struct VolumeGroup {
  std::string name;
  std::string id;
  std::uint64_t seqno = 0;
  std::uint64_t extent_size_sectors = 0;
  std::string system_id;
  std::vector<std::string> status;
  std::vector<PhysicalVolume> pvs;
  std::vector<LogicalVolume> lvs;
  std::uint64_t max_lv = 0;
  std::uint64_t max_pv = 0;
  std::uint64_t metadata_copies = 0;

  const PhysicalVolume* find_pv(std::string_view key) const;
  const PhysicalVolume* find_pv_by_uuid(std::string_view uuid) const;
  const LogicalVolume* find_lv(std::string_view name) const;
  bool is_read_only() const;
  bool is_exported() const;
  bool is_resizeable() const;
};

std::optional<VolumeGroup> parse_vg_metadata(std::string_view text, std::string& error);

}  // namespace lvmfmt
