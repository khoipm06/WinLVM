#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lvmfmt/metadata.h"
#include "lvmfmt/source.h"

namespace lvmfmt {

// Options for pvcreate.
struct PvCreateOptions {
  std::string custom_uuid;
  std::uint64_t override_device_size = 0;
  std::uint64_t pe_start_sectors = 2048;   // default 1 MiB
  std::uint64_t mda_size_sectors = 2048;   // default fills up to pe_start (1 MiB - 4 KiB)
};

// Initialize an LVM2 physical volume on the given storage source.
bool pvcreate_initialize(ByteSource& storage,
                         const PvCreateOptions& options,
                         std::string& generated_uuid,
                         std::string& error);

// Wipe LVM2 label and headers from the storage source.
bool pvremove_wipe(ByteSource& storage, std::string& error);

// Clear committed VG metadata descriptors from all non-ignored MDAs on a PV
// while preserving its LABELONE PV header (returns PV to orphan state).
bool pv_clear_vg_metadata(ByteSource& storage, std::string& error);

// Update the device_size field in the PV label sector and recompute its CRC.
bool pv_update_device_size(ByteSource& storage,
                           std::uint64_t new_size_bytes,
                           std::string& error);

// Commit an updated VolumeGroup metadata to all non-ignored MDAs on all member PVs
// using the two-phase circular-ring precommit/commit protocol.
bool commit_vg_metadata(const VolumeGroup& vg,
                        const std::vector<ByteSource*>& pv_storages,
                        std::string& error);

// Generate LVM2 metadata text representation of the volume group.
// Fails closed if any LV uses an unsupported or internal segment layout.
std::optional<std::string> serialize_vg_to_text(const VolumeGroup& vg, std::string& error);

// Save VG configuration to backup archive file.
bool backup_vg_metadata(const VolumeGroup& vg,
                        std::string_view filepath,
                        std::string& error);

// Restore VG configuration from backup archive file.
bool restore_vg_metadata(std::string_view filepath,
                         const std::vector<ByteSource*>& pv_storages,
                         std::string& error);

}  // namespace lvmfmt
