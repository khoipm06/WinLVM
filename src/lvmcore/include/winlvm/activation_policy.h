#pragma once

#include <string>
#include <vector>

#include "winlvm/model.h"
#include "winlvm/result.h"

namespace winlvm {

struct ActivationPolicyRequest {
  bool readonly = true;
  bool force = false;
  bool allow_system_disk = false;
  bool allow_mounted_backing_volume = false;
};

struct ActivationPolicyDecision {
  bool allowed = false;
  std::vector<std::string> reasons;
  std::vector<std::string> warnings;
};

Result<ActivationPolicyDecision> evaluate_activation_policy(
    const ScanResult& scan_result,
    const VolumeGroup& vg,
    const LogicalVolume& lv,
    const ActivationPolicyRequest& request);

}  // namespace winlvm
