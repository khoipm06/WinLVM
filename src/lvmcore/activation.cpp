#include "winlvm/activation.h"
#include "winlvm/activation_policy.h"
#include "winlvm/driver_client.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <span>
#include <sstream>

namespace winlvm {

namespace {

// ============================================================================
// Helper: Convert LVM UUID text to a deterministic 128-bit driver token.
//
// LVM UUIDs are not Windows GUID text and may contain non-hex characters. The
// driver only needs stable equality tokens inside one validated mapping table,
// so hash the original text instead of dropping characters or padding.
// ============================================================================
MappingUuid string_to_mapping_uuid(const std::string& uuid_str) {
  MappingUuid mu{};
  std::uint64_t first = 14695981039346656037ULL;
  std::uint64_t second = 1099511628211ULL;
  for (unsigned char c : uuid_str) {
    first ^= c;
    first *= 1099511628211ULL;
    second ^= static_cast<unsigned char>(c + 0x9dU);
    second *= 14029467366897019727ULL;
  }
  for (std::size_t i = 0; i < 8; ++i) {
    mu.bytes[i] = static_cast<std::uint8_t>((first >> (i * 8U)) & 0xffU);
    mu.bytes[i + 8] = static_cast<std::uint8_t>((second >> (i * 8U)) & 0xffU);
  }
  return mu;
}

// ============================================================================
// Concrete ActivationManager Implementation
// ============================================================================
class ActivationManagerImpl : public ActivationManager {
 public:
  ActivationManagerImpl() = default;

