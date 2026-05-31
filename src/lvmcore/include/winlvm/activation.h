#pragma once

#include <memory>
#include <string>
#include <vector>

#include "winlvm/mapping_table.h"
#include "winlvm/model.h"
#include "winlvm/result.h"

namespace winlvm {

// ============================================================================
// Activation Service Interface
//
// The activation service manages LV lifecycle:
// - Receives requests from CLI
// - Validates topology against fail-closed rules
// - Builds mapping tables
// - Communicates with kernel driver
// - Tracks active LVs
// ============================================================================

struct ActivationRequest {
  std::string vg_name;
  std::string lv_name;
  bool readonly = true;
  bool force = false;
  // Future: system_id claim, metadata mutation flags
};

struct ActivationResponse {
  bool success = false;
  std::string error_message;
  std::uint64_t activation_id = 0;  // Unique ID for this activation
};

struct DeactivationRequest {
  std::string vg_name;
  std::string lv_name;
  bool force = false;
};

struct DeactivationResponse {
  bool success = false;
  std::string error_message;
};

// ============================================================================
// Activation State Tracker (for local registry in service)
// ============================================================================
struct ActivationRecord {
  std::uint64_t id;
  std::string vg_name;
  std::string lv_name;
  MappingUuid lv_uuid;
  std::uint64_t lv_size_bytes;
  bool readonly;
  std::uint64_t timestamp_created;
  std::uint64_t driver_handle;  // Reference returned from kernel driver
};

// ============================================================================
// Activation Manager Interface (runs in service process)
// ============================================================================
class ActivationManager {
 public:
  virtual ~ActivationManager() = default;

  // Validate and activate an LV from scanned topology
  virtual Result<ActivationResponse> activate_logical_volume(
      const ScanResult& scan_result,
      const ActivationRequest& request) = 0;

  // Deactivate an active LV
  virtual Result<DeactivationResponse> deactivate_logical_volume(
      const DeactivationRequest& request) = 0;

  // List currently active LVs
  virtual std::vector<ActivationRecord> list_active_volumes() const = 0;

  // Find activation record by VG/LV name
  virtual ActivationRecord* find_activation(const std::string& vg_name,
                                             const std::string& lv_name) = 0;
};

// ============================================================================
// Factory: Create activation manager instance
// ============================================================================
std::unique_ptr<ActivationManager> create_activation_manager();

}  // namespace winlvm
