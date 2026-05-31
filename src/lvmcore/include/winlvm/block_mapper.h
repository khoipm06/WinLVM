#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "winlvm/model.h"
#include "winlvm/result.h"

namespace winlvm {

struct PhysicalIoRange {
  std::string pv_uuid;
  std::string source_path;
  std::uint64_t source_offset_bytes = 0;
  std::uint64_t length_bytes = 0;
};

struct LogicalIoRequest {
  std::uint64_t offset_bytes = 0;
  std::uint64_t length_bytes = 0;
  bool write = false;
};

Result<std::vector<PhysicalIoRange>> map_logical_io(
    const std::vector<MappingRange>& mappings,
    std::uint64_t lv_size_bytes,
    const LogicalIoRequest& request);

}  // namespace winlvm