  Result<ActivationResponse> activate_logical_volume(
      const ScanResult& scan_result,
      const ActivationRequest& request) override {

    // 1. Locate the target volume group
    const VolumeGroup* target_vg = nullptr;
    for (const auto& vg : scan_result.vgs) {
      if (vg.name == request.vg_name) {
        target_vg = &vg;
        break;
      }
    }

    if (!target_vg) {
      return Result<ActivationResponse>::failure("Volume group '" + request.vg_name + "' not found");
    }

    // 2. Locate the target logical volume inside VG
    const LogicalVolume* target_lv = nullptr;
    for (const auto& lv : target_vg->lvs) {
      if (lv.name == request.lv_name) {
        target_lv = &lv;
        break;
      }
    }

    if (!target_lv) {
      return Result<ActivationResponse>::failure("Logical volume '" + request.lv_name + "' not found in VG '" + request.vg_name + "'");
    }

    // 3. Strict Safety Validation (Fail Closed Rules)
    if (!target_lv->supported) {
      return Result<ActivationResponse>::failure("Cannot activate '" + request.vg_name + "/" + request.lv_name + "': " + target_lv->unsupported_reason);
    }

    if (!target_lv->visible) {
      return Result<ActivationResponse>::failure("Cannot activate internal/hidden logical volume '" + request.vg_name + "/" + request.lv_name + "'");
    }

    // Check if duplicate PV UUID diagnostic is present
    for (const auto& diag : scan_result.diagnostics) {
      if (diag.code == "duplicate_pv_uuid") {
        return Result<ActivationResponse>::failure("Cannot activate: Duplicate Physical Volume UUID detected");
      }
    }

    ActivationPolicyRequest policy_request;
    policy_request.readonly = request.readonly;
    policy_request.force = request.force;
    auto policy = evaluate_activation_policy(scan_result, *target_vg, *target_lv, policy_request);
    if (!policy) {
      return Result<ActivationResponse>::failure(policy.status.message);
    }
    if (!policy.value.allowed) {
      std::string message = "activation policy denied '" + request.vg_name + "/" + request.lv_name + "'";
      for (const std::string& reason : policy.value.reasons) {
        message += "; " + reason;
      }
      return Result<ActivationResponse>::failure(message);
    }

    // Check if already active
    if (find_activation(request.vg_name, request.lv_name)) {
      return Result<ActivationResponse>::failure("Logical volume '" + request.vg_name + "/" + request.lv_name + "' is already active");
    }

    // 4. Gather relevant physical mappings and compile MappingTable
    std::vector<MappingRange> lv_mappings;
    for (const auto& mapping : scan_result.mappings) {
      if (mapping.vg_name == request.vg_name && mapping.lv_name == request.lv_name) {
        lv_mappings.push_back(mapping);
      }
    }

    if (lv_mappings.empty()) {
      return Result<ActivationResponse>::failure("No physical mappings found for logical volume '" + request.vg_name + "/" + request.lv_name + "'");
    }

    // Sort mappings strictly by logical start bytes for safety
    std::sort(lv_mappings.begin(), lv_mappings.end(), [](const MappingRange& a, const MappingRange& b) {
      return a.lv_start_bytes < b.lv_start_bytes;
    });

    // Check contiguity in logical block mappings
    std::uint64_t expected_next_byte = 0;
    for (const auto& mapping : lv_mappings) {
      if (mapping.lv_start_bytes != expected_next_byte) {
        return Result<ActivationResponse>::failure("Mapping gap detected. Logical volume is not contiguous");
      }
      expected_next_byte += mapping.length_bytes;
    }

    if (expected_next_byte != target_lv->size_bytes) {
      return Result<ActivationResponse>::failure("Mapping size mismatch. Expected total size " +
                                                 std::to_string(target_lv->size_bytes) +
                                                 " but mapped size is " + std::to_string(expected_next_byte));
    }

    // Gather unique physical volumes mapped by this logical volume
    std::vector<PhysicalVolume> mapped_pvs;
    for (const auto& mapping : lv_mappings) {
      bool already_added = false;
      for (const auto& added : mapped_pvs) {
        if (added.uuid == mapping.pv_uuid) {
          already_added = true;
          break;
        }
      }
      if (!already_added) {
        // Find in scan_result physical volumes
        const PhysicalVolume* found_pv = nullptr;
        for (const auto& pv : scan_result.pvs) {
          if (pv.uuid == mapping.pv_uuid) {
            found_pv = &pv;
            break;
          }
        }
        if (found_pv) {
          mapped_pvs.push_back(*found_pv);
        } else {
          return Result<ActivationResponse>::failure("Physical Volume reference not found for UUID: " + mapping.pv_uuid);
        }
      }
    }

    // 5. Construct Mapping Table ABI Buffer
    std::uint32_t segment_count = static_cast<std::uint32_t>(lv_mappings.size());
    std::uint32_t pv_count = static_cast<std::uint32_t>(mapped_pvs.size());

    std::size_t header_size = sizeof(MappingTableHeader);
    std::size_t segments_size = segment_count * sizeof(MappingSegment);
    std::size_t pv_handles_size = pv_count * sizeof(MappingPvHandle);
    std::size_t total_buf_size = header_size + segments_size + pv_handles_size;

    std::vector<std::uint8_t> abibuf(total_buf_size, 0);

    MappingTableHeader* hdr = reinterpret_cast<MappingTableHeader*>(abibuf.data());
    hdr->magic = kMappingTableMagic;
    hdr->version = kMappingTableVersion;
    hdr->flags = 0;
    if (request.readonly) {
      hdr->flags |= kMappingFlagReadOnly;
    }
    if (request.force) {
      hdr->flags |= kMappingFlagForce;
    }
    hdr->total_size_bytes = total_buf_size;
    hdr->vg_uuid = string_to_mapping_uuid(target_vg->uuid);
    hdr->lv_uuid = string_to_mapping_uuid(target_lv->uuid);

    std::memcpy(hdr->vg_name, request.vg_name.data(), std::min(request.vg_name.size(), kMaxVgNameLength));
    std::memcpy(hdr->lv_name, request.lv_name.data(), std::min(request.lv_name.size(), kMaxLvNameLength));
    hdr->lv_size_bytes = target_lv->size_bytes;
    hdr->exposed_logical_sector_size = 512;  // Expose conservative sector size
    hdr->exposed_physical_sector_size = 4096;
    hdr->segment_count = segment_count;
    hdr->pv_handle_count = pv_count;
    hdr->segments_offset = header_size;
    hdr->pv_handles_offset = header_size + segments_size;

    // Populate Segment Array
    MappingSegment* seg_array = reinterpret_cast<MappingSegment*>(abibuf.data() + hdr->segments_offset);
    for (std::size_t i = 0; i < segment_count; ++i) {
      const auto& mapping = lv_mappings[i];
      seg_array[i].type = MappingSegmentType::kLinear;
      seg_array[i].data.linear.lv_start_bytes = mapping.lv_start_bytes;
      seg_array[i].data.linear.length_bytes = mapping.length_bytes;
      seg_array[i].data.linear.pv_uuid = string_to_mapping_uuid(mapping.pv_uuid);
      seg_array[i].data.linear.pv_start_bytes = mapping.pv_start_bytes;
    }

    // Populate Physical Volume handles
    MappingPvHandle* pv_array = reinterpret_cast<MappingPvHandle*>(abibuf.data() + hdr->pv_handles_offset);
    for (std::size_t i = 0; i < pv_count; ++i) {
      const auto& pv = mapped_pvs[i];
      if (pv.source_path.empty() || pv.source_path.size() > kMaxSourcePathLength) {
        return Result<ActivationResponse>::failure("Physical Volume source path is missing or too long for mapping table ABI");
      }
      if (pv.source_size_bytes == 0 || pv.device_size_bytes == 0) {
        return Result<ActivationResponse>::failure("Physical Volume source size is invalid for mapping table ABI");
      }
      pv_array[i].pv_uuid = string_to_mapping_uuid(pv.uuid);
      std::memcpy(pv_array[i].source_path, pv.source_path.data(),
                  pv.source_path.size());
      pv_array[i].source_size_bytes = pv.source_size_bytes;
      pv_array[i].pv_base_offset_bytes = pv.pv_base_offset_bytes;
      pv_array[i].pv_size_bytes = pv.device_size_bytes;
      pv_array[i].logical_sector_size = 512;
      pv_array[i].physical_sector_size = 4096;
      pv_array[i].driver_handle = 0;  // Populated by driver during activation
    }

    // Verify ABI buffer is valid
    if (!validate_mapping_table_header(*hdr)) {
      return Result<ActivationResponse>::failure("Generated mapping table header validation failed");
    }

    // 6. Transmit to Virtual/Kernel Block Driver Client
    // Since kernel sys is not yet built, we interface with a Virtual/Simulated Activation endpoint
    // to record activation and simulate driver handle allocation.
    std::uint64_t simulated_driver_handle = simulated_driver_activate(abibuf);

    if (simulated_driver_handle == 0) {
      return Result<ActivationResponse>::failure("Kernel driver refused mapping table activation");
    }

    // 7. Track activation state locally
    std::uint64_t act_id = generate_id();
    ActivationRecord record{};
    record.id = act_id;
    record.vg_name = request.vg_name;
    record.lv_name = request.lv_name;
    record.lv_uuid = hdr->lv_uuid;
    record.lv_size_bytes = target_lv->size_bytes;
    record.readonly = request.readonly;
    record.timestamp_created = get_timestamp();
    record.driver_handle = simulated_driver_handle;

    active_records_[act_id] = record;

    ActivationResponse response{};
    response.success = true;
    response.activation_id = act_id;
    return Result<ActivationResponse>::success(response);
  }

