#include "winlvm/activation_policy.h"

#include <algorithm>

namespace winlvm {

namespace {

bool has_error_diagnostic(const ScanResult& scan_result) {
  return std::any_of(scan_result.diagnostics.begin(), scan_result.diagnostics.end(), [](const Diagnostic& d) {
    return d.severity == "error";
  });
}

}  // namespace

Result<ActivationPolicyDecision> evaluate_activation_policy(
    const ScanResult& scan_result,
    const VolumeGroup& vg,
    const LogicalVolume& lv,
    const ActivationPolicyRequest& request) {
  ActivationPolicyDecision decision;

  if (scan_result.pvs.empty()) {
    decision.reasons.push_back("no physical volumes discovered");
  }

  if (has_error_diagnostic(scan_result)) {
    decision.reasons.push_back("scan contains error diagnostics");
  }

  if (!lv.visible) {
    decision.reasons.push_back("logical volume is hidden/internal");
  }

  if (!lv.supported) {
    decision.reasons.push_back("logical volume uses unsupported features: " + lv.unsupported_reason);
  }

  if (lv.segments.empty()) {
    decision.reasons.push_back("logical volume has no mapped segments");
  }

  if (!request.readonly && !request.force) {
    decision.warnings.push_back("read-write activation requires explicit force in this prototype");
    decision.reasons.push_back("read-write activation is not enabled without --force");
  }

  if (!request.readonly) {
    decision.warnings.push_back("read-write activation must lock/offline all backing PVs before enabling writes");
    decision.warnings.push_back("read-write activation must propagate flush/FUA to backing PVs");
  }

  (void)vg;
  decision.allowed = decision.reasons.empty();
  return Result<ActivationPolicyDecision>::success(decision);
}

}  // namespace winlvm
