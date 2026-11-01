#include "subcommands.h"
#include "cmd_common.h"
#include "lv_extent_alloc.h"

#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace lvmfmt {

namespace {

std::string generate_lv_uuid() {
  static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<std::size_t> dis(0, sizeof(chars) - 2);

  std::string s;
  s.reserve(32);
  for (int i = 0; i < 32; ++i) s.push_back(chars[dis(gen)]);
  return s;
}

}  // namespace

int cmd_lvcreate(const std::vector<std::string>& args) {
  if (check_help_requested(args, "lvcreate")) return 0;

  auto lv_name_opt = get_opt_value(args, "-n", "--name");
  if (!lv_name_opt) {
    std::cerr << "error: lvcreate requires logical volume name via -n/--name\n";
    print_command_help("lvcreate");
    return 1;
  }
  const std::string lv_name = *lv_name_opt;
  std::string err;
  if (!validate_lv_name(lv_name, err)) {
    std::cerr << "error: invalid logical volume name \"" << lv_name << "\": " << err << "\n";
    return 1;
  }

  bool has_perm = false;
  auto perm_rw = parse_permission_flag(args, has_perm, err);
  if (has_perm && !perm_rw) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  auto pos = get_positional_args(args);
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

  const std::uint64_t extent_bytes = loaded->vg.extent_size_sectors * 512ULL;
  std::uint64_t extents_needed = 0;

  if (auto sz_str = get_opt_value(args, "-L", "--size")) {
    auto spec = parse_size_spec(*sz_str, err);
    if (!spec || spec->sign == SizeSign::Minus || spec->value == 0) {
      std::cerr << "error: invalid size: " << (err.empty() ? *sz_str : err) << "\n";
      return 1;
    }
    extents_needed = (spec->value + extent_bytes - 1ULL) / extent_bytes;
  } else if (auto l_str = get_opt_value(args, "-l", "--extents")) {
    auto spec = parse_extents_spec(*l_str, err);
    if (!spec || spec->sign == SizeSign::Minus || spec->value == 0) {
      std::cerr << "error: invalid extent specification: " << (err.empty() ? *l_str : err) << "\n";
      return 1;
    }
    if (spec->is_percent) {
      auto maps = build_vg_extent_maps(loaded->vg);
      std::uint64_t free_extents = 0;
      std::uint64_t total_extents = 0;
      for (const auto& m : maps) {
        if (m.allocatable) free_extents += m.free_extents;
        total_extents += m.total_extents;
      }
      std::uint64_t base = 0;
      if (spec->percent_origin == "FREE") base = free_extents;
      else if (spec->percent_origin == "VG" || spec->percent_origin == "PVS") base = total_extents;
      else {
        std::cerr << "error: percentage origin %" << spec->percent_origin
                  << " is not valid for lvcreate\n";
        return 1;
      }
      extents_needed = (base * spec->value) / 100ULL;
      if (stripes > 1) extents_needed = (extents_needed / stripes) * stripes;
    } else {
      extents_needed = spec->value;
    }
  } else {
    std::cerr << "error: lvcreate requires either -L <size> or -l <extents>\n";
    return 1;
  }

  if (extents_needed == 0) {
    std::cerr << "error: requested extent count must be > 0\n";
    return 1;
  }

  LogicalVolume lv;
  lv.name = lv_name;
  lv.id = generate_lv_uuid();
  lv.supported = true;
  if (has_perm && !*perm_rw) {
    lv.status = {"READ", "VISIBLE"};
  } else {
    lv.status = {"READ", "WRITE", "VISIBLE"};
  }

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
