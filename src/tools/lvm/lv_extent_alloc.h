#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "lvmfmt/metadata.h"

namespace lvmfmt {

// Builds linear Segments for a logical volume allocating extents_needed from vg.
bool build_linear_segments(const VolumeGroup& vg,
                           std::uint64_t extents_needed,
                           std::vector<Segment>& out_segments,
                           std::string& error);

// Builds striped Segments for a logical volume allocating extents_needed across stripe_count PVs,
// handling fragmented PV chunk allocations.
bool build_striped_segments(const VolumeGroup& vg,
                            std::uint64_t extents_needed,
                            std::uint32_t stripe_count,
                            std::uint32_t stripe_size_sectors,
                            std::vector<Segment>& out_segments,
                            std::string& error);

} // namespace lvmfmt
