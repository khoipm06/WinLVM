#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "winlvm/model.h"
#include "winlvm/result.h"

namespace winlvm {

// ============================================================================
// Striped Segment Support (v2+)
// ============================================================================

// Represents a stripe (one stripe_size worth of data spread across multiple PVs)
struct StripeMember {
  std::string pv_name;
  std::string pv_uuid;
  std::uint64_t pv_start_extent;
  std::uint64_t pv_start_bytes;
};

// Validation result for striped layouts
struct StripedValidationResult {
  bool valid = false;
  std::string error_message;
  std::uint32_t stripe_count = 0;
  std::uint64_t stripe_size_bytes = 0;
  std::vector<StripeMember> members;
};

// ============================================================================
// Multi-PV Linear Support (v1.5 bridge)
// ============================================================================

// Check if multiple segments can be contiguous across multiple PVs
Result<bool> validate_multi_pv_linear(
  const std::vector<Segment>& segments
);

// ============================================================================
// Striped LV Validation
// ============================================================================

Result<StripedValidationResult> validate_striped_layout(
  const std::vector<Segment>& segments,
  std::uint64_t extent_size_bytes
);

// Translate striped logical offset to physical PV offset + PV index
Result<std::pair<std::uint32_t, std::uint64_t>> translate_striped_offset(
  const StripedValidationResult& stripe_layout,
  std::uint64_t logical_offset_bytes
);

}  // namespace winlvm
