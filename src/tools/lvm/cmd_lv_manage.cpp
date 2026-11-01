#include "subcommands.h"
#include "cmd_common.h"

#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lvmfmt {

namespace {

std::string build_lv_attr(const VolumeGroup& vg, const LogicalVolume& lv, bool is_active) {
  std::string attr = "-wi-------";
  attr[1] = (vg.is_read_only() || lv.is_read_only()) ? 'r' : 'w';
  attr[4] = is_active ? 'a' : '-';
  return attr;
}

#ifdef _WIN32
std::set<std::string> query_active_lv_uuids() {
  std::set<std::string> active;
  std::vector<WINLVM_INVENTORY_LV_ENTRY> entries;
  std::uint32_t records = 0;
  if (driver_query_inventory(entries, records)) {
    for (const auto& e : entries) {
      if ((e.flags & WINLVM_INV_LV_ACTIVE) != 0) {
        active.insert(normalize_uuid_raw32(std::string(e.lv_uuid, strnlen(e.lv_uuid, sizeof(e.lv_uuid)))));
      }
    }
  }
  return active;
}

std::vector<VolumeGroup> discover_all_win32_vgs() {
  std::map<std::string, VolumeGroup> by_uuid;
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
    auto it = by_uuid.find(key);
    if (it == by_uuid.end() || vg->seqno > it->second.seqno) {
      by_uuid[key] = std::move(*vg);
    }
  }
  std::vector<VolumeGroup> out;
  for (auto& kv : by_uuid) out.push_back(std::move(kv.second));
  return out;
}
#endif

}  // namespace

