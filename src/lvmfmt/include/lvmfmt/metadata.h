// The LVM object model, built from the parsed config tree.
//
// Design rule: this layer *parses everything and rejects nothing*. A thin
// pool, a RAID set, or a segment type we have never heard of still produces a
// LogicalVolume — flagged as unsupported, with the reason recorded. Deciding
// what to refuse is policy, and policy belongs above the format layer.
//
// The alternative (bail out at parse time) is what makes a tool fail an entire
// volume group because one LV in it is thin-provisioned.
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
  std::string id;                 // raw 32-char UUID
  std::vector<std::string> status;
  std::vector<Segment> segments;

  // Set when this LV cannot be mapped to a device-mapper table by this code.
  bool supported = true;
  std::string unsupported_reason;

  // True for LVM's hidden sub-volumes (RAID images, thin pool data/metadata).
  // Detected structurally: some other LV's segments reference this one.
  bool internal = false;

  std::uint64_t extent_count() const;
};

struct PhysicalVolume {
  std::string key;                // "pv0" — how segments refer to it
  std::string id;                 // raw 32-char UUID
  std::string device;             // Linux path from the metadata. Informational
                                  // only; never used to locate backing storage.
  std::uint64_t pe_start_sectors = 0;
  std::uint64_t pe_count = 0;
  std::uint64_t dev_size_sectors = 0;
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

  const PhysicalVolume* find_pv(std::string_view key) const;
  const PhysicalVolume* find_pv_by_uuid(std::string_view uuid) const;
  const LogicalVolume* find_lv(std::string_view name) const;
};

std::optional<VolumeGroup> parse_vg_metadata(std::string_view text, std::string& error);

}  // namespace lvmfmt