  Result<DeactivationResponse> deactivate_logical_volume(
      const DeactivationRequest& request) override {

    // Find active record
    ActivationRecord* target = find_activation(request.vg_name, request.lv_name);
    if (!target) {
      return Result<DeactivationResponse>::failure("Logical volume '" + request.vg_name + "/" + request.lv_name + "' is not active");
    }

    // In a real implementation, we query driver to check if volume is busy.
    // If busy, we fail unless request.force is set.
    bool busy = false;  // Simulation: not busy

    if (busy && !request.force) {
      return Result<DeactivationResponse>::failure("Cannot deactivate logical volume '" +
                                                   request.vg_name + "/" + request.lv_name +
                                                   "': Volume is currently busy/mounted");
    }

    // Query driver to remove device
    bool driver_removed = simulated_driver_deactivate(target->driver_handle);
    if (!driver_removed && !request.force) {
      return Result<DeactivationResponse>::failure("Kernel driver failed to release virtual mapping device");
    }

    // Erase activation record
    active_records_.erase(target->id);

    DeactivationResponse response{};
    response.success = true;
    return Result<DeactivationResponse>::success(response);
  }

  std::vector<ActivationRecord> list_active_volumes() const override {
    std::vector<ActivationRecord> records;
    for (const auto& pair : active_records_) {
      records.push_back(pair.second);
    }
    return records;
  }

  ActivationRecord* find_activation(const std::string& vg_name,
                                     const std::string& lv_name) override {
    for (auto& pair : active_records_) {
      if (pair.second.vg_name == vg_name && pair.second.lv_name == lv_name) {
        return &pair.second;
      }
    }
    return nullptr;
  }

 private:
  std::map<std::uint64_t, ActivationRecord> active_records_;
  std::uint64_t next_id_ = 1;

  std::uint64_t generate_id() { return next_id_++; }

  std::uint64_t get_timestamp() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  }

  // ==========================================================================
  // Driver activation mock interfaces (to bridge to sys and simulation)
  // ==========================================================================
  std::uint64_t simulated_driver_activate(const std::vector<std::uint8_t>& abibuf) {
    const MappingTableHeader* hdr = reinterpret_cast<const MappingTableHeader*>(abibuf.data());

#ifdef _WIN32
    auto client = DriverClient::open_default();
    if (client) {
      auto mapping = client.value.create_mapping(
          std::span<const std::uint8_t>(abibuf.data(), abibuf.size()));
      if (!mapping) {
        std::cerr << "  [winlvm] Real driver activation failed: "
                  << mapping.status.message << "\n";
        return 0;
      }
      std::cout << "  [winlvm] Real driver activation succeeded. mappingId="
                << mapping.value << "\n";
      return mapping.value;
    }
#endif

    // Simulate Windows IOCTL Device Creation.
    // During production driver development, this writes to `\\.\lvmdm` control device.
    // Here we generate a unique handle from magic and size
    std::uint64_t handle = 0xABCD000000000000ULL + abibuf.size() + hdr->lv_size_bytes;
    return handle;
  }

  bool simulated_driver_deactivate(std::uint64_t handle) {
#ifdef _WIN32
    // If this is a real driver handle, deactivate it via IOCTL
    if ((handle & 0xFFFF000000000000ULL) != 0xABCD000000000000ULL) {
      auto client = DriverClient::open_default();
      if (client) {
        return client.value.delete_mapping(handle).ok;
      }
      return false;
    }
#endif

    // In production, transmits IOCTL_LVMDM_REMOVE_DEVICE to physical driver.
    return (handle != 0);
  }
};

}  // namespace

std::unique_ptr<ActivationManager> create_activation_manager() {
  return std::make_unique<ActivationManagerImpl>();
}

}  // namespace winlvm