int cmd_lvremove(const std::vector<std::string>& args) {
  if (check_help_requested(args, "lvremove")) return 0;

  auto pos = get_positional_args(args);
  if (pos.empty()) {
    std::cerr << "error: lvremove requires [VGName/]LVName and member PV path(s)\n";
    print_command_help("lvremove");
    return 1;
  }

  auto [vg_hint, lv_name] = split_vg_lv_spec(pos[0]);
  std::vector<std::string> pv_paths(pos.begin() + 1, pos.end());
#ifdef _WIN32
  if (pv_paths.empty()) pv_paths = discover_win32_pv_paths_for_vg(vg_hint);
#endif
  if (pv_paths.empty()) {
    std::cerr << "error: lvremove requires member PV path(s)\n";
    print_command_help("lvremove");
    return 1;
  }

  std::string err;
  auto loaded = load_vg_flexible(pv_paths, true, err, true);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  if (!vg_hint.empty() && loaded->vg.name != vg_hint) {
    std::cerr << "error: VG name mismatch (expected \"" << vg_hint << "\", found \""
              << loaded->vg.name << "\")\n";
    return 1;
  }
  if (!check_vg_mutable(loaded->vg, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  bool found = false;
  std::string removed_uuid;
  std::vector<LogicalVolume> remaining_lvs;
  for (auto& lv : loaded->vg.lvs) {
    if (lv.name == lv_name) {
      found = true;
      removed_uuid = lv.id;
    } else {
      remaining_lvs.push_back(std::move(lv));
    }
  }
  if (!found) {
    std::cerr << "error: logical volume \"" << lv_name << "\" not found in VG "
              << loaded->vg.name << "\n";
    return 1;
  }

#ifdef _WIN32
  driver_activate_lv(removed_uuid, false);
#else
  (void)removed_uuid;
#endif

  loaded->vg.lvs = std::move(remaining_lvs);
  loaded->vg.seqno++;

  if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
    std::cerr << "error: failed to commit metadata: " << err << "\n";
    return 1;
  }

#ifdef _WIN32
  driver_refresh();
#endif

  std::cout << "  Logical volume \"" << lv_name << "\" successfully removed\n";
  return 0;
}

int cmd_lvs(const std::vector<std::string>& args) {
  if (check_help_requested(args, "lvs")) return 0;

  auto pos = get_positional_args(args);
  const bool noheadings = has_flag(args, "--noheadings");
  std::set<std::string> active_uuids;
#ifdef _WIN32
  active_uuids = query_active_lv_uuids();
  if (pos.empty()) {
    auto vgs = discover_all_win32_vgs();
    if (!noheadings) {
      std::cout << "  LV   VG   Attr       LSize\n";
    }
    for (const auto& vg : vgs) {
      const std::uint64_t ext_bytes = vg.extent_size_sectors * 512ULL;
      for (const auto& lv : vg.lvs) {
        if (lv.internal) continue;
        const bool active = active_uuids.count(normalize_uuid_raw32(lv.id)) > 0;
        std::cout << "  " << lv.name << " " << vg.name << " "
                  << build_lv_attr(vg, lv, active) << " "
                  << format_size_human(lv.extent_count() * ext_bytes) << "\n";
      }
    }
    return 0;
  }
#else
  if (pos.empty()) {
    std::cerr << "error: lvs requires member PV path(s)\n";
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
    std::cout << "  LV   VG   Attr       LSize\n";
  }
  const std::uint64_t ext_bytes = loaded->vg.extent_size_sectors * 512ULL;
  for (const auto& lv : loaded->vg.lvs) {
    if (lv.internal) continue;
    const bool active = active_uuids.count(normalize_uuid_raw32(lv.id)) > 0;
    std::cout << "  " << lv.name << " " << loaded->vg.name << " "
              << build_lv_attr(loaded->vg, lv, active) << " "
              << format_size_human(lv.extent_count() * ext_bytes) << "\n";
  }
  return 0;
}

int cmd_lvdisplay(const std::vector<std::string>& args) {
  if (check_help_requested(args, "lvdisplay")) return 0;

  auto pos = get_positional_args(args);
  std::set<std::string> active_uuids;
  std::vector<VolumeGroup> vgs;
  bool auto_scanned = false;

#ifdef _WIN32
  active_uuids = query_active_lv_uuids();
  if (pos.empty()) {
    auto_scanned = true;
    vgs = discover_all_win32_vgs();
  }
#endif

  if (!auto_scanned && vgs.empty()) {
    if (pos.empty()) {
      std::cerr << "error: lvdisplay requires member PV path(s)\n";
      return 1;
    }
    std::string err;
    auto loaded = load_vg_flexible(pos, false, err, false);
    if (!loaded) {
      std::cerr << "error: " << err << "\n";
      return 1;
    }
    vgs.push_back(std::move(loaded->vg));
  }

  for (const auto& vg : vgs) {
    const std::uint64_t ext_bytes = vg.extent_size_sectors * 512ULL;
    for (const auto& lv : vg.lvs) {
      if (lv.internal) continue;
      const bool active = active_uuids.count(normalize_uuid_raw32(lv.id)) > 0;
      const bool ro = vg.is_read_only() || lv.is_read_only();
      std::cout << "  --- Logical volume ---\n"
                << "  LV Path                /dev/" << vg.name << "/" << lv.name << "\n"
                << "  LV Name                " << lv.name << "\n"
                << "  VG Name                " << vg.name << "\n"
                << "  LV UUID                " << format_uuid(lv.id) << "\n"
                << "  LV Write Access        " << (ro ? "read only" : "read/write") << "\n"
                << "  LV Status              " << (active ? "available" : "NOT available") << "\n"
                << "  LV Size                " << format_size_human(lv.extent_count() * ext_bytes)
                << "\n"
                << "  Current LE             " << lv.extent_count() << "\n"
                << "  Segments               " << lv.segments.size() << "\n";
      if (!lv.segments.empty() && lv.segments[0].stripe_count > 1) {
        std::cout << "  Stripes                " << lv.segments[0].stripe_count << "\n"
                  << "  Stripe size            "
                  << format_size_human(lv.segments[0].stripe_size_sectors * 512ULL) << "\n";
      }
      std::cout << "\n";
    }
  }
  return 0;
}

int cmd_lvscan(const std::vector<std::string>& args) {
  if (check_help_requested(args, "lvscan")) return 0;

  auto pos = get_positional_args(args);
  std::set<std::string> active_uuids;
  std::vector<VolumeGroup> vgs;

#ifdef _WIN32
  active_uuids = query_active_lv_uuids();
  if (pos.empty()) {
    vgs = discover_all_win32_vgs();
  }
#else
  if (pos.empty()) {
    std::cerr << "error: lvscan requires member PV path(s) in offline mode\n";
    return 1;
  }
#endif

  if (vgs.empty() && !pos.empty()) {
    std::string err;
    auto loaded = load_vg_flexible(pos, false, err, false);
    if (!loaded) {
      std::cerr << "error: " << err << "\n";
      return 1;
    }
    vgs.push_back(std::move(loaded->vg));
  }

  for (const auto& vg : vgs) {
    const std::uint64_t ext_bytes = vg.extent_size_sectors * 512ULL;
    for (const auto& lv : vg.lvs) {
      if (lv.internal) continue;
      const bool active = active_uuids.count(normalize_uuid_raw32(lv.id)) > 0;
      std::cout << "  " << (active ? "ACTIVE            " : "inactive          ")
                << "'/dev/" << vg.name << "/" << lv.name << "' ["
                << format_size_human(lv.extent_count() * ext_bytes) << "]\n";
    }
  }
  return 0;
}

int cmd_lvchange(const std::vector<std::string>& args) {
  if (check_help_requested(args, "lvchange")) return 0;

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
    std::cerr << "error: lvchange requires -a/--activate (y|n) or -p/--permission (r|rw)\n";
    return 1;
  }

  auto pos = get_positional_args(args);
  if (pos.empty()) {
    std::cerr << "error: lvchange requires [VGName/]LVName [PVPath...]\n";
    return 1;
  }

  auto [vg_hint, lv_name] = split_vg_lv_spec(pos[0]);
  std::vector<std::string> pv_paths(pos.begin() + 1, pos.end());
#ifdef _WIN32
  if (pv_paths.empty()) {
    pv_paths = discover_win32_pv_paths_for_vg(vg_hint);
  }
#endif

  std::string target_uuid;
  if (!pv_paths.empty()) {
    auto loaded = load_vg_flexible(pv_paths, has_perm, err, has_perm);
    if (!loaded) {
      std::cerr << "error: " << err << "\n";
      return 1;
    }
    if (!vg_hint.empty() && loaded->vg.name != vg_hint) {
      std::cerr << "error: VG name mismatch (expected \"" << vg_hint << "\", found \""
                << loaded->vg.name << "\")\n";
      return 1;
    }

    LogicalVolume* target_lv = nullptr;
    for (auto& lv : loaded->vg.lvs) {
      if (lv.name == lv_name || normalize_uuid_raw32(lv.id) == normalize_uuid_raw32(lv_name)) {
        target_lv = &lv;
        break;
      }
    }
    if (!target_lv) {
      std::cerr << "error: logical volume \"" << lv_name << "\" not found in VG \""
                << loaded->vg.name << "\"\n";
      return 1;
    }
    target_uuid = target_lv->id;

    if (has_perm) {
      if (!check_vg_mutable(loaded->vg, err)) {
        std::cerr << "error: " << err << "\n";
        return 1;
      }
      if (target_lv->status.empty()) {
        target_lv->status = {"READ", "WRITE", "VISIBLE"};
      }
      set_status_flag(target_lv->status, "READ", true);
      set_status_flag(target_lv->status, "WRITE", *perm_opt);
      loaded->vg.seqno++;

      if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
        std::cerr << "error: failed to commit LV permission change: " << err << "\n";
        return 1;
      }
#ifdef _WIN32
      driver_refresh();
#endif
    }
  } else {
    if (has_perm) {
      std::cerr << "error: lvchange -p requires member PV path(s)\n";
      return 1;
    }
    const std::string raw = normalize_uuid_raw32(lv_name);
    if (raw.size() == 32) {
      target_uuid = raw;
    } else {
      std::cerr << "error: cannot resolve LV \"" << pos[0] << "\" without member PV path(s)\n";
      return 1;
    }
  }

  if (has_act) {
#ifdef _WIN32
    if (!driver_activate_lv(target_uuid, *act_opt)) {
      std::cerr << "error: failed to " << (*act_opt ? "activate" : "deactivate")
                << " logical volume \"" << pos[0] << "\" via WinLVM driver\n";
      return 1;
    }
#else
    (void)target_uuid;
    std::cerr << "error: online LV activation/deactivation requires the WinLVM kernel driver on Windows\n";
    return 1;
#endif
  }

  std::cout << "  Logical volume \"" << pos[0] << "\" changed.\n";
  return 0;
}

