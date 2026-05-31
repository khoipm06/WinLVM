#include "winlvm/stripe_support.h"
#include "winlvm/model.h"

#include <algorithm>
#include <map>

namespace winlvm {

// ============================================================================
// Validate Multi-PV Linear: Check contiguity across multiple PVs
// ============================================================================

Result<bool> validate_multi_pv_linear(
    const std::vector<Segment>& segments) {

  if (segments.empty()) {
    return Result<bool>::success(true);
  }

  // Linear layout means segments compose by ordering, not by striping.
  // Each linear segment must map a contiguous slice of LV address space.
  // Non-contiguous gaps are fatal.
  std::uint64_t expected_start = 0;

  for (const auto& seg : segments) {
    if (seg.type != "linear" && seg.type != "striped") {
      return Result<bool>::failure(
        "Non-linear segment type encountered: " + seg.type);
    }

    if (seg.lv_start_bytes != expected_start) {
      return Result<bool>::failure(
        "Multi-PV linear gap detected: expected LV byte " +
        std::to_string(expected_start) + " but segment starts at " +
        std::to_string(seg.lv_start_bytes));
    }

    if (seg.length_bytes == 0) {
      return Result<bool>::failure("Zero-length segment in multi-PV linear layout");
    }

    expected_start += seg.length_bytes;
  }

  return Result<bool>::success(true);
}

// ============================================================================
// Validate Striped Layout: Multi-PV striped topology
// ============================================================================

Result<StripedValidationResult> validate_striped_layout(
    const std::vector<Segment>& segments,
    std::uint64_t extent_size_bytes) {

  StripedValidationResult result;
  result.valid = false;

  if (segments.empty()) {
    result.error_message = "No segments provided";
    return Result<StripedValidationResult>::success(result);
  }

  // All segments must have the same type
  const auto& first_seg = segments[0];
  if (first_seg.type != "striped") {
    result.error_message = "Expected striped segment type, got " + first_seg.type;
    return Result<StripedValidationResult>::success(result);
  }

  // Count unique PVs across segments to determine stripe count
  std::map<std::string, StripeMember> member_map;
  std::uint64_t total_lv_bytes = 0;

  for (const auto& seg : segments) {
    if (seg.type != "striped") {
      result.error_message = "Inconsistent segment type across stripes: " + seg.type;
      return Result<StripedValidationResult>::success(result);
    }

    // Track unique PVs
    std::string pv_key = seg.pv_uuid.empty() ? seg.pv_name : seg.pv_uuid;
    if (member_map.find(pv_key) == member_map.end()) {
      StripeMember member;
      member.pv_name = seg.pv_name;
      member.pv_uuid = seg.pv_uuid;
      member.pv_start_extent = seg.pv_start_extent;
      member.pv_start_bytes = seg.pv_start_bytes;
      member_map[pv_key] = member;
    }

    total_lv_bytes += seg.length_bytes;
  }

  // Validate stripe count (must be at least 2 for real striping)
  result.stripe_count = static_cast<std::uint32_t>(member_map.size());
  for (const auto& pair : member_map) {
    result.members.push_back(pair.second);
  }

  if (result.stripe_count < 1) {
    result.error_message = "No valid PVs found in striped segment";
    return Result<StripedValidationResult>::success(result);
  }

  // Compute stripe size from first segment extent count
  // In LVM2 striped metadata, stripe_size is extent_size / stripe_count
  // Each stripe takes a stripe_size chunk before switching to the next PV
  result.stripe_size_bytes = extent_size_bytes;

  if (result.stripe_count == 1) {
    // Single stripe = linear; valid but not real striping
    result.valid = true;
    return Result<StripedValidationResult>::success(result);
  }

  // Multi-PV stripe validation: check that all PV members exist
  for (const auto& seg : segments) {
    std::string pv_key = seg.pv_uuid.empty() ? seg.pv_name : seg.pv_uuid;
    if (member_map.find(pv_key) == member_map.end()) {
      result.error_message = "Stripe references unknown PV: " + pv_key;
      return Result<StripedValidationResult>::success(result);
    }
  }

  result.valid = true;
  return Result<StripedValidationResult>::success(result);
}

// ============================================================================
// Translate Striped Offset: Logical LV byte → (PV index, PV byte offset)
// ============================================================================

Result<std::pair<std::uint32_t, std::uint64_t>> translate_striped_offset(
    const StripedValidationResult& stripe_layout,
    std::uint64_t logical_offset_bytes) {

  if (!stripe_layout.valid) {
    return Result<std::pair<std::uint32_t, std::uint64_t>>::failure(
      "Cannot translate offset: invalid stripe layout");
  }

  if (stripe_layout.stripe_count == 0) {
    return Result<std::pair<std::uint32_t, std::uint64_t>>::failure(
      "Cannot translate offset: stripe count is zero");
  }

  if (stripe_layout.stripe_size_bytes == 0) {
    return Result<std::pair<std::uint32_t, std::uint64_t>>::failure(
      "Cannot translate offset: stripe size is zero");
  }

  // In a striped layout:
  // logical_offset / stripe_size_bytes = stripe number
  // stripe number % stripe_count = which PV
  // stripe number / stripe_count = stripe_row
  // physical offset = stripe_row * stripe_size_bytes + (logical_offset % stripe_size_bytes)

  std::uint64_t stripe_number = logical_offset_bytes / stripe_layout.stripe_size_bytes;
  std::uint64_t stripe_within_row = logical_offset_bytes % stripe_layout.stripe_size_bytes;

  std::uint32_t pv_index = static_cast<std::uint32_t>(stripe_number % stripe_layout.stripe_count);
  std::uint64_t stripe_row = stripe_number / stripe_layout.stripe_count;

  std::uint64_t pv_offset = (stripe_row * stripe_layout.stripe_size_bytes) + stripe_within_row;

  if (pv_index >= stripe_layout.members.size()) {
    return Result<std::pair<std::uint32_t, std::uint64_t>>::failure(
      "PV index out of bounds: " + std::to_string(pv_index));
  }

  // Add PV start offset
  const auto& member = stripe_layout.members[pv_index];
  pv_offset += member.pv_start_bytes;

  return Result<std::pair<std::uint32_t, std::uint64_t>>::success(
    std::make_pair(pv_index, pv_offset));
}

}  // namespace winlvm
