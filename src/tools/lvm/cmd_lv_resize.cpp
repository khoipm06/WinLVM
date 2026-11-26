#include "subcommands.h"
#include "cmd_common.h"

#include <iostream>
#include <string>
#include <vector>

namespace lvmfmt {

namespace {

enum class ResizeMode { Extend, Reduce, Resize };

int execute_lv_resize(const std::vector<std::string>& args, ResizeMode mode,
                      std::string_view cmd_name) {
  if (check_help_requested(args, cmd_name)) return 0;

  auto pos = get_positional_args(args);
  if (pos.empty()) {
    std::cerr << "error: " << cmd_name << " requires [VGName/]LVName and member PV path(s)\n";
    print_command_help(cmd_name);
    return 1;
  }

  auto [vg_hint, lv_name] = split_vg_lv_spec(pos[0]);
  std::vector<std::string> pv_paths(pos.begin() + 1, pos.end());

#ifdef _WIN32
  if (pv_paths.empty()) {
    pv_paths = discover_win32_pv_paths_for_vg(vg_hint);
  }
#endif
  if (pv_paths.empty()) {
    std::cerr << "error: " << cmd_name << " requires member PV path(s)\n";
    print_command_help(cmd_name);
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

  const LogicalVolume* lv = loaded->vg.find_lv(lv_name);
  if (!lv) {
    std::cerr << "error: logical volume \"" << lv_name << "\" not found in VG \""
              << loaded->vg.name << "\"\n";
    return 1;
  }
  if (!lv->supported || lv->internal) {
    std::cerr << "error: cannot resize " << (lv->internal ? "internal" : "unsupported")
              << " logical volume \"" << lv_name << "\""
              << (lv->unsupported_reason.empty() ? "" : (" (" + lv->unsupported_reason + ")"))
              << "\n";
    return 1;
  }
  for (const auto& seg : lv->segments) {
    if (seg.type != "linear" && seg.type != "striped") {
      std::cerr << "error: " << cmd_name << " does not support segment type \"" << seg.type
                << "\" on logical volume \"" << lv_name << "\"\n";
      return 1;
    }
  }
  for (const auto& other : loaded->vg.lvs) {
    for (const auto& seg : other.segments) {
      if (seg.type == "snapshot" &&
          (seg.origin_lv == lv_name || seg.cow_store_lv == lv_name)) {
        std::cerr << "error: " << cmd_name
                  << " does not support snapshot or snapshot origin logical volume \"" << lv_name
                  << "\"\n";
        return 1;
      }
    }
  }

  const std::uint64_t ext_bytes = loaded->vg.extent_size_sectors * 512ULL;
  const std::uint64_t cur_extents = lv->extent_count();
  const std::uint64_t stripe_count =
      (!lv->segments.empty() && lv->segments.back().stripe_count > 1)
          ? lv->segments.back().stripe_count
          : 1ULL;

  auto maps = build_vg_extent_maps(loaded->vg);
  std::uint64_t vg_free_extents = 0;
  std::uint64_t vg_total_extents = 0;
  for (const auto& m : maps) {
    if (m.allocatable) vg_free_extents += m.free_extents;
    vg_total_extents += m.total_extents;
  }

  SizeSpec spec{};
  std::uint64_t target_extents = 0;

  if (auto sz_str = get_opt_value(args, "-L", "--size")) {
    auto parsed = parse_size_spec(*sz_str, err);
    if (!parsed) {
      std::cerr << "error: invalid size: " << err << "\n";
      return 1;
    }
    spec = *parsed;
    if (spec.value == 0) {
      std::cerr << "error: size must be greater than zero\n";
      return 1;
    }
    const std::uint64_t req_extents = (spec.value + ext_bytes - 1ULL) / ext_bytes;
    if (spec.sign == SizeSign::Plus) {
      target_extents = cur_extents + req_extents;
    } else if (spec.sign == SizeSign::Minus) {
      if (req_extents >= cur_extents) {
        std::cerr << "error: cannot reduce LV by " << req_extents
                  << " extents (current size is " << cur_extents << " extents)\n";
        return 1;
      }
      target_extents = cur_extents - req_extents;
    } else {
      target_extents = req_extents;
    }
  } else if (auto l_str = get_opt_value(args, "-l", "--extents")) {
    auto parsed = parse_extents_spec(*l_str, err);
    if (!parsed) {
      std::cerr << "error: invalid extent specification: " << err << "\n";
      return 1;
    }
    spec = *parsed;
    if (spec.value == 0) {
      std::cerr << "error: extent count must be greater than zero\n";
      return 1;
    }
    std::uint64_t count = spec.value;
    if (spec.is_percent) {
      std::uint64_t base = 0;
      if (spec.percent_origin == "FREE") base = vg_free_extents;
      else if (spec.percent_origin == "VG" || spec.percent_origin == "PVS") base = vg_total_extents;
      else if (spec.percent_origin == "LV" || spec.percent_origin == "ORIGIN") base = cur_extents;
      count = (base * spec.value) / 100ULL;
      if (stripe_count > 1) {
        count = (count / stripe_count) * stripe_count;
      }
    }

    if (spec.sign == SizeSign::Plus) {
      target_extents = cur_extents + count;
    } else if (spec.sign == SizeSign::Minus) {
      if (count >= cur_extents) {
        std::cerr << "error: cannot reduce LV by " << count << " extents (current size is "
                  << cur_extents << " extents)\n";
        return 1;
      }
      target_extents = cur_extents - count;
    } else {
      target_extents = count;
    }
  } else {
    std::cerr << "error: " << cmd_name << " requires -L/--size or -l/--extents\n";
    return 1;
  }

  if (mode == ResizeMode::Extend) {
    if (spec.sign == SizeSign::Minus) {
      std::cerr << "error: lvextend does not accept a negative (-) size delta\n";
      return 1;
    }
    if (target_extents <= cur_extents) {
      std::cerr << "error: new size (" << target_extents
                << " extents) is not larger than current size (" << cur_extents << " extents)\n";
      return 1;
    }
  } else if (mode == ResizeMode::Reduce) {
    if (spec.sign == SizeSign::Plus) {
      std::cerr << "error: lvreduce does not accept a positive (+) size delta\n";
      return 1;
    }
    if (target_extents == 0 || target_extents >= cur_extents) {
      std::cerr << "error: new size (" << target_extents
                << " extents) must be strictly smaller than current size (" << cur_extents
                << " extents) and > 0\n";
      return 1;
    }
  } else {
    if (target_extents == 0) {
      std::cerr << "error: target LV size must be > 0 extents\n";
      return 1;
    }
    if (target_extents == cur_extents) {
      std::cerr << "error: new size (" << target_extents
                << " extents) matches current size (nothing to do)\n";
      return 1;
    }
  }

  if (target_extents > cur_extents) {
    if (!extend_lv(loaded->vg, lv_name, target_extents - cur_extents, {}, err)) {
      std::cerr << "error: extend failed: " << err << "\n";
      return 1;
    }
  } else {
    if (!reduce_lv(loaded->vg, lv_name, cur_extents - target_extents, err)) {
      std::cerr << "error: reduce failed: " << err << "\n";
      return 1;
    }
  }

  if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
    std::cerr << "error: failed to commit metadata: " << err << "\n";
    return 1;
  }

#ifdef _WIN32
  std::uint32_t logical_sector = 512;
  std::uint32_t physical_sector = 512;
  loaded->max_sector_geometry(logical_sector, physical_sector);
  if (const LogicalVolume* updated = loaded->vg.find_lv(lv_name)) {
    driver_update_lv(updated->id, updated->extent_count() * ext_bytes,
                     logical_sector, physical_sector);
  }
#endif

  std::cout << "  Logical volume " << loaded->vg.name << "/" << lv_name << " successfully resized\n";
  return 0;
}

}  // namespace

int cmd_lvextend(const std::vector<std::string>& args) {
  return execute_lv_resize(args, ResizeMode::Extend, "lvextend");
}

int cmd_lvreduce(const std::vector<std::string>& args) {
  return execute_lv_resize(args, ResizeMode::Reduce, "lvreduce");
}

int cmd_lvresize(const std::vector<std::string>& args) {
  return execute_lv_resize(args, ResizeMode::Resize, "lvresize");
}

}  // namespace lvmfmt
