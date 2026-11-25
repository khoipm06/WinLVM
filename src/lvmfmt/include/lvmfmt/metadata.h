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

struct RaidLeg {
  std::string meta_lv;            // raid1: "<lv>_rmeta_N" (empty for mirror)
  std::string image_lv;           // raid1: "<lv>_rimage_N"; mirror: "<lv>_mimage_N"
  std::uint64_t start_extent = 0; // mirror leg start extent (typically 0)
};

struct Segment {
  std::uint64_t start_extent = 0;
  std::uint64_t extent_count = 0;
  std::string type;               // "striped", "linear", "thin-pool", "thin", "snapshot", "raid1", "mirror", ...
  std::uint64_t stripe_count = 1;
  std::uint64_t stripe_size_sectors = 0;
  std::vector<SegmentStripe> stripes;

  // Structured fields for thin-pool, thin, snapshot, raid1, and mirror.
  std::string metadata_lv;             // thin-pool: metadata = "<tmeta_lv>"
  std::string pool_lv;                 // thin-pool: pool = "<tdata_lv>"; thin: thin_pool = "<pool_lv>"
  std::uint64_t chunk_size_sectors = 0;// thin-pool & snapshot: chunk_size (512B sectors)
  std::uint64_t transaction_id = 0;    // thin-pool & thin
  bool has_transaction_id = false;
  bool zero_new_blocks = true;         // thin-pool
  bool has_zero_new_blocks = false;
  std::string discards;                // thin-pool: "passdown", "ignore", "nopassdown"
  std::uint64_t device_id = 0;         // thin
  bool has_device_id = false;
  std::string origin;                  // snapshot & thin snapshot
  std::string cow_store;               // snapshot: cow_store / cow
  std::uint64_t region_size_sectors = 0;// raid1 & mirror
  std::uint64_t device_count = 0;      // raid1: device_count / raid_count; mirror: mirror_count
  std::string mirror_log;              // mirror: mirror_log
  std::vector<RaidLeg> raid_legs;      // raid1: raids = [...]; mirror: mirrors = [...]
};

struct LogicalVolume {
  std::string name;
  std::string id;                 // raw 32-char or canonical dashed UUID
  std::vector<std::string> status;
  std::vector<std::string> flags;
  std::vector<std::string> tags;
  std::vector<Segment> segments;

  // Set when this LV cannot be mapped by winlvm (e.g., raid5, cache, vdo).
  bool supported = true;
  std::string unsupported_reason;

  // True for LVM's hidden sub-volumes (RAID images/meta, thin pool data/meta, COW stores, snapshot0).
  bool internal = false;

  // Semantic roles for complex volume layouts.
  bool is_thin_pool = false;
  bool is_snapshot_origin = false;
  bool is_snapshot_cow = false;
  std::string snapshot_wrapper;   // classic LVM2 wrapper name (e.g. "snapshot0")

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
  std::vector<std::string> flags;
  std::vector<std::string> tags;

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
  std::vector<std::string> flags;
  std::vector<std::string> tags;
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
