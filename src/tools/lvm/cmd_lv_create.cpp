#include "subcommands.h"
#include "cmd_common.h"
#include "lv_extent_alloc.h"
#include "snap_raid_ops.h"
#include "thin_meta.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace lvmfmt {

namespace {

bool ceil_div_u64(std::uint64_t num, std::uint64_t denom, std::uint64_t& out) {
  if (denom == 0) return false;
  out = (num / denom) + ((num % denom) ? 1ULL : 0ULL);
  return true;
}

bool resolve_extents_arg(const VolumeGroup& vg,
                         const std::vector<std::string>& args,
                         std::uint32_t stripes,
                         std::uint64_t& extents_out,
                         std::string& error) {
  auto ext_bytes_opt = checked_mul(vg.extent_size_sectors, 512ULL);
  if (!ext_bytes_opt || *ext_bytes_opt == 0) {
    error = "volume group extent size is zero or overflows";
    return false;
  }
  const std::uint64_t extent_bytes = *ext_bytes_opt;
  if (auto sz_str = get_opt_value(args, "-L", "--size")) {
    auto spec = parse_size_spec(*sz_str, error);
    if (!spec || spec->sign == SizeSign::Minus || spec->value == 0) {
      if (error.empty()) error = "invalid size: " + *sz_str;
      return false;
    }
    ceil_div_u64(spec->value, extent_bytes, extents_out);
    return extents_out > 0;
  }
  if (auto l_str = get_opt_value(args, "-l", "--extents")) {
    auto spec = parse_extents_spec(*l_str, error);
    if (!spec || spec->sign == SizeSign::Minus || spec->value == 0) {
      if (error.empty()) error = "invalid extent specification: " + *l_str;
      return false;
    }
    if (spec->is_percent) {
      auto maps = build_vg_extent_maps(vg);
      std::uint64_t free_extents = 0;
      std::uint64_t total_extents = 0;
      for (const auto& m : maps) {
        if (m.allocatable) {
          auto nf = checked_add(free_extents, m.free_extents);
          if (!nf) { error = "free extent count overflow"; return false; }
          free_extents = *nf;
        }
        auto nt = checked_add(total_extents, m.total_extents);
        if (!nt) { error = "total extent count overflow"; return false; }
        total_extents = *nt;
      }
      std::uint64_t base = 0;
      if (spec->percent_origin == "FREE") base = free_extents;
      else if (spec->percent_origin == "VG" || spec->percent_origin == "PVS") base = total_extents;
      else {
        error = "percentage origin %" + spec->percent_origin + " is not valid for lvcreate";
        return false;
      }
      auto prod = checked_mul(base, spec->value);
      if (!prod) {
        error = "extent percentage calculation overflow";
        return false;
      }
      extents_out = *prod / 100ULL;
      if (stripes > 1) extents_out = (extents_out / stripes) * stripes;
    } else {
      extents_out = spec->value;
    }
    if (extents_out == 0) {
      error = "requested extent count must be > 0";
      return false;
    }
    return true;
  }
  error = "lvcreate requires either -L <size> or -l <extents>";
  return false;
}

std::string next_snapshot_wrapper_name(const VolumeGroup& vg) {
  for (std::size_t idx = 0; ; ++idx) {
    std::string candidate = "snapshot" + std::to_string(idx);
    if (!vg.find_lv(candidate)) return candidate;
  }
}

int create_thin_pool_lv(const std::vector<std::string>& args,
                        const std::vector<std::string>& pos,
                        const std::optional<std::string>& name_opt,
                        const std::vector<std::string>& add_tags,
                        const std::vector<std::string>& del_tags) {
  std::string pool_name;
  std::vector<std::string> vg_pos = pos;
  if (name_opt) {
    pool_name = *name_opt;
  } else if (!pos.empty()) {
    auto [vg_part, lv_part] = split_vg_lv_spec(pos[0]);
    if (!vg_part.empty() && !lv_part.empty()) {
      pool_name = lv_part;
      vg_pos[0] = vg_part;
    }
  }
  if (pool_name.empty()) {
    std::cerr << "error: thin-pool creation requires pool name via -n <name> or <vg>/<pool>\n";
    return 1;
  }

  std::string err;
  if (!validate_lv_name(pool_name, err)) {
    std::cerr << "error: invalid thin-pool name \"" << pool_name << "\": " << err << "\n";
    return 1;
  }
  if (vg_pos.empty()) {
    std::cerr << "error: lvcreate --type thin-pool requires VG name and/or member PV path(s)\n";
    return 1;
  }

  auto loaded = load_vg_flexible(vg_pos, true, err, true);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  if (!check_vg_mutable(loaded->vg, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  const std::string tmeta_name = pool_name + "_tmeta";
  const std::string tdata_name = pool_name + "_tdata";
  if (loaded->vg.find_lv(pool_name) || loaded->vg.find_lv(tmeta_name) ||
      loaded->vg.find_lv(tdata_name)) {
    std::cerr << "error: logical volume \"" << pool_name << "\" (or its sub-volumes) already exists in VG "
              << loaded->vg.name << "\n";
    return 1;
  }

  std::uint64_t data_extents = 0;
  if (!resolve_extents_arg(loaded->vg, args, 1, data_extents, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  auto ext_bytes_opt = checked_mul(loaded->vg.extent_size_sectors, 512ULL);
  if (!ext_bytes_opt || *ext_bytes_opt == 0) {
    std::cerr << "error: volume group extent size overflow or zero\n";
    return 1;
  }
  const std::uint64_t extent_bytes = *ext_bytes_opt;
  std::uint64_t meta_extents = 1ULL;
  if (auto msz_str = get_opt_value(args, "", "--poolmetadatasize")) {
    auto msz = parse_size_bytes(*msz_str, err);
    if (!msz || *msz == 0) {
      std::cerr << "error: invalid --poolmetadatasize: " << err << "\n";
      return 1;
    }
    std::uint64_t ceiled = 0;
    ceil_div_u64(*msz, extent_bytes, ceiled);
    meta_extents = std::max<std::uint64_t>(1ULL, ceiled);
  }

  std::uint32_t chunk_sectors = 128u;  // 64 KiB default
  if (auto csz_str = get_opt_value(args, "-c", "--chunksize")) {
    auto csz = parse_size_bytes(*csz_str, err);
    if (!csz || *csz < 65536ULL || *csz > 1073741824ULL || (*csz % 512ULL) != 0) {
      std::cerr << "error: invalid thin-pool --chunksize (must be 64 KiB..1 GiB, power of 2)\n";
      return 1;
    }
    const std::uint64_t csec = *csz / 512ULL;
    if ((csec & (csec - 1ULL)) != 0) {
      std::cerr << "error: thin-pool --chunksize must be a power of 2\n";
      return 1;
    }
    chunk_sectors = static_cast<std::uint32_t>(csec);
  }

  // Allocate <pool>_tdata first, then <pool>_tmeta on disjoint extents.
  LogicalVolume tdata_lv;
  tdata_lv.name = tdata_name;
  tdata_lv.id = generate_random_lvm_uuid();
  tdata_lv.status = {"READ", "WRITE"};
  tdata_lv.has_status = true;
  tdata_lv.internal = true;
  if (!build_linear_segments(loaded->vg, data_extents, tdata_lv.segments, err)) {
    std::cerr << "error: failed to allocate thin-pool data extents: " << err << "\n";
    return 1;
  }
  loaded->vg.lvs.push_back(tdata_lv);

  LogicalVolume tmeta_lv;
  tmeta_lv.name = tmeta_name;
  tmeta_lv.id = generate_random_lvm_uuid();
  tmeta_lv.status = {"READ", "WRITE"};
  tmeta_lv.has_status = true;
  tmeta_lv.internal = true;
  if (!build_linear_segments(loaded->vg, meta_extents, tmeta_lv.segments, err)) {
    std::cerr << "error: failed to allocate thin-pool metadata extents: " << err << "\n";
    return 1;
  }
  loaded->vg.lvs.push_back(tmeta_lv);

  auto data_sectors = checked_mul(data_extents, loaded->vg.extent_size_sectors);
  if (!data_sectors) {
    std::cerr << "error: thin-pool data size arithmetic overflow\n";
    return 1;
  }
  const std::uint64_t nr_data_blocks = *data_sectors / chunk_sectors;
  if (!init_thin_pool_metadata(*loaded, tmeta_lv, nr_data_blocks, chunk_sectors, err)) {
    std::cerr << "error: failed to initialize dm-thin metadata: " << err << "\n";
    return 1;
  }

  LogicalVolume pool_lv;
  pool_lv.name = pool_name;
  pool_lv.id = generate_random_lvm_uuid();
  pool_lv.status = {"READ", "WRITE", "VISIBLE"};
  pool_lv.has_status = true;
  apply_tag_mutations(pool_lv.tags, add_tags, del_tags);

  Segment seg;
  seg.start_extent = 0;
  seg.extent_count = data_extents;
  seg.type = "thin-pool";
  seg.metadata_lv = tmeta_name;
  seg.pool_lv = tdata_name;
  seg.transaction_id = 0;
  seg.chunk_size_sectors = chunk_sectors;
  seg.discards = "passdown";
  seg.zero_new_blocks = true;
  pool_lv.segments.push_back(std::move(seg));

  loaded->vg.lvs.push_back(std::move(pool_lv));
  loaded->vg.seqno++;

  if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
    std::cerr << "error: failed to commit thin-pool metadata: " << err << "\n";
    return 1;
  }

#ifdef _WIN32
  driver_refresh();
#endif
  std::cout << "  Logical volume \"" << pool_name << "\" created.\n";
  return 0;
}

int create_thin_volume_lv(const std::vector<std::string>& args,
                          const std::vector<std::string>& pos,
                          const std::optional<std::string>& name_opt,
                          bool has_perm,
                          std::optional<bool> perm_rw,
                          const std::vector<std::string>& add_tags,
                          const std::vector<std::string>& del_tags) {
  if (!name_opt) {
    std::cerr << "error: thin LV creation requires -n/--name <thin_lv>\n";
    return 1;
  }
  const std::string& lv_name = *name_opt;
  std::string err;
  if (!validate_lv_name(lv_name, err)) {
    std::cerr << "error: invalid logical volume name \"" << lv_name << "\": " << err << "\n";
    return 1;
  }

  auto vsz_str = get_opt_value(args, "-V", "--virtualsize");
  if (!vsz_str) {
    std::cerr << "error: thin LV creation requires -V/--virtualsize <size>\n";
    return 1;
  }
  auto vsz_bytes = parse_size_bytes(*vsz_str, err);
  if (!vsz_bytes || *vsz_bytes == 0) {
    std::cerr << "error: invalid virtual size: " << err << "\n";
    return 1;
  }

  std::string pool_name;
  std::string vg_hint;
  std::vector<std::string> vg_pos = pos;

  if (auto tp_opt = get_opt_value(args, "", "--thinpool")) {
    auto [vg_p, lv_p] = split_vg_lv_spec(*tp_opt);
    pool_name = lv_p;
    vg_hint = vg_p;
  } else if (!pos.empty()) {
    auto [vg_p, lv_p] = split_vg_lv_spec(pos[0]);
    if (!vg_p.empty() && !lv_p.empty()) {
      vg_hint = vg_p;
      pool_name = lv_p;
      vg_pos[0] = vg_p;
    } else {
      std::cerr << "error: thin LV creation requires <vg>/<pool> or --thinpool <pool>\n";
      return 1;
    }
  }

  if (pool_name.empty()) {
    std::cerr << "error: thin LV creation requires a target thin pool\n";
    return 1;
  }
  if (vg_pos.empty()) {
    if (!vg_hint.empty()) vg_pos.push_back(vg_hint);
    else {
      std::cerr << "error: thin LV creation requires VG name and/or member PV path(s)\n";
      return 1;
    }
  }

  auto loaded = load_vg_flexible(vg_pos, true, err, true);
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
  if (loaded->vg.find_lv(lv_name)) {
    std::cerr << "error: logical volume \"" << lv_name << "\" already exists in VG "
              << loaded->vg.name << "\n";
    return 1;
  }

  LogicalVolume* pool_lv = nullptr;
  for (auto& lv : loaded->vg.lvs) {
    if (lv.name == pool_name) {
      pool_lv = &lv;
      break;
    }
  }
  if (!pool_lv || pool_lv->segments.empty() || pool_lv->segments[0].type != "thin-pool") {
    std::cerr << "error: thin-pool \"" << pool_name << "\" not found in VG \""
              << loaded->vg.name << "\"\n";
    return 1;
  }

  const LogicalVolume* tmeta_lv = loaded->vg.find_lv(pool_lv->segments[0].metadata_lv);
  if (!tmeta_lv) {
    std::cerr << "error: thin-pool metadata sub-volume \"" << pool_lv->segments[0].metadata_lv
              << "\" not found\n";
    return 1;
  }

  const std::uint64_t expected_vg_trans_id = pool_lv->segments[0].transaction_id;

  std::uint64_t max_dev_id = 0;
  for (const auto& lv : loaded->vg.lvs) {
    for (const auto& seg : lv.segments) {
      if (seg.type == "thin" && seg.pool_lv == pool_name && seg.device_id > max_dev_id) {
        max_dev_id = seg.device_id;
      }
    }
  }
  std::uint64_t tmeta_max_id = 0;
  if (!thin_pool_get_max_device_id(*loaded, *tmeta_lv, expected_vg_trans_id, tmeta_max_id, err)) {
    std::cerr << "error: failed to read thin-pool metadata: " << err << "\n";
    return 1;
  }
  const std::uint64_t next_dev_id = std::max(max_dev_id, tmeta_max_id) + 1ULL;

  auto ext_bytes_opt = checked_mul(loaded->vg.extent_size_sectors, 512ULL);
  if (!ext_bytes_opt || *ext_bytes_opt == 0) {
    std::cerr << "error: volume group extent size overflow or zero\n";
    return 1;
  }
  std::uint64_t virtual_extents = 0;
  ceil_div_u64(*vsz_bytes, *ext_bytes_opt, virtual_extents);
  virtual_extents = std::max<std::uint64_t>(1ULL, virtual_extents);

  std::uint64_t lv_trans_id = 0;
  std::uint64_t pool_trans_id = 0;
  if (!thin_pool_register_device(*loaded, *tmeta_lv, next_dev_id, expected_vg_trans_id,
                                 lv_trans_id, pool_trans_id, err)) {
    std::cerr << "error: failed to update thin-pool metadata: " << err << "\n";
    return 1;
  }

  pool_lv->segments[0].transaction_id = pool_trans_id;

  LogicalVolume thin_lv;
  thin_lv.name = lv_name;
  thin_lv.id = generate_random_lvm_uuid();
  thin_lv.has_status = true;
  if (has_perm && !*perm_rw) {
    thin_lv.status = {"READ", "VISIBLE"};
  } else {
    thin_lv.status = {"READ", "WRITE", "VISIBLE"};
  }
  apply_tag_mutations(thin_lv.tags, add_tags, del_tags);

  Segment seg;
  seg.start_extent = 0;
  seg.extent_count = virtual_extents;
  seg.type = "thin";
  seg.pool_lv = pool_name;
  seg.transaction_id = lv_trans_id;
  seg.device_id = next_dev_id;
  thin_lv.segments.push_back(std::move(seg));

  loaded->vg.lvs.push_back(std::move(thin_lv));
  loaded->vg.seqno++;

  if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
    std::cerr << "error: failed to commit thin LV metadata: " << err << "\n";
    return 1;
  }

#ifdef _WIN32
  driver_refresh();
#endif
  std::cout << "  Logical volume \"" << lv_name << "\" created.\n";
  return 0;
}

int create_snapshot_lv(const std::vector<std::string>& args,
                       const std::vector<std::string>& pos,
                       const std::optional<std::string>& name_opt,
                       bool has_perm,
                       std::optional<bool> perm_rw,
                       const std::vector<std::string>& add_tags,
                       const std::vector<std::string>& del_tags) {
  if (!name_opt) {
    std::cerr << "error: snapshot creation requires -n/--name <snap_lv>\n";
    return 1;
  }
  const std::string& snap_name = *name_opt;
  std::string err;
  if (!validate_lv_name(snap_name, err)) {
    std::cerr << "error: invalid snapshot name \"" << snap_name << "\": " << err << "\n";
    return 1;
  }
  if (pos.empty()) {
    std::cerr << "error: snapshot creation requires <vg>/<origin_lv> [PVPath...]\n";
    return 1;
  }

  auto [vg_hint, origin_name] = split_vg_lv_spec(pos[0]);
  if (origin_name.empty()) {
    std::cerr << "error: snapshot creation requires origin LV specification (<vg>/<origin_lv>)\n";
    return 1;
  }

  std::vector<std::string> vg_pos = pos;
  if (!vg_hint.empty()) {
    vg_pos[0] = vg_hint;
  } else if (pos.size() >= 2) {
    vg_pos.assign(pos.begin() + 1, pos.end());
  }

  auto loaded = load_vg_flexible(vg_pos, true, err, true);
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
  if (loaded->vg.find_lv(snap_name)) {
    std::cerr << "error: logical volume \"" << snap_name << "\" already exists in VG "
              << loaded->vg.name << "\n";
    return 1;
  }

  const LogicalVolume* origin_lv = loaded->vg.find_lv(origin_name);
  if (!origin_lv || origin_lv->internal || origin_lv->segments.empty()) {
    std::cerr << "error: origin logical volume \"" << origin_name << "\" not found in VG \""
              << loaded->vg.name << "\"\n";
    return 1;
  }
  const std::string& orig_type = origin_lv->segments[0].type;
  if (orig_type != "linear" && orig_type != "striped") {
    std::cerr << "error: classic snapshot origin \"" << origin_name
              << "\" must be linear or striped (found \"" << orig_type << "\")\n";
    return 1;
  }

  std::uint64_t cow_extents = 0;
  if (!resolve_extents_arg(loaded->vg, args, 1, cow_extents, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  std::uint32_t chunk_sectors = 8u;  // 4 KiB default (8 * 512B)
  if (auto csz_str = get_opt_value(args, "-c", "--chunksize")) {
    auto csz = parse_size_bytes(*csz_str, err);
    if (!csz || *csz < 4096ULL || *csz > 1048576ULL || (*csz % 512ULL) != 0) {
      std::cerr << "error: invalid snapshot --chunksize (must be 4 KiB..1 MiB, power of 2)\n";
      return 1;
    }
    const std::uint64_t csec = *csz / 512ULL;
    if ((csec & (csec - 1ULL)) != 0) {
      std::cerr << "error: snapshot --chunksize must be a power of 2\n";
      return 1;
    }
    chunk_sectors = static_cast<std::uint32_t>(csec);
  }

  const std::uint64_t origin_extents = origin_lv->extent_count();

  // Allocate COW LV (hidden status without VISIBLE, matching upstream LVM2).
  LogicalVolume cow_lv;
  cow_lv.name = snap_name;
  cow_lv.id = generate_random_lvm_uuid();
  cow_lv.has_status = true;
  if (has_perm && !*perm_rw) {
    cow_lv.status = {"READ"};
  } else {
    cow_lv.status = {"READ", "WRITE"};
  }
  cow_lv.internal = true;
  apply_tag_mutations(cow_lv.tags, add_tags, del_tags);

  if (!build_linear_segments(loaded->vg, cow_extents, cow_lv.segments, err)) {
    std::cerr << "error: failed to allocate snapshot COW extents: " << err << "\n";
    return 1;
  }

  if (!init_snapshot_cow_header(*loaded, cow_lv, chunk_sectors, err)) {
    std::cerr << "error: failed to initialize dm-snapshot COW header: " << err << "\n";
    return 1;
  }

  LogicalVolume snap_wrapper;
  snap_wrapper.name = next_snapshot_wrapper_name(loaded->vg);
  snap_wrapper.id = generate_random_lvm_uuid();
  snap_wrapper.has_status = true;
  if (has_perm && !*perm_rw) {
    snap_wrapper.status = {"READ", "VISIBLE"};
  } else {
    snap_wrapper.status = {"READ", "WRITE", "VISIBLE"};
  }

  Segment seg;
  seg.start_extent = 0;
  seg.extent_count = origin_extents;
  seg.type = "snapshot";
  seg.chunk_size_sectors = chunk_sectors;
  seg.origin_lv = origin_name;
  seg.cow_store_lv = snap_name;
  snap_wrapper.segments.push_back(std::move(seg));

  loaded->vg.lvs.push_back(std::move(cow_lv));
  loaded->vg.lvs.push_back(std::move(snap_wrapper));
  loaded->vg.seqno++;

  if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
    std::cerr << "error: failed to commit snapshot metadata: " << err << "\n";
    return 1;
  }

#ifdef _WIN32
  driver_refresh();
#endif
  std::cout << "  Logical volume \"" << snap_name << "\" created.\n";
  return 0;
}

}  // namespace

int cmd_lvcreate(const std::vector<std::string>& args) {
  if (check_help_requested(args, "lvcreate")) return 0;

  std::vector<std::string> add_tags;
  std::vector<std::string> del_tags;
  std::string err;
  if (!parse_tag_mutations(args, add_tags, del_tags, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  bool has_perm = false;
  auto perm_rw = parse_permission_flag(args, has_perm, err);
  if (has_perm && !perm_rw) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  const std::set<std::string> val_flags = {
      "-n", "--name", "-L", "--size", "-l", "--extents",
      "-i", "--stripes", "-I", "--stripesize",
      "-p", "--permission", "--type",
      "--poolmetadatasize", "--thinpool", "-V", "--virtualsize",
      "-c", "--chunksize", "--addtag", "--deltag",
  };
  const auto pos = get_positional_args_for(args, val_flags);
  const auto lv_name_opt = get_opt_value(args, "-n", "--name");
  const auto type_opt = get_opt_value(args, "", "--type");
  const bool has_thin_flag = has_flag(args, "-T", "--thin");
  const bool has_snap_flag = has_flag(args, "-s", "--snapshot");
  const bool has_vsize = get_opt_value(args, "-V", "--virtualsize").has_value();
  const bool has_thinpool_opt = get_opt_value(args, "", "--thinpool").has_value();

  if (type_opt) {
    if (*type_opt == "thin-pool") {
      return create_thin_pool_lv(args, pos, lv_name_opt, add_tags, del_tags);
    }
    if (*type_opt == "thin") {
      return create_thin_volume_lv(args, pos, lv_name_opt, has_perm, perm_rw, add_tags, del_tags);
    }
    if (*type_opt == "snapshot") {
      return create_snapshot_lv(args, pos, lv_name_opt, has_perm, perm_rw, add_tags, del_tags);
    }
    if (*type_opt != "linear" && *type_opt != "striped") {
      std::cerr << "error: unsupported --type \"" << *type_opt
                << "\" for lvcreate (supported: linear, striped, thin-pool, thin, snapshot)\n";
      return 1;
    }
  }

  if (has_snap_flag) {
    return create_snapshot_lv(args, pos, lv_name_opt, has_perm, perm_rw, add_tags, del_tags);
  }
  if (has_vsize || has_thinpool_opt) {
    return create_thin_volume_lv(args, pos, lv_name_opt, has_perm, perm_rw, add_tags, del_tags);
  }
  if (has_thin_flag) {
    return create_thin_pool_lv(args, pos, lv_name_opt, add_tags, del_tags);
  }

  if (!lv_name_opt) {
    std::cerr << "error: lvcreate requires logical volume name via -n/--name\n";
    print_command_help("lvcreate");
    return 1;
  }
  const std::string lv_name = *lv_name_opt;
  if (!validate_lv_name(lv_name, err)) {
    std::cerr << "error: invalid logical volume name \"" << lv_name << "\": " << err << "\n";
    return 1;
  }

  if (pos.empty()) {
    std::cerr << "error: lvcreate requires VG name and/or member PV path(s)\n";
    print_command_help("lvcreate");
    return 1;
  }

  auto loaded = load_vg_flexible(pos, true, err, true);
  if (!loaded) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  if (!check_vg_mutable(loaded->vg, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  for (const auto& lv : loaded->vg.lvs) {
    if (lv.name == lv_name) {
      std::cerr << "error: logical volume \"" << lv_name << "\" already exists in VG "
                << loaded->vg.name << "\n";
      return 1;
    }
  }

  std::uint32_t stripes = 1;
  if (auto str_stripes = get_opt_value(args, "-i", "--stripes")) {
    try {
      std::size_t idx = 0;
      const unsigned long val = std::stoul(*str_stripes, &idx);
      if (idx != str_stripes->size() || val == 0 || val > 128) {
        std::cerr << "error: invalid stripe count: " << *str_stripes << "\n";
        return 1;
      }
      stripes = static_cast<std::uint32_t>(val);
    } catch (...) {
      std::cerr << "error: invalid stripe count: " << *str_stripes << "\n";
      return 1;
    }
  }

  std::uint64_t extents_needed = 0;
  if (!resolve_extents_arg(loaded->vg, args, stripes, extents_needed, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  LogicalVolume lv;
  lv.name = lv_name;
  lv.id = generate_random_lvm_uuid();
  lv.supported = true;
  lv.has_status = true;
  if (has_perm && !*perm_rw) {
    lv.status = {"READ", "VISIBLE"};
  } else {
    lv.status = {"READ", "WRITE", "VISIBLE"};
  }
  apply_tag_mutations(lv.tags, add_tags, del_tags);

  if (stripes <= 1) {
    if (!build_linear_segments(loaded->vg, extents_needed, lv.segments, err)) {
      std::cerr << "error: allocation failed: " << err << "\n";
      return 1;
    }
  } else {
    std::uint32_t stripe_size_sectors = 128;  // 64 KiB default
    if (auto str_stripe_size = get_opt_value(args, "-I", "--stripesize")) {
      std::string err_sz;
      auto sz = parse_size_bytes(*str_stripe_size, err_sz);
      if (!sz || *sz < 4096 || (*sz % 512 != 0)) {
        std::cerr << "error: invalid stripe size: " << *str_stripe_size << "\n";
        return 1;
      }
      const std::uint64_t ssec = *sz / 512ULL;
      if ((ssec & (ssec - 1ULL)) != 0 || ssec > loaded->vg.extent_size_sectors) {
        std::cerr << "error: stripe size must be a power of 2 and <= extent size\n";
        return 1;
      }
      stripe_size_sectors = static_cast<std::uint32_t>(ssec);
    }

    if (!build_striped_segments(loaded->vg, extents_needed, stripes, stripe_size_sectors,
                                lv.segments, err)) {
      std::cerr << "error: striped allocation failed: " << err << "\n";
      return 1;
    }
  }

  const std::string created_uuid = lv.id;
  loaded->vg.lvs.push_back(std::move(lv));
  loaded->vg.seqno++;

  if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
    std::cerr << "error: failed to commit metadata: " << err << "\n";
    return 1;
  }

#ifdef _WIN32
  driver_refresh();
  driver_activate_lv(created_uuid, true);
#else
  (void)created_uuid;
#endif

  std::cout << "  Logical volume \"" << lv_name << "\" created.\n";
  return 0;
}

}  // namespace lvmfmt
