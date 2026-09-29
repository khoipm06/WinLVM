#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include "lvmfmt/metadata.h"
#include "lvmfmt/source.h"

namespace lvmfmt {

// Parse human-readable sizes (e.g. "100M", "2G", "512K", "1024") into bytes.
std::optional<std::uint64_t> parse_size_bytes(std::string_view str, std::string& error);

// Format bytes into human-readable representation.
std::string format_size_human(std::uint64_t bytes);

// Tracks allocated and free extents on a Physical Volume.
struct PvExtentMap {
    std::string pv_key;
    std::string pv_uuid;
    std::uint64_t total_extents = 0;
    std::vector<bool> extent_used; // index 0..total_extents-1
    std::uint64_t free_extents = 0;
};

// Build extent usage maps for all PVs in a Volume Group.
std::vector<PvExtentMap> build_vg_extent_maps(const VolumeGroup& vg);

// Allocation result for new or extended segments.
struct ExtentAllocation {
    std::string pv_key;
    std::uint64_t start_pe = 0;
    std::uint64_t count = 0;
};

// Allocate extents for a linear logical volume.
// If target_pvs is non-empty, only allocates from the specified PV keys/names.
bool allocate_linear(const VolumeGroup& vg,
                     std::uint64_t extents_needed,
                     const std::vector<std::string>& preferred_pvs,
                     std::vector<ExtentAllocation>& out_allocations,
                     std::string& error);

// Allocate extents for a striped logical volume across N stripes.
bool allocate_striped(const VolumeGroup& vg,
                      std::uint64_t extents_needed,
                      std::uint32_t stripe_count,
                      const std::vector<std::string>& preferred_pvs,
                      std::vector<std::vector<ExtentAllocation>>& out_stripe_allocations,
                      std::string& error);

// Extend an existing LV with new allocations.
bool extend_lv(VolumeGroup& vg,
               std::string_view lv_name,
               std::uint64_t additional_extents,
               const std::vector<std::string>& preferred_pvs,
               std::string& error);

// Reduce an existing LV by removing trailing extents.
bool reduce_lv(VolumeGroup& vg,
               std::string_view lv_name,
               std::uint64_t extents_to_remove,
               std::string& error);

// Migrate allocated extents from source PV to destination PV (pvmove).
// Copies underlying data blocks between block sources if provided.
bool move_pv_extents(VolumeGroup& vg,
                     std::string_view source_pv_key_or_name,
                     std::string_view dest_pv_key_or_name,
                     std::optional<std::string_view> target_lv_name,
                     ByteSource* source_storage,
                     ByteSource* dest_storage,
                     std::string& error);

} // namespace lvmfmt
