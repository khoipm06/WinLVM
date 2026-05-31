#pragma once

#include <string>
#include <vector>

#include "winlvm/model.h"
#include "winlvm/result.h"

namespace winlvm {

struct ScanOptions {
  std::vector<std::string> image_paths;
  std::vector<std::string> device_paths;
};

Result<ScanResult> scan_images(const ScanOptions& options);

}  // namespace winlvm
