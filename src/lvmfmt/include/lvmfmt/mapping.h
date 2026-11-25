// Translating an LV's extents into a device-mapper table.
//
// This is the function the whole format layer exists to serve, and it is the
// one with an external oracle: Linux's own `dmsetup table` output for the same
// volume. Any disagreement is a bug here, not a difference of opinion.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lvmfmt/metadata.h"

namespace lvmfmt {

inline constexpr std::uint64_t kSectorSize = 512;

struct DmDestination {
  std::string pv_key;             // "pv0"
  std::string pv_uuid;            // raw 32-char UUID — the identity that matters
  std::uint64_t offset_sectors = 0;  // absolute offset into the PV
};

struct DmTarget {
  std::uint64_t start_sector = 0;
  std::uint64_t length_sectors = 0;
  std::string type;                  // "linear", "striped", "thin", "snapshot", "snapshot-origin", "raid1", "mirror"
  std::uint64_t stripe_count = 1;
  std::uint64_t chunk_sectors = 0;   // striped & snapshot
  std::uint64_t device_id = 0;       // thin
  std::uint64_t region_size_sectors = 0; // raid1 & mirror
  std::string pool_lv;               // thin
  std::string origin_lv;             // snapshot & snapshot-origin
  std::string cow_lv;                // snapshot
  std::string mirror_log;            // mirror
  std::vector<DmDestination> destinations;
  std::vector<std::string> sub_lvs;  // raid1/mirror sub-LV references
};

// Builds the table for `lv`. Returns nullopt with a reason in `error` if the
// LV is unsupported, its segments do not tile [0, size) exactly, or any offset
// would fall outside its physical volume.
std::optional<std::vector<DmTarget>> build_dm_table(const VolumeGroup& vg,
                                                    const LogicalVolume& lv,
                                                    std::string& error);

// Renders one target in `dmsetup table` syntax, using `device_name` in place
// of the major:minor that dmsetup prints. Exists so the corpus tests can
// compare our output to Linux's textually.
std::string format_dm_target(const DmTarget& target,
                             const std::vector<std::string>& device_names);

}  // namespace lvmfmt
