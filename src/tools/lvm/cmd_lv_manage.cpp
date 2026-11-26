#include "subcommands.h"
#include "cmd_common.h"
#include "lv_io.h"
#include "report_json.h"
#include "snap_raid_ops.h"
#include "thin_meta.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lvmfmt {

namespace {

#ifdef _WIN32
std::set<std::string> query_active_lv_uuids() {
  std::set<std::string> active;
  std::vector<WINLVM_INVENTORY_LV_ENTRY> entries;
  std::uint32_t records = 0;
  if (driver_query_inventory(entries, records)) {
    for (const auto& e : entries) {
      if ((e.flags & WINLVM_INV_LV_ACTIVE) != 0) {
        active.insert(
            normalize_uuid_raw32(std::string(e.lv_uuid, strnlen(e.lv_uuid, sizeof(e.lv_uuid)))));
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

bool load_vgs_for_lv_report(const std::vector<std::string>& pos,
                            std::vector<VolumeGroup>& out_vgs,
                            std::string& lv_name_filter,
                            std::string& error) {
  out_vgs.clear();
  lv_name_filter.clear();
#ifdef _WIN32
  if (pos.empty()) {
    out_vgs = discover_all_win32_vgs();
    return true;
  }
#else
  if (pos.empty()) {
    error = "requires member PV path(s)";
    return false;
  }
#endif

  std::string err1;
  auto loaded = load_vg_flexible(pos, false, err1, false);
  if (loaded) {
    // Check whether pos[0] was VG/LV and not an openable file path.
    std::string probe_err;
    if (!open_storage_source(pos[0], false, probe_err)) {
      auto [vg_h, lv_h] = split_vg_lv_spec(pos[0]);
      if (!vg_h.empty() && !lv_h.empty()) lv_name_filter = lv_h;
    }
    out_vgs.push_back(std::move(loaded->vg));
    return true;
  }

  // Fallback: scan all positional paths for multiple VGs.
  std::map<std::string, VolumeGroup> by_uuid;
  for (const auto& dev : pos) {
    std::string local_err;
    auto src = open_storage_source(dev, false, local_err);
    if (!src) continue;
    auto lbl = read_pv_label(*src, local_err);
    if (!lbl) continue;
    auto meta = read_vg_metadata(*src, *lbl, local_err);
    if (!meta) continue;
    auto vg = parse_vg_metadata(*meta, local_err);
    if (!vg) continue;
    const std::string key = normalize_uuid_raw32(vg->id);
    auto it = by_uuid.find(key);
    if (it == by_uuid.end() || vg->seqno > it->second.seqno) {
      by_uuid[key] = std::move(*vg);
    }
  }
  if (by_uuid.empty()) {
    error = err1;
    return false;
  }
  for (auto& kv : by_uuid) out_vgs.push_back(std::move(kv.second));
  return true;
}

bool allocate_linear_segments_on_pvs(const VolumeGroup& vg,
                                     std::uint64_t extents_needed,
                                     const std::vector<std::string>& preferred_pvs,
                                     std::vector<Segment>& out_segments,
                                     std::string& error) {
  out_segments.clear();
  std::vector<ExtentAllocation> allocs;
  if (!allocate_linear(vg, extents_needed, preferred_pvs, allocs, error)) return false;
  std::uint64_t cur_le = 0;
  for (const auto& a : allocs) {
    Segment seg;
    seg.start_extent = cur_le;
    seg.extent_count = a.count;
    seg.type = "linear";
    seg.stripe_count = 1;
    seg.stripes.push_back({a.pv_key, a.start_pe});
    out_segments.push_back(std::move(seg));
    cur_le += a.count;
  }
  return true;
}

std::set<std::string> collect_lv_pv_keys(const LogicalVolume& lv) {
  std::set<std::string> keys;
  for (const auto& seg : lv.segments) {
    for (const auto& st : seg.stripes) keys.insert(st.target);
  }
  return keys;
}

int lvconvert_to_raid1_or_mirror(LoadedVG& loaded,
                                 const std::string& lv_name,
                                 const std::string& target_type) {
  LogicalVolume* target_lv = nullptr;
  for (auto& lv : loaded.vg.lvs) {
    if (lv.name == lv_name) {
      target_lv = &lv;
      break;
    }
  }
  if (!target_lv || target_lv->internal || target_lv->segments.empty()) {
    std::cerr << "error: logical volume \"" << lv_name << "\" not found in VG \""
              << loaded.vg.name << "\"\n";
    return 1;
  }
  for (const auto& seg : target_lv->segments) {
    if ((seg.type != "linear" && seg.type != "striped") || seg.stripe_count > 1) {
      std::cerr << "error: lvconvert --type " << target_type
                << " requires a linear source LV (found \"" << seg.type << "\")\n";
      return 1;
    }
  }

  // Refuse converting an LV that is currently a snapshot origin.
  for (const auto& lv : loaded.vg.lvs) {
    for (const auto& seg : lv.segments) {
      if (seg.type == "snapshot" && seg.origin_lv == lv_name) {
        std::cerr << "error: cannot convert snapshot origin \"" << lv_name << "\"\n";
        return 1;
      }
    }
  }

  const std::uint64_t lv_extents = target_lv->extent_count();
  std::string err;
  std::uint64_t lv_bytes = 0;
  if (!lv_byte_size_checked(loaded.vg, *target_lv, lv_bytes, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  auto lv_sectors_opt = checked_mul(lv_extents, loaded.vg.extent_size_sectors);
  if (!lv_sectors_opt) {
    std::cerr << "error: logical volume sector count overflow\n";
    return 1;
  }
  const std::uint64_t lv_sectors = *lv_sectors_opt;
  const std::uint32_t region_size_sectors = 4096u;  // 2 MiB default

  const auto primary_pvs = collect_lv_pv_keys(*target_lv);
  auto maps = build_vg_extent_maps(loaded.vg);

  // Select a distinct allocatable secondary PV with enough free space for leg 1 (+ rmeta_1 for raid1).
  const std::uint64_t sec_needed = lv_extents + (target_type == "raid1" ? 1ULL : 0ULL);
  std::string secondary_pv;
  for (const auto& m : maps) {
    if (!m.allocatable) continue;
    if (primary_pvs.count(m.pv_key) == 0 && m.free_extents >= sec_needed) {
      secondary_pv = m.pv_key;
      break;
    }
  }
  if (secondary_pv.empty()) {
    std::cerr << "error: lvconvert --type " << target_type
              << " -m 1 requires a second allocatable PV with at least " << sec_needed
              << " free extent(s)\n";
    return 1;
  }

  if (target_type == "raid1") {
    const std::string rimage0_name = lv_name + "_rimage_0";
    const std::string rimage1_name = lv_name + "_rimage_1";
    const std::string rmeta0_name = lv_name + "_rmeta_0";
    const std::string rmeta1_name = lv_name + "_rmeta_1";
    if (loaded.vg.find_lv(rimage0_name) || loaded.vg.find_lv(rimage1_name) ||
        loaded.vg.find_lv(rmeta0_name) || loaded.vg.find_lv(rmeta1_name)) {
      std::cerr << "error: RAID1 sub-volumes for \"" << lv_name << "\" already exist\n";
      return 1;
    }

    // Build leg 0 reusing existing segments of target_lv in place.
    LogicalVolume rimage0;
    rimage0.name = rimage0_name;
    rimage0.id = generate_random_lvm_uuid();
    rimage0.status = {"READ", "WRITE"};
    rimage0.has_status = true;
    rimage0.internal = true;
    rimage0.segments = target_lv->segments;

    // Select PV for rmeta_0 (prefer primary PV, fallback any PV != secondary_pv).
    std::vector<std::string> rmeta0_pvs;
    for (const auto& k : primary_pvs) rmeta0_pvs.push_back(k);
    for (const auto& m : maps) {
      if (m.allocatable && m.pv_key != secondary_pv && primary_pvs.count(m.pv_key) == 0) {
        rmeta0_pvs.push_back(m.pv_key);
      }
    }

    LogicalVolume rmeta0;
    rmeta0.name = rmeta0_name;
    rmeta0.id = generate_random_lvm_uuid();
    rmeta0.status = {"READ", "WRITE"};
    rmeta0.has_status = true;
    rmeta0.internal = true;
    if (!allocate_linear_segments_on_pvs(loaded.vg, 1ULL, rmeta0_pvs, rmeta0.segments, err)) {
      std::cerr << "error: failed to allocate " << rmeta0_name << ": " << err << "\n";
      return 1;
    }

    // Temporarily append rmeta0 so rmeta1/rimage1 allocations see updated free map.
    loaded.vg.lvs.push_back(rmeta0);

    LogicalVolume rmeta1;
    rmeta1.name = rmeta1_name;
    rmeta1.id = generate_random_lvm_uuid();
    rmeta1.status = {"READ", "WRITE"};
    rmeta1.has_status = true;
    rmeta1.internal = true;
    if (!allocate_linear_segments_on_pvs(loaded.vg, 1ULL, {secondary_pv}, rmeta1.segments, err)) {
      std::cerr << "error: failed to allocate " << rmeta1_name << ": " << err << "\n";
      return 1;
    }
    loaded.vg.lvs.push_back(rmeta1);

    LogicalVolume rimage1;
    rimage1.name = rimage1_name;
    rimage1.id = generate_random_lvm_uuid();
    rimage1.status = {"READ", "WRITE"};
    rimage1.has_status = true;
    rimage1.internal = true;
    if (!allocate_linear_segments_on_pvs(loaded.vg, lv_extents, {secondary_pv}, rimage1.segments, err)) {
      std::cerr << "error: failed to allocate " << rimage1_name << ": " << err << "\n";
      return 1;
    }

    // Copy existing data byte-for-byte from leg 0 to leg 1.
    if (!copy_lv_bytes(loaded, rimage0, rimage1, lv_bytes, err)) {
      std::cerr << "error: failed to synchronize RAID1 leg 1: " << err << "\n";
      return 1;
    }

    // Write valid dm-raid DmRd superblocks + clean bitmaps on both rmeta sub-volumes.
    if (!write_dm_raid1_superblock(loaded, rmeta0, 0u, 2u, lv_sectors, region_size_sectors, err) ||
        !write_dm_raid1_superblock(loaded, rmeta1, 1u, 2u, lv_sectors, region_size_sectors, err)) {
      std::cerr << "error: failed to write dm-raid superblocks: " << err << "\n";
      return 1;
    }

    // Re-locate target_lv after push_backs and replace its segment with raid1.
    for (auto& lv : loaded.vg.lvs) {
      if (lv.name == lv_name) {
        Segment rseg;
        rseg.start_extent = 0;
        rseg.extent_count = lv_extents;
        rseg.type = "raid1";
        rseg.device_count = 2;
        rseg.region_size_sectors = region_size_sectors;
        rseg.raids = {rmeta0_name, rimage0_name, rmeta1_name, rimage1_name};
        lv.segments = {std::move(rseg)};
        break;
      }
    }
    loaded.vg.lvs.push_back(std::move(rimage0));
    loaded.vg.lvs.push_back(std::move(rimage1));
  } else {
    // mirror with --mirrorlog core
    const std::string mimage0_name = lv_name + "_mimage_0";
    const std::string mimage1_name = lv_name + "_mimage_1";
    if (loaded.vg.find_lv(mimage0_name) || loaded.vg.find_lv(mimage1_name)) {
      std::cerr << "error: mirror sub-volumes for \"" << lv_name << "\" already exist\n";
      return 1;
    }

    LogicalVolume mimage0;
    mimage0.name = mimage0_name;
    mimage0.id = generate_random_lvm_uuid();
    mimage0.status = {"READ", "WRITE"};
    mimage0.has_status = true;
    mimage0.internal = true;
    mimage0.segments = target_lv->segments;

    LogicalVolume mimage1;
    mimage1.name = mimage1_name;
    mimage1.id = generate_random_lvm_uuid();
    mimage1.status = {"READ", "WRITE"};
    mimage1.has_status = true;
    mimage1.internal = true;
    if (!allocate_linear_segments_on_pvs(loaded.vg, lv_extents, {secondary_pv}, mimage1.segments, err)) {
      std::cerr << "error: failed to allocate " << mimage1_name << ": " << err << "\n";
      return 1;
    }

    if (!copy_lv_bytes(loaded, mimage0, mimage1, lv_bytes, err)) {
      std::cerr << "error: failed to synchronize mirror leg 1: " << err << "\n";
      return 1;
    }

    Segment mseg;
    mseg.start_extent = 0;
    mseg.extent_count = lv_extents;
    mseg.type = "mirror";
    mseg.mirror_count = 2;
    mseg.region_size_sectors = region_size_sectors;
    mseg.mirrors = {{mimage0_name, 0ULL}, {mimage1_name, 0ULL}};
    target_lv->segments = {std::move(mseg)};

    loaded.vg.lvs.push_back(std::move(mimage0));
    loaded.vg.lvs.push_back(std::move(mimage1));
  }

  loaded.vg.seqno++;
  if (!commit_vg_metadata(loaded.vg, loaded.raw_storages(), err)) {
    std::cerr << "error: failed to commit lvconvert metadata: " << err << "\n";
    return 1;
  }

#ifdef _WIN32
  driver_refresh();
#endif
  std::cout << "  Logical volume " << loaded.vg.name << "/" << lv_name
            << " successfully converted.\n";
  return 0;
}

int lvconvert_to_linear(LoadedVG& loaded, const std::string& lv_name) {
  LogicalVolume* target_lv = nullptr;
  for (auto& lv : loaded.vg.lvs) {
    if (lv.name == lv_name) {
      target_lv = &lv;
      break;
    }
  }
  if (!target_lv || target_lv->segments.empty()) {
    std::cerr << "error: logical volume \"" << lv_name << "\" not found in VG \""
              << loaded.vg.name << "\"\n";
    return 1;
  }

  const Segment seg = target_lv->segments[0];
  std::set<std::string> sub_lvs_to_remove;
  std::string primary_img_name;
  std::string err;

  if (seg.type == "raid1") {
    if (seg.device_count < 2 || seg.raids.size() != seg.device_count * 2) {
      std::cerr << "error: malformed raid1 segment metadata on \"" << lv_name << "\"\n";
      return 1;
    }
    auto expected_sectors = checked_mul(target_lv->extent_count(), loaded.vg.extent_size_sectors);
    if (!expected_sectors) {
      std::cerr << "error: raid1 sector count overflow\n";
      return 1;
    }
    // Validate DmRd superblocks on EVERY leg before discarding any mirror leg.
    std::vector<DmRaid1SuperblockInfo> sb_infos;
    sb_infos.reserve(static_cast<std::size_t>(seg.device_count));
    for (std::size_t leg = 0; leg < static_cast<std::size_t>(seg.device_count); ++leg) {
      const std::string& rmeta_name = seg.raids[leg * 2];
      const std::string& rimage_name = seg.raids[leg * 2 + 1];
      const LogicalVolume* rmeta_lv = loaded.vg.find_lv(rmeta_name);
      const LogicalVolume* rimage_lv = loaded.vg.find_lv(rimage_name);
      if (!rmeta_lv || !rimage_lv) {
        std::cerr << "error: RAID1 leg " << leg << " sub-volume missing for \"" << lv_name
                  << "\"; refusing unsafe collapse to linear\n";
        return 1;
      }
      DmRaid1SuperblockInfo info{};
      if (!validate_dm_raid1_superblock(loaded, *rmeta_lv,
                                        static_cast<std::uint32_t>(leg),
                                        static_cast<std::uint32_t>(seg.device_count),
                                        *expected_sectors, info, err)) {
        std::cerr << "error: " << err << "\n";
        return 1;
      }
      sb_infos.push_back(info);
      sub_lvs_to_remove.insert(rmeta_name);
      sub_lvs_to_remove.insert(rimage_name);
    }
    for (std::size_t leg = 1; leg < sb_infos.size(); ++leg) {
      if (sb_infos[leg].events != sb_infos[0].events) {
        std::cerr << "error: RAID1 superblock event count mismatch between legs ("
                  << sb_infos[0].events << " vs " << sb_infos[leg].events
                  << "); refusing unsafe collapse to linear\n";
        return 1;
      }
    }
    primary_img_name = seg.raids[1];
  } else if (seg.type == "mirror") {
    for (const auto& m : seg.mirrors) sub_lvs_to_remove.insert(m.target);
    if (!seg.mirror_log_lv.empty()) sub_lvs_to_remove.insert(seg.mirror_log_lv);
    for (const auto& m : seg.mirrors) {
      if (loaded.vg.find_lv(m.target)) {
        primary_img_name = m.target;
        break;
      }
    }
  } else {
    std::cerr << "error: lvconvert -m 0/--type linear requires a raid1 or mirror LV (found \""
              << seg.type << "\")\n";
    return 1;
  }

  const LogicalVolume* primary_img =
      primary_img_name.empty() ? nullptr : loaded.vg.find_lv(primary_img_name);
  if (!primary_img || primary_img->segments.empty()) {
    std::cerr << "error: primary leg sub-volume for \"" << lv_name << "\" not found\n";
    return 1;
  }

  target_lv->segments = primary_img->segments;
  target_lv->supported = true;
  target_lv->unsupported_reason.clear();

  std::vector<LogicalVolume> kept;
  kept.reserve(loaded.vg.lvs.size());
  for (auto& lv : loaded.vg.lvs) {
    if (sub_lvs_to_remove.count(lv.name) == 0) {
      kept.push_back(std::move(lv));
    }
  }
  loaded.vg.lvs = std::move(kept);
  loaded.vg.seqno++;

  if (!commit_vg_metadata(loaded.vg, loaded.raw_storages(), err)) {
    std::cerr << "error: failed to commit linear conversion metadata: " << err << "\n";
    return 1;
  }

#ifdef _WIN32
  driver_refresh();
#endif
  std::cout << "  Logical volume " << loaded.vg.name << "/" << lv_name
            << " successfully converted.\n";
  return 0;
}

int lvconvert_merge_snapshot(LoadedVG& loaded, const std::string& snap_spec_name) {
  std::string wrapper_name;
  std::string origin_name;
  std::string cow_name;

  for (const auto& lv : loaded.vg.lvs) {
    for (const auto& seg : lv.segments) {
      if (seg.type == "snapshot" &&
          (seg.cow_store_lv == snap_spec_name || lv.name == snap_spec_name)) {
        wrapper_name = lv.name;
        origin_name = seg.origin_lv;
        cow_name = seg.cow_store_lv;
        break;
      }
    }
    if (!wrapper_name.empty()) break;
  }

  if (wrapper_name.empty()) {
    std::cerr << "error: snapshot \"" << snap_spec_name << "\" not found in VG \""
              << loaded.vg.name << "\"\n";
    return 1;
  }

  // Offline merge writes directly onto origin_lv without COW-ing displaced chunks
  // into sibling snapshots, so fail closed if > 1 snapshot shares origin_name.
  std::size_t origin_snap_count = 0;
  for (const auto& lv : loaded.vg.lvs) {
    for (const auto& seg : lv.segments) {
      if (seg.type == "snapshot" && seg.origin_lv == origin_name) {
        origin_snap_count++;
      }
    }
  }
  if (origin_snap_count > 1) {
    std::cerr << "error: cannot merge snapshot \"" << cow_name
              << "\" offline while origin \"" << origin_name << "\" has "
              << origin_snap_count << " snapshots; remove sibling snapshots first\n";
    return 1;
  }

  const LogicalVolume* origin_lv = loaded.vg.find_lv(origin_name);
  const LogicalVolume* cow_lv = loaded.vg.find_lv(cow_name);
  if (!origin_lv || !cow_lv) {
    std::cerr << "error: snapshot \"" << snap_spec_name << "\" references missing origin or COW LV\n";
    return 1;
  }

  std::string err;
  std::uint64_t merged_chunks = 0;
  if (!merge_snapshot_cow_to_origin(loaded, *origin_lv, *cow_lv, merged_chunks, err)) {
    std::cerr << "error: snapshot merge failed: " << err << "\n";
    return 1;
  }

  // Remove snapshot wrapper and COW store LVs from VG metadata.
  std::vector<LogicalVolume> kept;
  kept.reserve(loaded.vg.lvs.size());
  for (auto& lv : loaded.vg.lvs) {
    if (lv.name == wrapper_name || lv.name == cow_name) continue;
    kept.push_back(std::move(lv));
  }
  loaded.vg.lvs = std::move(kept);
  loaded.vg.seqno++;

  if (!commit_vg_metadata(loaded.vg, loaded.raw_storages(), err)) {
    std::cerr << "error: failed to commit metadata after snapshot merge: " << err << "\n";
    return 1;
  }

#ifdef _WIN32
  driver_refresh();
#endif
  std::cout << "  Merging of volume " << loaded.vg.name << "/" << cow_name
            << " completed (" << merged_chunks << " exception chunk(s) applied).\n";
  return 0;
}

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

  std::set<std::string> to_remove;
  std::string removed_uuid;
  const LogicalVolume* target = loaded->vg.find_lv(lv_name);
  if (!target) {
    std::cerr << "error: logical volume \"" << lv_name << "\" not found in VG "
              << loaded->vg.name << "\n";
    return 1;
  }

  // Refuse removing a thin-pool if thin LVs still reference it, or an origin with snapshots.
  for (const auto& lv : loaded->vg.lvs) {
    for (const auto& seg : lv.segments) {
      if (seg.type == "thin" && seg.pool_lv == lv_name) {
        std::cerr << "error: cannot remove thin-pool \"" << lv_name
                  << "\" while thin LV \"" << lv.name << "\" still exists\n";
        return 1;
      }
      if (seg.type == "snapshot" && seg.origin_lv == lv_name) {
        std::cerr << "error: cannot remove snapshot origin \"" << lv_name
                  << "\" while snapshot \"" << seg.cow_store_lv << "\" exists\n";
        return 1;
      }
      if (seg.type == "snapshot" && seg.cow_store_lv == lv_name) {
        to_remove.insert(lv.name);  // also remove snapshot0 wrapper
      }
    }
  }

  to_remove.insert(target->name);
  removed_uuid = target->id;

  // If removing a thin LV, delete its device_id from <pool>_tmeta (freeing mapped
  // data blocks and mapping tree nodes) and synchronize pool transaction_id.
  if (!target->segments.empty() && target->segments[0].type == "thin") {
    const Segment& tseg = target->segments[0];
    LogicalVolume* pool_lv = nullptr;
    for (auto& lv : loaded->vg.lvs) {
      if (lv.name == tseg.pool_lv) {
        pool_lv = &lv;
        break;
      }
    }
    if (!pool_lv || pool_lv->segments.empty() || pool_lv->segments[0].type != "thin-pool") {
      std::cerr << "error: thin-pool \"" << tseg.pool_lv << "\" for thin LV \"" << lv_name
                << "\" not found\n";
      return 1;
    }
    const LogicalVolume* tmeta_lv = loaded->vg.find_lv(pool_lv->segments[0].metadata_lv);
    if (!tmeta_lv) {
      std::cerr << "error: thin-pool metadata sub-volume \"" << pool_lv->segments[0].metadata_lv
                << "\" not found\n";
      return 1;
    }
    std::uint64_t new_pool_trans = 0;
    if (!thin_pool_delete_device(*loaded, *tmeta_lv, tseg.device_id,
                                 pool_lv->segments[0].transaction_id, new_pool_trans, err)) {
      std::cerr << "error: failed to remove thin LV \"" << lv_name
                << "\" from thin-pool metadata: " << err << "\n";
      return 1;
    }
    pool_lv->segments[0].transaction_id = new_pool_trans;
    // Detach surviving thin snapshots that referenced this thin LV as their origin.
    for (auto& lv : loaded->vg.lvs) {
      if (lv.name == lv_name) continue;
      for (auto& seg : lv.segments) {
        if (seg.type == "thin" && seg.origin_lv == lv_name) {
          seg.origin_lv.clear();
        }
      }
    }
  }

  for (const auto& seg : target->segments) {
    if (seg.type == "thin-pool") {
      to_remove.insert(seg.metadata_lv);
      to_remove.insert(seg.pool_lv);
    } else if (seg.type == "snapshot") {
      to_remove.insert(seg.cow_store_lv);
    } else if (seg.type == "raid1") {
      for (const auto& r : seg.raids) to_remove.insert(r);
    } else if (seg.type == "mirror") {
      for (const auto& m : seg.mirrors) to_remove.insert(m.target);
      if (!seg.mirror_log_lv.empty()) to_remove.insert(seg.mirror_log_lv);
    }
  }

#ifdef _WIN32
  driver_activate_lv(removed_uuid, false);
#else
  (void)removed_uuid;
#endif

  std::vector<LogicalVolume> remaining_lvs;
  for (auto& lv : loaded->vg.lvs) {
    if (to_remove.count(lv.name) == 0) {
      remaining_lvs.push_back(std::move(lv));
    }
  }
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

  ReportFormat fmt = ReportFormat::Default;
  std::string err;
  if (!parse_report_format(args, fmt, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  const std::set<std::string> val_flags = {"-o", "--options", "--separator", "--reportformat"};
  auto raw_pos = get_positional_args_for(args, val_flags);
  std::vector<std::string> pos;
  std::set<std::string> tag_filters;
  if (!split_tag_filters(raw_pos, pos, tag_filters, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  const bool noheadings = has_flag(args, "--noheadings");
  const auto opt_cols = get_opt_value(args, "-o", "--options");
  const auto sep_opt = get_opt_value(args, "", "--separator");
  const std::string sep = sep_opt ? *sep_opt : " ";
  const bool show_tags = options_requests_column(opt_cols, "tags", "lv_tags");
  const bool show_segtype = options_requests_column(opt_cols, "segtype");
  const bool show_pool = options_requests_column(opt_cols, "pool_lv");
  const bool show_origin = options_requests_column(opt_cols, "origin");

  std::set<std::string> active_uuids;
#ifdef _WIN32
  active_uuids = query_active_lv_uuids();
#endif

  std::vector<VolumeGroup> vgs;
  std::string lv_name_filter;
  if (!load_vgs_for_lv_report(pos, vgs, lv_name_filter, err)) {
    std::cerr << "error: lvs " << err << "\n";
    return 1;
  }

  std::vector<LvReportRow> rows;
  for (const auto& vg : vgs) {
    for (auto& r : build_lv_report_rows(vg, active_uuids)) {
      if (!lv_name_filter.empty() && r.lv_name != lv_name_filter) continue;
      if (!matches_tag_filters(r.lv_tags, tag_filters)) continue;
      rows.push_back(std::move(r));
    }
  }

  if (fmt == ReportFormat::Json || fmt == ReportFormat::JsonStd) {
    print_lvs_json(rows, fmt);
    return 0;
  }

  if (!noheadings) {
    std::cout << "  LV   VG   Attr       LSize";
    if (show_pool) std::cout << "  Pool";
    if (show_origin) std::cout << "  Origin";
    if (show_segtype) std::cout << "  Type";
    if (show_tags) std::cout << "  LV Tags";
    std::cout << "\n";
  }
  for (const auto& r : rows) {
    std::cout << "  " << r.lv_name << sep << r.vg_name << sep << r.lv_attr << sep << r.lv_size;
    if (show_pool) std::cout << sep << r.pool_lv;
    if (show_origin) std::cout << sep << r.origin;
    if (show_segtype) std::cout << sep << r.segtype;
    if (show_tags) std::cout << sep << join_tags_csv(r.lv_tags);
    std::cout << "\n";
  }
  return 0;
}

int cmd_lvdisplay(const std::vector<std::string>& args) {
  if (check_help_requested(args, "lvdisplay")) return 0;

  ReportFormat fmt = ReportFormat::Default;
  std::string err;
  if (!parse_report_format(args, fmt, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  const std::set<std::string> val_flags = {"--reportformat", "-o", "--options", "--separator"};
  auto raw_pos = get_positional_args_for(args, val_flags);
  std::vector<std::string> pos;
  std::set<std::string> tag_filters;
  if (!split_tag_filters(raw_pos, pos, tag_filters, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  std::set<std::string> active_uuids;
#ifdef _WIN32
  active_uuids = query_active_lv_uuids();
#endif

  std::vector<VolumeGroup> vgs;
  std::string lv_name_filter;
  if (!load_vgs_for_lv_report(pos, vgs, lv_name_filter, err)) {
    std::cerr << "error: lvdisplay " << err << "\n";
    return 1;
  }

  std::vector<LvReportRow> rows;
  for (const auto& vg : vgs) {
    for (auto& r : build_lv_report_rows(vg, active_uuids)) {
      if (!lv_name_filter.empty() && r.lv_name != lv_name_filter) continue;
      if (!matches_tag_filters(r.lv_tags, tag_filters)) continue;
      rows.push_back(std::move(r));
    }
  }

  if (fmt == ReportFormat::Json || fmt == ReportFormat::JsonStd) {
    print_lvs_json(rows, fmt);
    return 0;
  }

  for (const auto& r : rows) {
    std::cout << "  --- Logical volume ---\n"
              << "  LV Path                /dev/" << r.vg_name << "/" << r.lv_name << "\n"
              << "  LV Name                " << r.lv_name << "\n"
              << "  VG Name                " << r.vg_name << "\n"
              << "  LV UUID                " << r.lv_uuid << "\n"
              << "  LV Write Access        " << (r.read_only ? "read only" : "read/write") << "\n"
              << "  LV Status              " << (r.active ? "available" : "NOT available") << "\n"
              << "  LV Size                " << r.lv_size << "\n"
              << "  Current LE             " << r.extents << "\n"
              << "  Segments               " << r.segment_count << "\n";
    if (!r.pool_lv.empty()) {
      std::cout << "  LV Pool name           " << r.pool_lv << "\n";
    }
    if (!r.origin.empty()) {
      std::cout << "  LV snapshot origin     " << r.origin << "\n";
    }
    if (r.stripe_count > 1) {
      std::cout << "  Stripes                " << r.stripe_count << "\n"
                << "  Stripe size            "
                << format_size_human(r.stripe_size_sectors * 512ULL) << "\n";
    }
    if (!r.lv_tags.empty()) {
      std::cout << "  LV Tags                " << join_tags_csv(r.lv_tags) << "\n";
    }
    std::cout << "\n";
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
    for (const auto& r : build_lv_report_rows(vg, active_uuids)) {
      std::cout << "  " << (r.active ? "ACTIVE            " : "inactive          ")
                << "'/dev/" << r.vg_name << "/" << r.lv_name << "' ["
                << r.lv_size << "]\n";
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
  std::vector<std::string> add_tags;
  std::vector<std::string> del_tags;
  if (!parse_tag_mutations(args, add_tags, del_tags, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  const bool has_tags = !add_tags.empty() || !del_tags.empty();

  if (!has_act && !has_perm && !has_tags) {
    std::cerr << "error: lvchange requires -a/--activate, -p/--permission, --addtag, or --deltag\n";
    return 1;
  }

  const std::set<std::string> val_flags = {
      "-a", "--activate", "-p", "--permission", "--addtag", "--deltag",
  };
  auto pos = get_positional_args_for(args, val_flags);
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

  const bool mutates_meta = has_perm || has_tags;
  std::string target_uuid;
  if (!pv_paths.empty()) {
    auto loaded = load_vg_flexible(pv_paths, mutates_meta, err, mutates_meta);
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

    if (mutates_meta) {
      if (!check_vg_mutable(loaded->vg, err)) {
        std::cerr << "error: " << err << "\n";
        return 1;
      }
      if (has_perm) {
        if (target_lv->status.empty()) {
          target_lv->status = {"READ", "WRITE", "VISIBLE"};
        }
        target_lv->has_status = true;
        set_status_flag(target_lv->status, "READ", true);
        set_status_flag(target_lv->status, "WRITE", *perm_opt);
      }
      if (has_tags) {
        apply_tag_mutations(target_lv->tags, add_tags, del_tags);
      }
      loaded->vg.seqno++;

      if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
        std::cerr << "error: failed to commit lvchange metadata: " << err << "\n";
        return 1;
      }
#ifdef _WIN32
      driver_refresh();
#endif
    }
  } else {
    if (mutates_meta) {
      std::cerr << "error: lvchange metadata modifications require member PV path(s)\n";
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

  LogicalVolume* target_lv = nullptr;
  for (auto& lv : loaded->vg.lvs) {
    if (lv.name == old_name) {
      target_lv = &lv;
      break;
    }
  }
  if (!target_lv) {
    std::cerr << "error: logical volume \"" << old_name << "\" not found in VG "
              << loaded->vg.name << "\n";
    return 1;
  }

  // Allow renaming a snapshot COW LV (whose internal flag is set because snapshot0 references it),
  // but refuse renaming internal sub-volumes (_tmeta, _tdata, _rmeta_*, _rimage_*, _mimage_*, _pmspare).
  bool is_snap_cow = false;
  for (const auto& lv : loaded->vg.lvs) {
    for (const auto& seg : lv.segments) {
      if (seg.type == "snapshot" && seg.cow_store_lv == old_name) is_snap_cow = true;
    }
  }
  if (target_lv->internal && !is_snap_cow) {
    std::cerr << "error: cannot rename internal sub-volume \"" << old_name << "\" directly\n";
    return 1;
  }
  if (!target_lv->segments.empty() && target_lv->segments[0].type == "snapshot") {
    std::cerr << "error: rename snapshot by its visible COW volume name, not \"" << old_name
              << "\"\n";
    return 1;
  }

  // Collect atomic rename pairs (old sub-LV name -> new sub-LV name) for composite LVs.
  std::map<std::string, std::string> rename_map;
  rename_map[old_name] = new_name;
  auto remap_prefix = [&](const std::string& sub_name) {
    const std::string pfx = old_name + "_";
    if (sub_name.rfind(pfx, 0) == 0) {
      rename_map[sub_name] = new_name + sub_name.substr(old_name.size());
    }
  };
  for (const auto& seg : target_lv->segments) {
    if (seg.type == "thin-pool") {
      remap_prefix(seg.metadata_lv);
      remap_prefix(seg.pool_lv);
    } else if (seg.type == "raid1") {
      for (const auto& r : seg.raids) remap_prefix(r);
    } else if (seg.type == "mirror") {
      for (const auto& m : seg.mirrors) remap_prefix(m.target);
      if (!seg.mirror_log_lv.empty()) remap_prefix(seg.mirror_log_lv);
    }
  }

  // Validate no target name collides with an existing LV outside the rename set.
  for (const auto& kv : rename_map) {
    if (!validate_lv_name(kv.second, err)) {
      std::cerr << "error: invalid renamed sub-volume \"" << kv.second << "\": " << err << "\n";
      return 1;
    }
    if (loaded->vg.find_lv(kv.second) && rename_map.count(kv.second) == 0) {
      std::cerr << "error: logical volume \"" << kv.second << "\" already exists in VG "
                << loaded->vg.name << "\n";
      return 1;
    }
  }

  // Apply renames atomically across LV names and all referencing segment fields.
  auto rewrite_name = [&](std::string& s) {
    auto it = rename_map.find(s);
    if (it != rename_map.end()) s = it->second;
  };
  for (auto& lv : loaded->vg.lvs) {
    rewrite_name(lv.name);
    for (auto& seg : lv.segments) {
      rewrite_name(seg.metadata_lv);
      rewrite_name(seg.pool_lv);
      rewrite_name(seg.origin_lv);
      rewrite_name(seg.cow_store_lv);
      rewrite_name(seg.mirror_log_lv);
      for (auto& r : seg.raids) rewrite_name(r);
      for (auto& m : seg.mirrors) rewrite_name(m.target);
    }
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

  const auto type_opt = get_opt_value(args, "", "--type");
  const auto mirrors_opt = get_opt_value(args, "-m", "--mirrors");
  const auto mlog_opt = get_opt_value(args, "", "--mirrorlog");
  const bool merge_flag = has_flag(args, "", "--merge");

  const std::set<std::string> val_flags = {"--type", "-m", "--mirrors", "--mirrorlog"};
  const auto pos = get_positional_args_for(args, val_flags);

  if (mlog_opt && *mlog_opt != "core") {
    std::cerr << "error: unsupported --mirrorlog \"" << *mlog_opt
              << "\" (only \"core\" is supported)\n";
    return 1;
  }

  if (type_opt && *type_opt != "raid1" && *type_opt != "mirror" && *type_opt != "linear") {
    std::cerr << "error: unsupported lvconvert --type \"" << *type_opt
              << "\" (supported: raid1, mirror, linear)\n";
    return 1;
  }

  std::optional<int> mirror_count;
  if (mirrors_opt) {
    try {
      std::size_t idx = 0;
      const int v = std::stoi(*mirrors_opt, &idx);
      if (idx != mirrors_opt->size() || (v != 0 && v != 1)) {
        std::cerr << "error: unsupported mirror count -m " << *mirrors_opt
                  << " (supported: 0 or 1)\n";
        return 1;
      }
      mirror_count = v;
    } catch (...) {
      std::cerr << "error: invalid mirror count: " << *mirrors_opt << "\n";
      return 1;
    }
  }

  if (!merge_flag && !type_opt && !mirror_count) {
    std::cerr << "error: lvconvert requires --type {raid1|mirror|linear}, -m {0|1}, or --merge\n";
    print_command_help("lvconvert");
    return 1;
  }

  if (pos.empty()) {
    std::cerr << "error: lvconvert requires [VGName/]LVName [PVPath...]\n";
    print_command_help("lvconvert");
    return 1;
  }

  auto [vg_hint, lv_name] = split_vg_lv_spec(pos[0]);
  std::vector<std::string> pv_paths(pos.begin() + 1, pos.end());
#ifdef _WIN32
  if (pv_paths.empty()) pv_paths = discover_win32_pv_paths_for_vg(vg_hint);
#endif
  if (pv_paths.empty()) {
    std::cerr << "error: lvconvert requires member PV path(s)\n";
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

  if (merge_flag) {
    return lvconvert_merge_snapshot(*loaded, lv_name);
  }
  if ((type_opt && *type_opt == "linear") || (mirror_count && *mirror_count == 0)) {
    if (mirror_count && *mirror_count != 0) {
      std::cerr << "error: --type linear cannot be combined with -m " << *mirror_count << "\n";
      return 1;
    }
    return lvconvert_to_linear(*loaded, lv_name);
  }

  const std::string eff_type = type_opt ? *type_opt : "raid1";
  if (mirror_count && *mirror_count != 1) {
    std::cerr << "error: --type " << eff_type << " requires -m 1\n";
    return 1;
  }
  return lvconvert_to_raid1_or_mirror(*loaded, lv_name, eff_type);
}

}  // namespace lvmfmt
