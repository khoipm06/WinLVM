#include "subcommands.h"
#include "cmd_common.h"
#include "lvmfmt/mapping.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <set>

namespace lvmfmt {

namespace {

std::string generate_vg_uuid() {
  static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<std::size_t> dis(0, sizeof(chars) - 2);

  std::string s;
  s.reserve(32);
  for (int i = 0; i < 32; ++i) s.push_back(chars[dis(gen)]);
  return s;
}

std::string format_vg_attr(const VolumeGroup& vg) {
  std::string attr = "wz--n-";
  attr[0] = vg.is_read_only() ? 'r' : 'w';
  attr[1] = vg.is_resizeable() ? 'z' : '-';
  attr[2] = vg.is_exported() ? 'x' : '-';
  bool any_missing = false;
  for (const auto& pv : vg.pvs) {
    if (pv.is_missing()) any_missing = true;
  }
  attr[3] = any_missing ? 'p' : '-';
  return attr;
}

std::size_t count_visible_lvs(const VolumeGroup& vg) {
  std::size_t count = 0;
  for (const auto& lv : vg.lvs) {
    if (!lv.internal) count++;
  }
  return count;
}

void print_vg_row(const VolumeGroup& vg) {
  auto maps = build_vg_extent_maps(vg);
  std::uint64_t total_extents = 0;
  std::uint64_t free_extents = 0;
  for (const auto& m : maps) {
    total_extents += m.total_extents;
    free_extents += m.free_extents;
  }
  const std::uint64_t ext_bytes = vg.extent_size_sectors * 512ULL;
  std::cout << "  " << vg.name << "  " << vg.pvs.size() << "   " << count_visible_lvs(vg)
            << "   0 " << format_vg_attr(vg) << " "
            << format_size_human(total_extents * ext_bytes) << " "
            << format_size_human(free_extents * ext_bytes) << "\n";
}

std::string next_unused_pv_key(const VolumeGroup& vg) {
  for (std::size_t idx = 0; ; ++idx) {
    std::string candidate = "pv" + std::to_string(idx);
    if (!vg.find_pv(candidate)) return candidate;
  }
}

}  // namespace

int cmd_vgcreate(const std::vector<std::string>& args) {
  if (check_help_requested(args, "vgcreate")) return 0;

  auto pos = get_positional_args(args);
  if (pos.size() < 2) {
    std::cerr << "error: vgcreate requires VGName and at least one PV device path\n";
    print_command_help("vgcreate");
    return 1;
  }

  const std::string& vg_name = pos[0];
  std::string err;
  if (!validate_lvm_name(vg_name, "volume group", err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  const bool force = has_flag(args, "-f", "--force");
  std::vector<std::string> pv_paths(pos.begin() + 1, pos.end());

  std::uint64_t extent_size_bytes = 4ULL * 1024ULL * 1024ULL;
  if (auto s_str = get_opt_value(args, "-s", "--physicalextentsize")) {
    auto sz = parse_size_bytes(*s_str, err);
    if (!sz || *sz < 4096 || (*sz % 512) != 0) {
      std::cerr << "error: invalid physical extent size: " << err << "\n";
      return 1;
    }
    extent_size_bytes = *sz;
  }
  const std::uint64_t extent_size_sectors = extent_size_bytes / 512ULL;

  VolumeGroup vg;
  vg.name = vg_name;
  vg.id = generate_vg_uuid();
  vg.seqno = 1;
  vg.extent_size_sectors = extent_size_sectors;
  vg.status = {"RESIZEABLE", "READ", "WRITE"};

  std::vector<std::unique_ptr<ByteSource>> storages;
  std::vector<ByteSource*> raw_storages;
  std::set<std::string> seen_uuids;

  std::size_t pv_index = 0;
  for (const auto& path : pv_paths) {
    auto storage = open_storage_source(path, true, err);
    if (!storage) {
      std::cerr << "error: cannot open PV " << path << ": " << err << "\n";
      return 1;
    }

    auto label = read_pv_label(*storage, err);
    if (!label) {
      std::cerr << "error: device " << path << " does not have a valid PV label: " << err << "\n";
      return 1;
    }

    const std::string raw_uuid = normalize_uuid_raw32(label->pv_uuid);
    if (!seen_uuids.insert(raw_uuid).second) {
      std::cerr << "error: duplicate physical volume specified: " << path << "\n";
      return 1;
    }

    if (!force) {
      std::string probe_err;
      if (auto meta = read_vg_metadata(*storage, *label, probe_err)) {
        if (auto existing = parse_vg_metadata(*meta, probe_err)) {
          std::cerr << "error: PV " << path << " already belongs to VG \"" << existing->name
                    << "\"; use -f/--force to override\n";
          return 1;
        }
      }
    }

    PhysicalVolume pv;
    pv.key = "pv" + std::to_string(pv_index++);
    pv.id = label->pv_uuid;
    pv.device = path;
    pv.status = {"ALLOCATABLE"};
    pv.dev_size_sectors = label->device_size / 512ULL;
    pv.pe_start_sectors =
        (!label->data_areas.empty() && label->data_areas[0].offset > 0)
            ? (label->data_areas[0].offset / 512ULL)
            : 2048ULL;
    if (pv.dev_size_sectors <= pv.pe_start_sectors ||
        (pv.dev_size_sectors - pv.pe_start_sectors) < extent_size_sectors) {
      std::cerr << "error: PV " << path << " is too small for extent size "
                << format_size_human(extent_size_bytes) << "\n";
      return 1;
    }
    pv.pe_count = (pv.dev_size_sectors - pv.pe_start_sectors) / extent_size_sectors;

    vg.pvs.push_back(std::move(pv));
    raw_storages.push_back(storage.get());
    storages.push_back(std::move(storage));
  }

  if (!commit_vg_metadata(vg, raw_storages, err)) {
    std::cerr << "error: failed to commit VG metadata: " << err << "\n";
    return 1;
  }

  std::cout << "  Volume group \"" << vg_name << "\" successfully created\n";
  return 0;
}

int cmd_vgremove(const std::vector<std::string>& args) {
  if (check_help_requested(args, "vgremove")) return 0;

  auto pos = get_positional_args(args);
  if (pos.empty()) {
    std::cerr << "error: vgremove requires VGName and member PV path(s)\n";
    print_command_help("vgremove");
    return 1;
  }

  const std::string& vg_name = pos[0];
  std::vector<std::string> pv_paths(pos.begin() + 1, pos.end());
#ifdef _WIN32
  if (pv_paths.empty()) {
    pv_paths = discover_win32_pv_paths_for_vg(vg_name);
  }
#endif
  if (pv_paths.empty()) {
    std::cerr << "error: vgremove requires VGName and member PV path(s)\n";
    return 1;
  }

  std::string err;
  auto loaded = load_vg_from_pvs(pv_paths, true, err, true);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  if (loaded->vg.name != vg_name) {
    std::cerr << "error: specified VG name \"" << vg_name << "\" does not match found VG \""
              << loaded->vg.name << "\"\n";
    return 1;
  }

#ifdef _WIN32
  for (const auto& lv : loaded->vg.lvs) {
    if (!lv.internal) driver_activate_lv(lv.id, false);
  }
#endif

  for (std::size_t i = 0; i < loaded->storages.size(); ++i) {
    if (!pvremove_wipe(*loaded->storages[i], err)) {
      std::cerr << "error: failed to wipe PV " << loaded->pv_paths[i] << ": " << err << "\n";
      return 1;
    }
  }

#ifdef _WIN32
  driver_refresh();
#endif

  std::cout << "  Volume group \"" << vg_name << "\" successfully removed\n";
  return 0;
}

int cmd_vgs(const std::vector<std::string>& args) {
  if (check_help_requested(args, "vgs")) return 0;

  auto pos = get_positional_args(args);
  const bool noheadings = has_flag(args, "--noheadings");

#ifdef _WIN32
  if (pos.empty()) {
    std::map<std::string, VolumeGroup> vgs_by_uuid;
    for (const auto& cand : discover_win32_candidate_devices()) {
      std::string err;
      auto src = open_storage_source(cand.open_path, false, err);
      if (!src) continue;
      auto label = read_pv_label(*src, err);
      if (!label) continue;
      auto meta = read_vg_metadata(*src, *label, err);
      if (!meta) continue;
      auto vg = parse_vg_metadata(*meta, err);
      if (!vg) continue;
      const std::string key = normalize_uuid_raw32(vg->id);
      auto it = vgs_by_uuid.find(key);
      if (it == vgs_by_uuid.end() || vg->seqno > it->second.seqno) {
        vgs_by_uuid[key] = std::move(*vg);
      }
    }
    if (!noheadings) {
      std::cout << "  VG         #PV #LV #SN Attr   VSize    VFree\n";
    }
    for (const auto& kv : vgs_by_uuid) print_vg_row(kv.second);
    return 0;
  }
#else
  if (pos.empty()) {
    std::cerr << "error: vgs requires member PV path(s)\n";
    return 1;
  }
#endif

  std::string err;
  auto loaded = load_vg_flexible(pos, false, err, false);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  if (!noheadings) {
    std::cout << "  VG         #PV #LV #SN Attr   VSize    VFree\n";
  }
  print_vg_row(loaded->vg);
  return 0;
}

int cmd_vgdisplay(const std::vector<std::string>& args) {
  if (check_help_requested(args, "vgdisplay")) return 0;

  auto pos = get_positional_args(args);
#ifdef _WIN32
  if (pos.empty()) {
    pos = discover_win32_pv_paths_for_vg("");
  }
#endif
  if (pos.empty()) {
    std::cerr << "error: vgdisplay requires member PV path(s)\n";
    return 1;
  }

  std::string err;
  auto loaded = load_vg_flexible(pos, false, err, false);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  auto maps = build_vg_extent_maps(loaded->vg);
  std::uint64_t total_extents = 0;
  std::uint64_t free_extents = 0;
  for (const auto& m : maps) {
    total_extents += m.total_extents;
    free_extents += m.free_extents;
  }
  const std::uint64_t ext_bytes = loaded->vg.extent_size_sectors * 512ULL;
  std::size_t mda_total = 0;
  for (const auto& lbl : loaded->pv_labels) mda_total += lbl.metadata_areas.size();
  if (mda_total == 0) mda_total = loaded->vg.pvs.size();

  std::cout << "  --- Volume group ---\n"
            << "  VG Name               " << loaded->vg.name << "\n"
            << "  System ID             " << loaded->vg.system_id << "\n"
            << "  Format                lvm2\n"
            << "  Metadata Areas        " << mda_total << "\n"
            << "  Metadata Sequence No  " << loaded->vg.seqno << "\n"
            << "  VG Access             " << (loaded->vg.is_read_only() ? "read-only" : "read/write") << "\n"
            << "  VG Status             "
            << (loaded->vg.is_exported() ? "exported/resizable" : "resizable") << "\n"
            << "  MAX LV                " << loaded->vg.max_lv << "\n"
            << "  Cur LV                " << count_visible_lvs(loaded->vg) << "\n"
            << "  Max PV                " << loaded->vg.max_pv << "\n"
            << "  Cur PV                " << loaded->vg.pvs.size() << "\n"
            << "  Act PV                " << loaded->vg.pvs.size() << "\n"
            << "  VG Size               " << format_size_human(total_extents * ext_bytes) << "\n"
            << "  PE Size               " << format_size_human(ext_bytes) << "\n"
            << "  Total PE              " << total_extents << "\n"
            << "  Alloc PE / Size       " << (total_extents - free_extents) << " / "
            << format_size_human((total_extents - free_extents) * ext_bytes) << "\n"
            << "  Free  PE / Size       " << free_extents << " / "
            << format_size_human(free_extents * ext_bytes) << "\n"
            << "  VG UUID               " << format_uuid(loaded->vg.id) << "\n\n";
  return 0;
}

int cmd_vgscan(const std::vector<std::string>& args) {
  if (check_help_requested(args, "vgscan")) return 0;

  auto pos = get_positional_args(args);
#ifdef _WIN32
  std::vector<Win32CandidateDevice> candidates;
  if (pos.empty()) {
    candidates = discover_win32_candidate_devices();
  } else {
    for (const auto& p : pos) candidates.push_back({p, resolve_win32_probe_path(p)});
  }

  std::set<std::string> found_vgs;
  for (const auto& cand : candidates) {
    std::string err;
    auto src = open_storage_source(cand.open_path, false, err);
    if (!src) continue;
    auto label = read_pv_label(*src, err);
    if (!label) continue;

    driver_probe_device(cand.probe_path);

    auto meta = read_vg_metadata(*src, *label, err);
    if (!meta) continue;
    auto vg = parse_vg_metadata(*meta, err);
    if (vg && found_vgs.insert(normalize_uuid_raw32(vg->id)).second) {
      std::cout << "  Found volume group \"" << vg->name << "\" using metadata type lvm2\n";
    }
  }
  driver_refresh();
  return 0;
#else
  if (pos.empty()) {
    std::cerr << "error: vgscan requires device or image path(s) in offline mode\n";
    return 1;
  }
  std::string err;
  auto loaded = load_vg_from_pvs(pos, false, err, false);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  std::cout << "  Found volume group \"" << loaded->vg.name << "\" using metadata type lvm2\n";
  return 0;
#endif
}

int cmd_vgchange(const std::vector<std::string>& args) {
  if (check_help_requested(args, "vgchange")) return 0;

  bool has_act = false;
  bool has_perm = false;
  std::string err;
  auto act_opt = parse_activate_flag(args, has_act, err);
  if (has_act && !act_opt) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  auto perm_opt = parse_permission_flag(args, has_perm, err);
  if (has_perm && !perm_opt) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  if (!has_act && !has_perm) {
    std::cerr << "error: vgchange requires -a/--activate (y|n) or -p/--permission (r|rw)\n";
    return 1;
  }

  auto pos = get_positional_args(args);
  if (pos.empty()) {
    std::cerr << "error: vgchange requires VGName and/or member PV path(s)\n";
    return 1;
  }

  auto loaded = load_vg_flexible(pos, has_perm, err, has_perm);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  if (has_perm) {
    if (!check_vg_mutable(loaded->vg, err, false, true)) {
      std::cerr << "error: " << err << "\n";
      return 1;
    }
    if (loaded->vg.status.empty()) {
      loaded->vg.status = {"RESIZEABLE", "READ", "WRITE"};
    }
    set_status_flag(loaded->vg.status, "READ", true);
    set_status_flag(loaded->vg.status, "WRITE", *perm_opt);
    loaded->vg.seqno++;
    if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
      std::cerr << "error: failed to commit permission change: " << err << "\n";
      return 1;
    }
#ifdef _WIN32
    driver_refresh();
#endif
    std::cout << "  Volume group \"" << loaded->vg.name << "\" permission changed to "
              << (*perm_opt ? "read/write" : "read-only") << "\n";
  }

  if (has_act) {
#ifdef _WIN32
    std::size_t changed = 0;
    for (const auto& lv : loaded->vg.lvs) {
      if (lv.internal || !lv.supported) continue;
      if (!driver_activate_lv(lv.id, *act_opt)) {
        std::cerr << "error: failed to " << (*act_opt ? "activate" : "deactivate")
                  << " LV \"" << lv.name << "\" via WinLVM driver\n";
        return 1;
      }
      changed++;
    }
    std::cout << "  " << changed << " logical volume(s) in volume group \"" << loaded->vg.name
              << "\" now " << (*act_opt ? "active" : "inactive") << "\n";
#else
    std::cerr << "error: online LV activation/deactivation requires the WinLVM kernel driver on Windows\n";
    return 1;
#endif
  }

  return 0;
}

int cmd_vgextend(const std::vector<std::string>& args) {
  if (check_help_requested(args, "vgextend")) return 0;

  auto pos = get_positional_args(args);
#ifdef _WIN32
  if (pos.size() < 2) {
    std::cerr << "error: vgextend requires VGName and at least one new PV path\n";
    print_command_help("vgextend");
    return 1;
  }
#else
  if (pos.size() < 3) {
    std::cerr << "error: vgextend requires VGName, existing member PV(s), and new PV path(s)\n";
    print_command_help("vgextend");
    return 1;
  }
#endif

  const std::string& vg_name = pos[0];
  const bool force = has_flag(args, "-f", "--force");
  std::vector<std::string> existing_paths;
  std::vector<std::string> new_paths;

  for (std::size_t i = 1; i < pos.size(); ++i) {
    std::string err;
    auto src = open_storage_source(pos[i], false, err);
    if (!src) {
      std::cerr << "error: cannot open " << pos[i] << ": " << err << "\n";
      return 1;
    }
    auto label = read_pv_label(*src, err);
    if (!label) {
      std::cerr << "error: device " << pos[i] << " has no valid PV label: " << err << "\n";
      return 1;
    }
    std::string meta_err;
    auto meta = read_vg_metadata(*src, *label, meta_err);
    if (meta) {
      auto vg = parse_vg_metadata(*meta, meta_err);
      if (vg && vg->name == vg_name) {
        existing_paths.push_back(pos[i]);
        continue;
      }
      if (vg && !force) {
        std::cerr << "error: PV " << pos[i] << " already belongs to VG \"" << vg->name << "\"\n";
        return 1;
      }
    }
    new_paths.push_back(pos[i]);
  }

#ifdef _WIN32
  if (existing_paths.empty()) {
    existing_paths = discover_win32_pv_paths_for_vg(vg_name);
  }
#endif
  if (existing_paths.empty() || new_paths.empty()) {
    std::cerr << "error: vgextend requires existing member PV(s) of \"" << vg_name
              << "\" and at least one new PV\n";
    return 1;
  }

  std::string err;
  auto loaded = load_vg_from_pvs(existing_paths, true, err, true);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  if (loaded->vg.name != vg_name) {
    std::cerr << "error: VG name mismatch (expected \"" << vg_name << "\", found \""
              << loaded->vg.name << "\")\n";
    return 1;
  }
  if (!check_vg_mutable(loaded->vg, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  for (const auto& new_pv : new_paths) {
    auto new_storage = open_storage_source(new_pv, true, err);
    if (!new_storage) {
      std::cerr << "error: cannot open new PV " << new_pv << ": " << err << "\n";
      return 1;
    }
    auto new_label = read_pv_label(*new_storage, err);
    if (!new_label) {
      std::cerr << "error: device " << new_pv << " has no valid PV label: " << err << "\n";
      return 1;
    }
    if (loaded->vg.find_pv_by_uuid(new_label->pv_uuid)) {
      std::cerr << "error: PV " << new_pv << " is already a member of VG \"" << vg_name << "\"\n";
      return 1;
    }

    PhysicalVolume pv;
    pv.key = next_unused_pv_key(loaded->vg);
    pv.id = new_label->pv_uuid;
    pv.device = new_pv;
    pv.status = {"ALLOCATABLE"};
    pv.dev_size_sectors = new_label->device_size / 512ULL;
    pv.pe_start_sectors =
        (!new_label->data_areas.empty() && new_label->data_areas[0].offset > 0)
            ? (new_label->data_areas[0].offset / 512ULL)
            : 2048ULL;
    if (pv.dev_size_sectors <= pv.pe_start_sectors ||
        (pv.dev_size_sectors - pv.pe_start_sectors) < loaded->vg.extent_size_sectors) {
      std::cerr << "error: new PV " << new_pv << " is too small for VG extent size\n";
      return 1;
    }
    pv.pe_count = (pv.dev_size_sectors - pv.pe_start_sectors) / loaded->vg.extent_size_sectors;

    loaded->vg.pvs.push_back(std::move(pv));
    loaded->storages.push_back(std::move(new_storage));
    loaded->pv_paths.push_back(new_pv);
    loaded->pv_labels.push_back(std::move(*new_label));
  }

  loaded->vg.seqno++;
  if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
    std::cerr << "error: failed to commit updated VG metadata: " << err << "\n";
    return 1;
  }

  std::cout << "  Volume group \"" << vg_name << "\" successfully extended\n";
  return 0;
}

int cmd_vgreduce(const std::vector<std::string>& args) {
  if (check_help_requested(args, "vgreduce")) return 0;

  const bool remove_all = has_flag(args, "-a", "--all");
  auto pos = get_positional_args(args);
  if (pos.size() < 2) {
    std::cerr << "error: vgreduce requires VGName and PV path(s)\n";
    print_command_help("vgreduce");
    return 1;
  }

  const std::string& vg_name = pos[0];
  std::vector<std::string> pv_paths(pos.begin() + 1, pos.end());

  std::string err;
  auto loaded = load_vg_from_pvs(pv_paths, true, err, true);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  if (loaded->vg.name != vg_name) {
    std::cerr << "error: VG name mismatch (expected \"" << vg_name << "\", found \""
              << loaded->vg.name << "\")\n";
    return 1;
  }
  if (!check_vg_mutable(loaded->vg, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  auto maps = build_vg_extent_maps(loaded->vg);
  std::set<std::string> remove_raw_uuids;

  if (remove_all) {
    std::size_t remaining = loaded->vg.pvs.size();
    for (const auto& m : maps) {
      if (m.free_extents == m.total_extents && remaining > 1) {
        remove_raw_uuids.insert(normalize_uuid_raw32(m.pv_uuid));
        remaining--;
      }
    }
    if (remove_raw_uuids.empty()) {
      std::cerr << "error: no removable empty physical volumes found in VG \"" << vg_name << "\"\n";
      return 1;
    }
  } else {
    // pos[1] is the PV to remove; locate its UUID in loaded->pv_paths.
    const std::string& target_path = pos[1];
    std::string target_uuid;
    for (std::size_t i = 0; i < loaded->pv_paths.size(); ++i) {
      if (loaded->pv_paths[i] == target_path) {
        target_uuid = normalize_uuid_raw32(loaded->pv_labels[i].pv_uuid);
        break;
      }
    }
    if (target_uuid.empty()) {
      std::cerr << "error: PV \"" << target_path << "\" is not in VG \"" << vg_name << "\"\n";
      return 1;
    }
    if (loaded->vg.pvs.size() <= 1) {
      std::cerr << "error: cannot remove the last PV from VG \"" << vg_name
                << "\"; use vgremove instead\n";
      return 1;
    }
    for (const auto& m : maps) {
      if (normalize_uuid_raw32(m.pv_uuid) == target_uuid) {
        if (m.free_extents != m.total_extents) {
          std::cerr << "error: physical volume \"" << target_path
                    << "\" still has allocated extents (" << (m.total_extents - m.free_extents)
                    << " used); run pvmove first\n";
          return 1;
        }
        break;
      }
    }
    remove_raw_uuids.insert(target_uuid);
  }

  std::vector<PhysicalVolume> kept_pvs;
  for (const auto& pv : loaded->vg.pvs) {
    if (!remove_raw_uuids.count(normalize_uuid_raw32(pv.id))) {
      kept_pvs.push_back(pv);
    }
  }
  loaded->vg.pvs = std::move(kept_pvs);
  loaded->vg.seqno++;

  std::vector<ByteSource*> remaining_storages;
  std::vector<ByteSource*> removed_storages;
  for (std::size_t i = 0; i < loaded->storages.size(); ++i) {
    if (remove_raw_uuids.count(normalize_uuid_raw32(loaded->pv_labels[i].pv_uuid))) {
      removed_storages.push_back(loaded->storages[i].get());
    } else {
      remaining_storages.push_back(loaded->storages[i].get());
    }
  }

  if (!commit_vg_metadata(loaded->vg, remaining_storages, err)) {
    std::cerr << "error: failed to commit updated VG metadata: " << err << "\n";
    return 1;
  }
  for (ByteSource* removed : removed_storages) {
    if (!pv_clear_vg_metadata(*removed, err)) {
      std::cerr << "error: failed to clear VG metadata on removed PV: " << err << "\n";
      return 1;
    }
  }

  std::cout << "  Removed " << removed_storages.size() << " physical volume(s) from volume group \""
            << vg_name << "\"\n";
  return 0;
}

int cmd_vgrename(const std::vector<std::string>& args) {
  if (check_help_requested(args, "vgrename")) return 0;

  auto pos = get_positional_args(args);
  if (pos.size() < 2) {
    std::cerr << "error: vgrename requires OldVG NewVG [PVPath...]\n";
    print_command_help("vgrename");
    return 1;
  }

  const std::string& old_name = pos[0];
  const std::string& new_name = pos[1];
  std::string err;
  if (!validate_lvm_name(new_name, "volume group", err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  std::vector<std::string> pvs(pos.begin() + 2, pos.end());
#ifdef _WIN32
  if (pvs.empty()) pvs = discover_win32_pv_paths_for_vg(old_name);
#endif
  if (pvs.empty()) {
    std::cerr << "error: vgrename requires OldVG NewVG PVPath...\n";
    return 1;
  }

  auto loaded = load_vg_from_pvs(pvs, true, err, true);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  if (loaded->vg.name != old_name) {
    std::cerr << "error: VG name mismatch (expected \"" << old_name << "\", found \""
              << loaded->vg.name << "\")\n";
    return 1;
  }
  if (!check_vg_mutable(loaded->vg, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  loaded->vg.name = new_name;
  loaded->vg.seqno++;

  if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
    std::cerr << "error: failed to commit rename: " << err << "\n";
    return 1;
  }

  std::cout << "  Volume group \"" << old_name << "\" successfully renamed to \"" << new_name
            << "\"\n";
  return 0;
}

int cmd_vgcfgbackup(const std::vector<std::string>& args) {
  if (check_help_requested(args, "vgcfgbackup")) return 0;

  auto pos = get_positional_args(args, true);
  auto backup_file = get_opt_value(args, "-f", "--file");
  if (!backup_file) backup_file = "vg_backup.vg";

  if (pos.empty()) {
    std::cerr << "error: vgcfgbackup requires member PV paths\n";
    return 1;
  }

  std::string err;
  auto loaded = load_vg_flexible(pos, false, err, false);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  if (!backup_vg_metadata(loaded->vg, *backup_file, err)) {
    std::cerr << "error: backup failed: " << err << "\n";
    return 1;
  }

  std::cout << "  Volume group \"" << loaded->vg.name << "\" successfully backed up to "
            << *backup_file << "\n";
  return 0;
}

int cmd_vgcfgrestore(const std::vector<std::string>& args) {
  if (check_help_requested(args, "vgcfgrestore")) return 0;

  auto backup_file = get_opt_value(args, "-f", "--file");
  if (!backup_file) {
    std::cerr << "error: vgcfgrestore requires backup file via -f\n";
    return 1;
  }

  auto pos = get_positional_args(args, true);
  if (pos.empty()) {
    std::cerr << "error: vgcfgrestore requires target PV paths\n";
    return 1;
  }

  std::ifstream in(*backup_file, std::ios::binary);
  if (!in) {
    std::cerr << "error: cannot open backup file " << *backup_file << "\n";
    return 1;
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();

  std::string err;
  auto vg_opt = parse_vg_metadata(text, err);
  if (!vg_opt) {
    std::cerr << "error: failed to parse backup metadata: " << err << "\n";
    return 1;
  }

  std::vector<std::string> pv_targets;
  if (pos.size() >= 2 && pos[0] == vg_opt->name) {
    pv_targets.assign(pos.begin() + 1, pos.end());
  } else {
    // Verify VGName if pos[0] is not a readable PV file.
    std::string probe_err;
    if (pos.size() >= 2 && !open_storage_source(pos[0], false, probe_err)) {
      std::cerr << "error: backup VG name \"" << vg_opt->name
                << "\" does not match requested VG \"" << pos[0] << "\"\n";
      return 1;
    }
    pv_targets = pos;
  }

  std::vector<std::unique_ptr<ByteSource>> storages;
  std::vector<ByteSource*> raw_storages;
  std::set<std::string> opened_uuids;

  for (const auto& path : pv_targets) {
    auto st = open_storage_source(path, true, err);
    if (!st) {
      std::cerr << "error: cannot open " << path << ": " << err << "\n";
      return 1;
    }
    auto lbl = read_pv_label(*st, err);
    if (!lbl) {
      std::cerr << "error: target PV " << path << " has no valid PV label: " << err << "\n";
      return 1;
    }
    if (!vg_opt->find_pv_by_uuid(lbl->pv_uuid)) {
      std::cerr << "error: target PV " << path << " (" << format_uuid(lbl->pv_uuid)
                << ") is not a member of backed-up VG \"" << vg_opt->name << "\"\n";
      return 1;
    }
    opened_uuids.insert(normalize_uuid_raw32(lbl->pv_uuid));
    raw_storages.push_back(st.get());
    storages.push_back(std::move(st));
  }

  for (const auto& pv : vg_opt->pvs) {
    if (pv.is_missing()) continue;
    if (!opened_uuids.count(normalize_uuid_raw32(pv.id))) {
      std::cerr << "error: missing member PV " << pv.key << " (" << format_uuid(pv.id)
                << ") required for vgcfgrestore\n";
      return 1;
    }
  }

  if (!commit_vg_metadata(*vg_opt, raw_storages, err)) {
    std::cerr << "error: restore failed: " << err << "\n";
    return 1;
  }

  std::cout << "  Restored volume group \"" << vg_opt->name << "\" from " << *backup_file << "\n";
  return 0;
}

int cmd_vgck(const std::vector<std::string>& args) {
  if (check_help_requested(args, "vgck")) return 0;

  auto pos = get_positional_args(args);
#ifdef _WIN32
  if (pos.empty()) pos = discover_win32_pv_paths_for_vg("");
#endif
  if (pos.empty()) {
    std::cerr << "error: vgck requires member PV paths\n";
    return 1;
  }

  // Strip optional leading VG name if pos[0] is not a readable path.
  std::string expected_vg;
  std::vector<std::string> pv_paths = pos;
  if (pos.size() >= 2) {
    std::string probe_err;
    if (!open_storage_source(pos[0], false, probe_err)) {
      expected_vg = pos[0];
      pv_paths.assign(pos.begin() + 1, pos.end());
    }
  }

  std::optional<VolumeGroup> ref_vg;
  std::set<std::string> present_uuids;

  for (const auto& path : pv_paths) {
    std::string err;
    auto src = open_storage_source(path, false, err);
    if (!src) {
      std::cerr << "error: cannot open " << path << ": " << err << "\n";
      return 1;
    }
    auto label = read_pv_label(*src, err);
    if (!label) {
      std::cerr << "error: invalid PV label on " << path << ": " << err << "\n";
      return 1;
    }
    present_uuids.insert(normalize_uuid_raw32(label->pv_uuid));

    for (const auto& area : label->metadata_areas) {
      auto raw_mda = read_mda_header_raw(*src, area, err);
      if (!raw_mda) {
        std::cerr << "error: invalid MDA header on " << path << ": " << err << "\n";
        return 1;
      }
      if ((raw_mda->commit_flags & kRawLocnIgnored) != 0 || raw_mda->commit_size == 0) continue;

      auto mda = read_mda_header(*src, area, err);
      if (!mda) {
        std::cerr << "error: invalid MDA descriptor on " << path << ": " << err << "\n";
        return 1;
      }
      auto text = read_metadata_text(*src, *mda, err);
      if (!text) {
        std::cerr << "error: corrupt metadata CRC on " << path << ": " << err << "\n";
        return 1;
      }
      auto vg = parse_vg_metadata(*text, err);
      if (!vg) {
        std::cerr << "error: invalid metadata grammar on " << path << ": " << err << "\n";
        return 1;
      }
      if (!ref_vg) {
        ref_vg = std::move(*vg);
      } else {
        if (normalize_uuid_raw32(vg->id) != normalize_uuid_raw32(ref_vg->id) ||
            vg->name != ref_vg->name) {
          std::cerr << "error: VG identity mismatch on " << path << " (got \"" << vg->name
                    << "\", expected \"" << ref_vg->name << "\")\n";
          return 1;
        }
        if (vg->seqno != ref_vg->seqno) {
          std::cerr << "error: VG \"" << ref_vg->name << "\" seqno mismatch on " << path
                    << " (found " << vg->seqno << ", expected " << ref_vg->seqno << ")\n";
          return 1;
        }
      }
    }
  }

  if (!ref_vg) {
    std::cerr << "error: no committed VG metadata found across specified PV(s)\n";
    return 1;
  }
  if (!expected_vg.empty() && ref_vg->name != expected_vg) {
    std::cerr << "error: VG name mismatch (expected \"" << expected_vg << "\", found \""
              << ref_vg->name << "\")\n";
    return 1;
  }

  // Verify no missing PVs.
  for (const auto& pv : ref_vg->pvs) {
    if (pv.is_missing()) {
      std::cerr << "error: VG \"" << ref_vg->name << "\" has PV " << pv.key
                << " marked MISSING\n";
      return 1;
    }
    if (!present_uuids.count(normalize_uuid_raw32(pv.id))) {
      std::cerr << "error: VG \"" << ref_vg->name << "\" is missing PV " << pv.key << " ("
                << format_uuid(pv.id) << ")\n";
      return 1;
    }
  }

  // Validate LV segment bounds and detect any double-allocated physical extents.
  std::map<std::string, std::vector<bool>> pe_used;
  for (const auto& pv : ref_vg->pvs) {
    pe_used[pv.key].assign(static_cast<std::size_t>(pv.pe_count), false);
  }
  for (const auto& lv : ref_vg->lvs) {
    if (lv.internal) continue;
    if (lv.supported) {
      std::string map_err;
      if (!build_dm_table(*ref_vg, lv, map_err)) {
        std::cerr << "error: LV \"" << lv.name << "\" failed segment validation: " << map_err
                  << "\n";
        return 1;
      }
      for (const auto& seg : lv.segments) {
        const std::uint64_t sc = (seg.stripe_count == 0) ? 1ULL : seg.stripe_count;
        const std::uint64_t per_stripe = seg.extent_count / sc;
        for (const auto& st : seg.stripes) {
          auto& used = pe_used[st.target];
          for (std::uint64_t e = 0; e < per_stripe; ++e) {
            const std::size_t idx = static_cast<std::size_t>(st.start_extent + e);
            if (idx >= used.size() || used[idx]) {
              std::cerr << "error: overlapping or out-of-bounds physical extent " << idx
                        << " on " << st.target << " in LV \"" << lv.name << "\"\n";
              return 1;
            }
            used[idx] = true;
          }
        }
      }
    }
  }

  std::cout << "  Volume group \"" << ref_vg->name << "\" consistency check OK.\n";
  return 0;
}

int cmd_vgexport(const std::vector<std::string>& args) {
  if (check_help_requested(args, "vgexport")) return 0;

  auto pos = get_positional_args(args);
  if (pos.empty()) {
    std::cerr << "error: vgexport requires VGName and member PV path(s)\n";
    return 1;
  }

  std::string err;
  auto loaded = load_vg_flexible(pos, true, err, true);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  if (loaded->vg.is_exported()) {
    std::cerr << "error: volume group \"" << loaded->vg.name << "\" is already exported\n";
    return 1;
  }

#ifdef _WIN32
  for (const auto& lv : loaded->vg.lvs) {
    if (!lv.internal) driver_activate_lv(lv.id, false);
  }
#endif

  if (loaded->vg.status.empty()) {
    loaded->vg.status = {"RESIZEABLE", "READ", "WRITE"};
  }
  set_status_flag(loaded->vg.status, "EXPORTED", true);
  loaded->vg.seqno++;

  if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
    std::cerr << "error: failed to commit vgexport: " << err << "\n";
    return 1;
  }

  std::cout << "  Volume group \"" << loaded->vg.name << "\" successfully exported\n";
  return 0;
}

int cmd_vgimport(const std::vector<std::string>& args) {
  if (check_help_requested(args, "vgimport")) return 0;

  auto pos = get_positional_args(args);
  if (pos.empty()) {
    std::cerr << "error: vgimport requires VGName and member PV path(s)\n";
    return 1;
  }

  std::string err;
  auto loaded = load_vg_flexible(pos, true, err, true);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  if (!loaded->vg.is_exported()) {
    std::cerr << "error: volume group \"" << loaded->vg.name << "\" is not exported\n";
    return 1;
  }

  set_status_flag(loaded->vg.status, "EXPORTED", false);
  loaded->vg.seqno++;

  if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
    std::cerr << "error: failed to commit vgimport: " << err << "\n";
    return 1;
  }

  std::cout << "  Volume group \"" << loaded->vg.name << "\" successfully imported\n";
  return 0;
}

}  // namespace lvmfmt
