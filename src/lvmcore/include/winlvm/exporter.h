#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "winlvm/result.h"
#include "winlvm/scanner.h"

namespace winlvm {

struct ExportOptions {
  ScanOptions scan_options;
  std::string target;
  std::string output_path;
  std::uint64_t offset_bytes = 0;
  std::optional<std::uint64_t> length_bytes;
  bool readonly = true;
};

Result<std::uint64_t> export_logical_volume(const ExportOptions& options);

}  // namespace winlvm
