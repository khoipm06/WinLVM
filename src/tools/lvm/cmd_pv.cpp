#include "subcommands.h"
#include "cmd_common.h"
#include "lvmfmt/crc32.h"

#include <iomanip>
#include <iostream>
#include <set>

namespace lvmfmt {

namespace {

std::uint64_t align_up_u64(std::uint64_t val, std::uint64_t align) {
  if (align == 0) return val;
  const std::uint64_t rem = val % align;
  return rem ? (val + (align - rem)) : val;
}

}  // namespace

int cmd_pvcreate(const std::vector<std::string>& args) {
  if (check_help_requested(args, "pvcreate")) return 0;

  auto pos = get_positional_args(args);
  if (pos.empty()) {
    std::cerr << "error: pvcreate requires at least one device path\n";
    print_command_help("pvcreate");
    return 1;
  }

  const bool force = has_flag(args, "-f", "--force");
  PvCreateOptions opts;
  std::uint64_t align_sectors = 2048ULL;
  bool align_specified = false;

  if (auto uuid = get_opt_value(args, "-u", "--uuid")) {
    opts.custom_uuid = *uuid;
  }
  if (auto sz_str = get_opt_value(args, "--setphysicalvolumesize")) {
    std::string err;
    auto sz = parse_size_bytes(*sz_str, err);
    if (!sz || *sz == 0) {
      std::cerr << "error: invalid physical volume size: " << err << "\n";
      return 1;
    }
    opts.override_device_size = *sz;
  }
  if (auto align_str = get_opt_value(args, "--dataalignment")) {
    std::string err;
    auto align = parse_size_bytes(*align_str, err);
    if (!align || *align < 8192 || (*align % 512) != 0) {
      std::cerr << "error: invalid --dataalignment (must be >= 8 KiB and a multiple of 512 bytes)\n";
      return 1;
    }
    align_sectors = *align / 512ULL;
    opts.pe_start_sectors = align_sectors;
    align_specified = true;
  }
  if (auto mda_str = get_opt_value(args, "--metadatasize")) {
    std::string err;
    auto mda_bytes = parse_size_bytes(*mda_str, err);
    if (!mda_bytes || *mda_bytes < 4096) {
      std::cerr << "error: invalid --metadatasize (minimum 4 KiB)\n";
      return 1;
    }
    opts.mda_size_sectors = (*mda_bytes + 511ULL) / 512ULL;
    const std::uint64_t min_pe_start = 8ULL + opts.mda_size_sectors;
    if (opts.pe_start_sectors < min_pe_start) {
      opts.pe_start_sectors = align_up_u64(min_pe_start, align_specified ? align_sectors : 2048ULL);
    }
  }

  for (const auto& dev : pos) {
    std::string err;
    auto storage = open_storage_source(dev, true, err);
    if (!storage) {
      std::cerr << "error: cannot open device " << dev << ": " << err << "\n";
      return 1;
    }

    if (!force) {
      std::string probe_err;
      if (auto existing_label = read_pv_label(*storage, probe_err)) {
        if (auto existing_meta = read_vg_metadata(*storage, *existing_label, probe_err)) {
          if (auto existing_vg = parse_vg_metadata(*existing_meta, probe_err)) {
            std::cerr << "error: device \"" << dev << "\" already belongs to VG \""
                      << existing_vg->name << "\"; use -f/--force to overwrite\n";
            return 1;
          }
        }
      }
    }

    std::string gen_uuid;
    if (!pvcreate_initialize(*storage, opts, gen_uuid, err)) {
      std::cerr << "error: pvcreate failed on " << dev << ": " << err << "\n";
      return 1;
    }

    std::cout << "  Physical volume \"" << dev << "\" successfully created.\n";
  }

  return 0;
}

int cmd_pvremove(const std::vector<std::string>& args) {
  if (check_help_requested(args, "pvremove")) return 0;

  auto pos = get_positional_args(args);
  if (pos.empty()) {
    std::cerr << "error: pvremove requires at least one device path\n";
    print_command_help("pvremove");
    return 1;
  }

  const bool force = has_flag(args, "-f", "--force");
  for (const auto& dev : pos) {
    std::string err;
    auto storage = open_storage_source(dev, true, err);
    if (!storage) {
      std::cerr << "error: cannot open device " << dev << ": " << err << "\n";
      return 1;
    }

    if (!force) {
      std::string probe_err;
      if (auto label = read_pv_label(*storage, probe_err)) {
        if (auto meta = read_vg_metadata(*storage, *label, probe_err)) {
          if (auto vg = parse_vg_metadata(*meta, probe_err)) {
            std::cerr << "error: PV \"" << dev << "\" belongs to VG \"" << vg->name
                      << "\"; remove from VG first or pass -f/--force\n";
            return 1;
          }
        }
      }
    }

    if (!pvremove_wipe(*storage, err)) {
      std::cerr << "error: pvremove failed on " << dev << ": " << err << "\n";
      return 1;
    }

    std::cout << "  Labels on physical volume \"" << dev << "\" successfully wiped.\n";
  }

  return 0;
}

int cmd_pvs(const std::vector<std::string>& args) {
  if (check_help_requested(args, "pvs")) return 0;

  auto pos = get_positional_args(args);
  const bool noheadings = has_flag(args, "--noheadings");
  bool auto_scan = false;

#ifdef _WIN32
  if (pos.empty()) {
    auto_scan = true;
    for (const auto& c : discover_win32_candidate_devices()) {
      pos.push_back(c.open_path);
    }
  }
#else
  if (pos.empty()) {
    std::cerr << "error: pvs requires physical volume or image path(s) in direct mode\n";
    return 1;
  }
#endif

  if (!noheadings) {
    std::cout << "  PV         VG         Fmt  Attr PSize   PFree   PV UUID\n";
  }

  std::set<std::string> seen_pv_uuids;
  for (const auto& dev : pos) {
    std::string err;
    auto storage = open_storage_source(dev, false, err);
    if (!storage) {
      if (!auto_scan) std::cerr << "error: " << dev << ": " << err << "\n";
      continue;
    }

    auto label = read_pv_label(*storage, err);
    if (!label) {
      if (!auto_scan) std::cerr << "error: " << dev << ": " << err << "\n";
      continue;
    }
    const std::string raw_pv_uuid = normalize_uuid_raw32(label->pv_uuid);
    if (auto_scan && !seen_pv_uuids.insert(raw_pv_uuid).second) {
      continue;
    }

    std::string vg_name = "";
    std::string attr = "---";
    std::string free_str = format_size_human(label->device_size);
    auto meta = read_vg_metadata(*storage, *label, err);
    if (meta) {
      auto vg = parse_vg_metadata(*meta, err);
      if (vg) {
        vg_name = vg->name;
        const PhysicalVolume* pv_entry = vg->find_pv_by_uuid(label->pv_uuid);
        if (pv_entry) {
          attr[0] = pv_entry->is_allocatable() ? 'a' : '-';
          attr[1] = vg->is_exported() ? 'x' : '-';
          attr[2] = pv_entry->is_missing() ? 'm' : '-';
        }
        auto maps = build_vg_extent_maps(*vg);
        const std::string raw_uuid = normalize_uuid_raw32(label->pv_uuid);
        for (const auto& m : maps) {
          if (normalize_uuid_raw32(m.pv_uuid) == raw_uuid) {
            free_str = format_size_human(m.free_extents * vg->extent_size_sectors * 512ULL);
            break;
          }
        }
      }
    }

    std::cout << "  " << dev << "  " << (vg_name.empty() ? "[orphan]" : vg_name)
              << "  lvm2 " << attr << "  " << format_size_human(label->device_size)
              << "  " << free_str << "  " << format_uuid(label->pv_uuid) << "\n";
  }

  return 0;
}

int cmd_pvdisplay(const std::vector<std::string>& args) {
  if (check_help_requested(args, "pvdisplay")) return 0;

  auto pos = get_positional_args(args);
  bool auto_scan = false;
#ifdef _WIN32
  if (pos.empty()) {
    auto_scan = true;
    for (const auto& c : discover_win32_candidate_devices()) {
      pos.push_back(c.open_path);
    }
  }
#else
  if (pos.empty()) {
    std::cerr << "error: pvdisplay requires physical volume path(s)\n";
    return 1;
  }
#endif

  std::set<std::string> seen_pv_uuids;
  for (const auto& dev : pos) {
    std::string err;
    auto storage = open_storage_source(dev, false, err);
    if (!storage) {
      if (!auto_scan) std::cerr << "error: " << dev << ": " << err << "\n";
      continue;
    }

    auto label = read_pv_label(*storage, err);
    if (!label) {
      if (!auto_scan) std::cerr << "error: " << dev << ": " << err << "\n";
      continue;
    }
    const std::string raw_pv_uuid = normalize_uuid_raw32(label->pv_uuid);
    if (auto_scan && !seen_pv_uuids.insert(raw_pv_uuid).second) {
      continue;
    }

    std::cout << "  --- Physical volume ---\n"
              << "  PV Name               " << dev << "\n"
              << "  PV UUID               " << format_uuid(label->pv_uuid) << "\n"
              << "  PV Size               " << format_size_human(label->device_size) << "\n";

    auto meta = read_vg_metadata(*storage, *label, err);
    if (meta) {
      auto vg = parse_vg_metadata(*meta, err);
      if (vg) {
        const PhysicalVolume* pv_entry = vg->find_pv_by_uuid(label->pv_uuid);
        std::cout << "  VG Name               " << vg->name << "\n"
                  << "  VG UUID               " << format_uuid(vg->id) << "\n"
                  << "  Allocatable           "
                  << ((pv_entry && pv_entry->is_allocatable()) ? "yes" : "no") << "\n"
                  << "  PE Size               "
                  << format_size_human(vg->extent_size_sectors * 512ULL) << "\n";
        auto maps = build_vg_extent_maps(*vg);
        const std::string raw_uuid = normalize_uuid_raw32(label->pv_uuid);
        for (const auto& m : maps) {
          if (normalize_uuid_raw32(m.pv_uuid) == raw_uuid) {
            std::cout << "  Total PE              " << m.total_extents << "\n"
                      << "  Allocated PE          " << (m.total_extents - m.free_extents) << "\n"
                      << "  Free PE               " << m.free_extents << "\n";
            break;
          }
        }
      }
    }
    std::cout << "\n";
  }

  return 0;
}

int cmd_pvscan(const std::vector<std::string>& args) {
  if (check_help_requested(args, "pvscan")) return 0;

  auto pos = get_positional_args(args);
#ifdef _WIN32
  std::vector<Win32CandidateDevice> candidates;
  if (pos.empty()) {
    candidates = discover_win32_candidate_devices();
  } else {
    for (const auto& p : pos) {
      candidates.push_back({p, resolve_win32_probe_path(p)});
    }
  }

  driver_refresh();

  std::uint64_t total_pvs = 0;
  std::uint64_t total_bytes = 0;
  std::set<std::string> seen_pv_uuids;
  for (const auto& cand : candidates) {
    std::string err;
    auto storage = open_storage_source(cand.open_path, false, err);
    if (!storage) continue;

    auto label = read_pv_label(*storage, err);
    if (!label) continue;
    if (pos.empty() && !seen_pv_uuids.insert(normalize_uuid_raw32(label->pv_uuid)).second) {
      continue;
    }

    // Attach this physical/WinLUKS block device into winlvm.sys if online.
    driver_probe_device(cand.probe_path);

    total_pvs++;
    total_bytes += label->device_size;
    std::string vg_info = "is a new physical volume of";
    auto meta = read_vg_metadata(*storage, *label, err);
    if (meta) {
      auto vg = parse_vg_metadata(*meta, err);
      if (vg) vg_info = "VG " + vg->name + "   lvm2";
    }
    std::cout << "  PV " << cand.open_path << "   " << vg_info << " ["
              << format_size_human(label->device_size) << "]\n";
  }
  std::cout << "  Total: " << total_pvs << " [" << format_size_human(total_bytes) << "]\n";
  return 0;
#else
  if (pos.empty()) {
    std::cerr << "error: pvscan requires device or image path(s) in offline mode\n";
    return 1;
  }

  std::uint64_t total_pvs = 0;
  std::uint64_t total_bytes = 0;
  for (const auto& dev : pos) {
    std::string err;
    auto storage = open_storage_source(dev, false, err);
    if (!storage) continue;

    auto label = read_pv_label(*storage, err);
    if (!label) continue;

    total_pvs++;
    total_bytes += label->device_size;
    std::string vg_info = "is a new physical volume of";
    auto meta = read_vg_metadata(*storage, *label, err);
    if (meta) {
      auto vg = parse_vg_metadata(*meta, err);
      if (vg) vg_info = "VG " + vg->name + "   lvm2";
    }

    std::cout << "  PV " << dev << "   " << vg_info << " ["
              << format_size_human(label->device_size) << "]\n";
  }
  std::cout << "  Total: " << total_pvs << " [" << format_size_human(total_bytes) << "]\n";
  return 0;
#endif
}

int cmd_pvresize(const std::vector<std::string>& args) {
  if (check_help_requested(args, "pvresize")) return 0;

  auto pos = get_positional_args(args);
  if (pos.empty()) {
    std::cerr << "error: pvresize requires device path (and sibling VG PV paths if in a multi-PV VG)\n";
    return 1;
  }

  const std::string& dev = pos[0];
  std::string err;
  auto storage = open_storage_source(dev, true, err);
  if (!storage) {
    std::cerr << "error: cannot open " << dev << ": " << err << "\n";
    return 1;
  }

  auto label = read_pv_label(*storage, err);
  if (!label) {
    std::cerr << "error: cannot read PV label: " << err << "\n";
    return 1;
  }

  std::uint64_t new_size = storage->size();
  if (auto sz_str = get_opt_value(args, "--setphysicalvolumesize")) {
    auto sz = parse_size_bytes(*sz_str, err);
    if (!sz || *sz == 0) {
      std::cerr << "error: invalid size: " << err << "\n";
      return 1;
    }
    new_size = *sz;
  }
  if (storage->size() > 0 && new_size > storage->size()) {
    std::cerr << "error: requested PV size exceeds backing storage size\n";
    return 1;
  }

  const std::uint64_t pe_start_bytes =
      !label->data_areas.empty() ? label->data_areas[0].offset : (2048ULL * 512ULL);
  if (new_size <= pe_start_bytes) {
    std::cerr << "error: new PV size must be larger than data area start (" << pe_start_bytes
              << " bytes)\n";
    return 1;
  }

  std::string meta_err;
  auto meta = read_vg_metadata(*storage, *label, meta_err);
  const std::uint64_t old_size = label->device_size;
  const std::string target_raw_uuid = normalize_uuid_raw32(label->pv_uuid);

  if (meta && parse_vg_metadata(*meta, meta_err).has_value()) {
    // Release handle before load_vg_from_pvs opens all member PVs cleanly.
    storage.reset();

    auto loaded = load_vg_from_pvs(pos, true, err, true);
    if (!loaded) {
      std::cerr << "error: " << err << "\n";
      return 1;
    }
    if (!check_vg_mutable(loaded->vg, err)) {
      std::cerr << "error: " << err << "\n";
      return 1;
    }

    PhysicalVolume* target_pv = nullptr;
    for (auto& pv : loaded->vg.pvs) {
      if (normalize_uuid_raw32(pv.id) == target_raw_uuid) {
        target_pv = &pv;
        break;
      }
    }
    if (!target_pv) {
      std::cerr << "error: target PV not found in VG metadata\n";
      return 1;
    }

    const std::uint64_t new_dev_sectors = new_size / 512ULL;
    if (new_dev_sectors <= target_pv->pe_start_sectors) {
      std::cerr << "error: new PV size leaves zero space for physical extents\n";
      return 1;
    }
    const std::uint64_t new_pe_count =
        (new_dev_sectors - target_pv->pe_start_sectors) / loaded->vg.extent_size_sectors;
    if (new_pe_count == 0) {
      std::cerr << "error: new PV size fits zero extents\n";
      return 1;
    }

    auto maps = build_vg_extent_maps(loaded->vg);
    for (const auto& m : maps) {
      if (m.pv_key == target_pv->key) {
        for (std::uint64_t pe = new_pe_count; pe < m.total_extents; ++pe) {
          if (m.extent_used[static_cast<std::size_t>(pe)]) {
            std::cerr << "error: cannot shrink PV below allocated physical extent " << pe << "\n";
            return 1;
          }
        }
      }
    }

    ByteSource* target_storage = nullptr;
    for (std::size_t i = 0; i < loaded->pv_labels.size(); ++i) {
      if (normalize_uuid_raw32(loaded->pv_labels[i].pv_uuid) == target_raw_uuid) {
        target_storage = loaded->storages[i].get();
        break;
      }
    }
    if (!target_storage || !pv_update_device_size(*target_storage, new_size, err)) {
      std::cerr << "error: failed to update PV label size: " << err << "\n";
      return 1;
    }

    target_pv->dev_size_sectors = new_dev_sectors;
    target_pv->pe_count = new_pe_count;
    loaded->vg.seqno++;

    if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
      std::cerr << "error: failed to commit updated VG metadata after pvresize: " << err << "\n";
      return 1;
    }
  } else {
    if (!pv_update_device_size(*storage, new_size, err)) {
      std::cerr << "error: failed to update PV label size: " << err << "\n";
      return 1;
    }
  }

