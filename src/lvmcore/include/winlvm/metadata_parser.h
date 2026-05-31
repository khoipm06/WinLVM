#pragma once

#include <string>

#include "winlvm/model.h"
#include "winlvm/result.h"

namespace winlvm {

Result<VolumeGroup> parse_vg_metadata_text(const std::string& text);

}  // namespace winlvm
