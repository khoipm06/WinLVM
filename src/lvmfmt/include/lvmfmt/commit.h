#pragma once

#include <cstdint>
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
    std::uint64_t pe_start_sectors = 2048; // default 1 MiB
    std::uint64_t mda_size_sectors = 2048; // default 1 MiB
};

// Initialize an LVM2 physical volume on the given storage source.
bool pvcreate_initialize(ByteSource& storage,
                         const PvCreateOptions& options,
                         std::string& generated_uuid,
                         std::string& error);

// Wipe LVM2 label and headers from the storage source.
bool pvremove_wipe(ByteSource& storage, std::string& error);

// Commit an updated VolumeGroup metadata to all of its member PV storage sources.
bool commit_vg_metadata(const VolumeGroup& vg,
                        const std::vector<ByteSource*>& pv_storages,
                        std::string& error);

// Generate LVM2 metadata text representation of the volume group.
std::string serialize_vg_to_text(const VolumeGroup& vg);

// Save VG configuration to backup archive file.
bool backup_vg_metadata(const VolumeGroup& vg,
                        std::string_view filepath,
                        std::string& error);

// Restore VG configuration from backup archive file.
bool restore_vg_metadata(std::string_view filepath,
                         const std::vector<ByteSource*>& pv_storages,
                         std::string& error);

} // namespace lvmfmt