int cmd_lvrename(const std::vector<std::string>& args) {
  if (check_help_requested(args, "lvrename")) return 0;

  auto pos = get_positional_args(args);
  if (pos.size() < 2) {
    std::cerr << "error: lvrename requires [VGName/]OldLV NewLV [PVPath...]\n";
    print_command_help("lvrename");
    return 1;
  }

  auto [vg_hint, old_name] = split_vg_lv_spec(pos[0]);
  const std::string new_name = strip_lv_path(pos[1]);

  std::string err;
  if (!validate_lv_name(new_name, err)) {
    std::cerr << "error: invalid logical volume name \"" << new_name << "\": " << err << "\n";
    return 1;
  }

  std::vector<std::string> pv_paths(pos.begin() + 2, pos.end());
#ifdef _WIN32
  if (pv_paths.empty()) pv_paths = discover_win32_pv_paths_for_vg(vg_hint);
#endif
  if (pv_paths.empty()) {
    std::cerr << "error: lvrename requires member PV path(s)\n";
    print_command_help("lvrename");
    return 1;
  }

  auto loaded = load_vg_flexible(pv_paths, true, err, true);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  if (!vg_hint.empty() && loaded->vg.name != vg_hint) {
    std::cerr << "error: VG name mismatch (expected \"" << vg_hint << "\", found \""
              << loaded->vg.name << "\")\n";
    return 1;
  }
  if (!check_vg_mutable(loaded->vg, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  for (const auto& lv : loaded->vg.lvs) {
    if (lv.name == new_name) {
      std::cerr << "error: logical volume \"" << new_name << "\" already exists in VG "
                << loaded->vg.name << "\n";
      return 1;
    }
  }

  bool found = false;
  for (auto& lv : loaded->vg.lvs) {
    if (lv.name == old_name) {
      lv.name = new_name;
      found = true;
      break;
    }
  }
  if (!found) {
    std::cerr << "error: logical volume \"" << old_name << "\" not found in VG "
              << loaded->vg.name << "\n";
    return 1;
  }

  loaded->vg.seqno++;
  if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
    std::cerr << "error: failed to commit rename: " << err << "\n";
    return 1;
  }

  std::cout << "  Renamed \"" << old_name << "\" to \"" << new_name << "\" in volume group \""
            << loaded->vg.name << "\"\n";
  return 0;
}

int cmd_lvconvert(const std::vector<std::string>& args) {
  if (check_help_requested(args, "lvconvert")) return 0;
  std::cerr << "error: lvconvert is not supported by this version of WinLVM\n";
  return 1;
}

}  // namespace lvmfmt