  std::cout << "  Physical volume \"" << dev << "\" resized from " << format_size_human(old_size)
            << " to " << format_size_human(new_size) << ".\n";
  return 0;
}

int cmd_pvchange(const std::vector<std::string>& args) {
  if (check_help_requested(args, "pvchange")) return 0;
  std::cerr << "error: pvchange is not supported by this version of WinLVM\n";
  return 1;
}

int cmd_pvmove(const std::vector<std::string>& args) {
  if (check_help_requested(args, "pvmove")) return 0;

  auto pos = get_positional_args(args);
  if (pos.size() < 2) {
    std::cerr << "error: pvmove requires SourcePV and DestinationPV paths\n";
    print_command_help("pvmove");
    return 1;
  }

  const std::string& src_path = pos[0];
  const std::string& dst_path = pos[1];
  auto target_lv = get_opt_value(args, "-n", "--name");

  std::string err;
  auto loaded = load_vg_from_pvs(pos, true, err, true);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  if (!check_vg_mutable(loaded->vg, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  std::size_t src_idx = loaded->pv_paths.size();
  std::size_t dst_idx = loaded->pv_paths.size();
  for (std::size_t i = 0; i < loaded->pv_paths.size(); ++i) {
    if (loaded->pv_paths[i] == src_path) src_idx = i;
    if (loaded->pv_paths[i] == dst_path) dst_idx = i;
  }
  if (src_idx >= loaded->storages.size() || dst_idx >= loaded->storages.size()) {
    std::cerr << "error: source and destination PVs must be distinct members of the same VG\n";
    return 1;
  }

  std::optional<std::string_view> lv_opt;
  if (target_lv) lv_opt = *target_lv;

  std::cout << "  Moving extents from " << src_path << " to " << dst_path << "...\n";
  if (!move_pv_extents(loaded->vg,
                       loaded->pv_labels[src_idx].pv_uuid,
                       loaded->pv_labels[dst_idx].pv_uuid,
                       lv_opt,
                       loaded->storages[src_idx].get(),
                       loaded->storages[dst_idx].get(),
                       err)) {
    std::cerr << "error: pvmove failed: " << err << "\n";
    return 1;
  }

  if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
    std::cerr << "error: failed to commit updated metadata after pvmove: " << err << "\n";
    return 1;
  }

  std::cout << "  Physical volume \"" << src_path << "\" extents successfully moved.\n";
  return 0;
}

int cmd_pvck(const std::vector<std::string>& args) {
  if (check_help_requested(args, "pvck")) return 0;

  auto pos = get_positional_args(args);
  if (pos.empty()) {
    std::cerr << "error: pvck requires physical volume path\n";
    return 1;
  }

  for (const auto& dev : pos) {
    std::string err;
    auto storage = open_storage_source(dev, false, err);
    if (!storage) {
      std::cerr << "error: cannot open " << dev << ": " << err << "\n";
      return 1;
    }

    auto label = read_pv_label(*storage, err);
    if (!label) {
      std::cerr << "error: corrupt or missing PV label on " << dev << ": " << err << "\n";
      return 1;
    }

    if (label->metadata_areas.empty()) {
      std::cerr << "error: no metadata area found on " << dev << "\n";
      return 1;
    }

    for (const auto& area : label->metadata_areas) {
      auto raw_mda = read_mda_header_raw(*storage, area, err);
      if (!raw_mda) {
        std::cerr << "error: invalid MDA header on " << dev << " at offset " << area.offset
                  << ": " << err << "\n";
        return 1;
      }

      if ((raw_mda->commit_flags & kRawLocnIgnored) != 0 || raw_mda->commit_size == 0) {
        continue;
      }

      auto mda = read_mda_header(*storage, area, err);
      if (!mda) {
        std::cerr << "error: invalid committed MDA descriptor on " << dev << " at offset "
                  << area.offset << ": " << err << "\n";
        return 1;
      }

      auto text = read_metadata_text(*storage, *mda, err);
      if (!text) {
        std::cerr << "error: corrupt metadata text on " << dev << " at offset " << area.offset
                  << ": " << err << "\n";
        return 1;
      }

      auto vg = parse_vg_metadata(*text, err);
      if (!vg) {
        std::cerr << "error: invalid VG metadata grammar on " << dev << " at offset "
                  << area.offset << ": " << err << "\n";
        return 1;
      }

      if (!vg->find_pv_by_uuid(label->pv_uuid)) {
        std::cerr << "error: PV UUID " << format_uuid(label->pv_uuid)
                  << " is not listed in its own VG \"" << vg->name << "\" metadata\n";
        return 1;
      }
    }

    std::cout << "  Physical volume \"" << dev << "\" label and MDA checksums verified OK.\n";
  }

  return 0;
}

}  // namespace lvmfmt
