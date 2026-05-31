#include "winlvm/block_mapper.h"

#include <algorithm>
#include <limits>

namespace winlvm {

namespace {

bool add_overflows(std::uint64_t a, std::uint64_t b) {
  return b > std::numeric_limits<std::uint64_t>::max() - a;
}

}  // namespace

Result<std::vector<PhysicalIoRange>> map_logical_io(
    const std::vector<MappingRange>& mappings,
    std::uint64_t lv_size_bytes,
    const LogicalIoRequest& request) {
  std::vector<PhysicalIoRange> physical_ranges;

  if (request.length_bytes == 0) {
    return Result<std::vector<PhysicalIoRange>>::success(physical_ranges);
  }
  if (add_overflows(request.offset_bytes, request.length_bytes)) {
    return Result<std::vector<PhysicalIoRange>>::failure("logical I/O range overflows 64-bit offset");
  }
  const std::uint64_t request_end = request.offset_bytes + request.length_bytes;
  if (request_end > lv_size_bytes) {
    return Result<std::vector<PhysicalIoRange>>::failure("logical I/O range is outside LV bounds");
  }

  std::vector<MappingRange> sorted = mappings;
  std::sort(sorted.begin(), sorted.end(), [](const MappingRange& a, const MappingRange& b) {
    return a.lv_start_bytes < b.lv_start_bytes;
  });

  std::uint64_t cursor = request.offset_bytes;
  for (const MappingRange& mapping : sorted) {
    if (cursor >= request_end) {
      break;
    }
    if (mapping.length_bytes == 0) {
      continue;
    }
    if (add_overflows(mapping.lv_start_bytes, mapping.length_bytes) ||
        add_overflows(mapping.source_offset_bytes, mapping.length_bytes)) {
      return Result<std::vector<PhysicalIoRange>>::failure("mapping range overflows 64-bit offset");
    }

    const std::uint64_t mapping_end = mapping.lv_start_bytes + mapping.length_bytes;
    if (cursor < mapping.lv_start_bytes) {
      continue;
    }
    if (cursor >= mapping_end) {
      continue;
    }

    const std::uint64_t within_mapping = cursor - mapping.lv_start_bytes;
    const std::uint64_t available = mapping_end - cursor;
    const std::uint64_t needed = request_end - cursor;
    const std::uint64_t chunk = std::min(available, needed);

    PhysicalIoRange range;
    range.pv_uuid = mapping.pv_uuid;
    range.source_path = mapping.source_path;
    range.source_offset_bytes = mapping.source_offset_bytes + within_mapping;
    range.length_bytes = chunk;
    physical_ranges.push_back(range);
    cursor += chunk;
  }

  if (cursor != request_end) {
    return Result<std::vector<PhysicalIoRange>>::failure("logical I/O range is not fully mapped");
  }

  return Result<std::vector<PhysicalIoRange>>::success(physical_ranges);
}

}  // namespace winlvm
