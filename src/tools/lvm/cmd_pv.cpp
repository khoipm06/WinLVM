#include "subcommands.h"
#include "cmd_common.h"
#include "report_json.h"
#include "lvmfmt/bytes.h"
#include "lvmfmt/crc32.h"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>

namespace lvmfmt {

namespace {

std::uint64_t align_up_u64(std::uint64_t val, std::uint64_t align) {
  if (align == 0) return val;
  const std::uint64_t rem = val % align;
  return rem ? (val + (align - rem)) : val;
}

void write_u32_le(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xffu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xffu);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xffu);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xffu);
}

bool pv_rewrite_label_uuid(ByteSource& storage,
                           const PvLabel& label,
                           std::string_view new_raw32_uuid,
                           std::vector<std::uint8_t>& orig_sector_out,
                           std::string& error) {
  if (new_raw32_uuid.size() != 32) {
    error = "new PV UUID must be 32 raw characters";
    return false;
  }
  if (!storage.read_vec(label.label_offset, kLabelSize, orig_sector_out) ||
      orig_sector_out.size() != kLabelSize) {
    error = "failed to read PV label sector";
    return false;
  }
  std::vector<std::uint8_t> sec = orig_sector_out;
  const std::uint32_t data_off = le32(&sec[20]);
  if (data_off < 32 || data_off + 40 > kLabelSize) {
    error = "invalid PV header offset in label sector";
    return false;
  }
  std::memcpy(&sec[data_off], new_raw32_uuid.data(), 32);
  write_u32_le(&sec[16], lvm_crc32(&sec[20], kLabelSize - 20));
  if (!storage.write(label.label_offset, sec.data(), sec.size(), error)) {
    error = "failed to write updated PV label UUID: " + error;
    return false;
  }
  return true;
}

}  // namespace

int cmd_pvcreate(const std::vector<std::string>& args) {
  if (check_help_requested(args, "pvcreate")) return 0;

  std::vector<std::string> add_tags;
  std::vector<std::string> del_tags;
  std::string tag_err;
  if (!parse_tag_mutations(args, add_tags, del_tags, tag_err)) {
    std::cerr << "error: " << tag_err << "\n";
    return 1;
  }
  if (!add_tags.empty() || !del_tags.empty()) {
    std::cerr << "error: pvcreate cannot persist PV tags on an orphan PV; "
                 "create a volume group first and use pvchange --addtag\n";
    return 1;
  }

  const std::set<std::string> val_flags = {
      "-u", "--uuid", "--setphysicalvolumesize", "--metadatasize", "--dataalignment",
      "--addtag", "--deltag",
  };
  auto pos = get_positional_args_for(args, val_flags);
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
  const std::string sep = sep_opt ? *sep_opt : "  ";
  const bool show_tags = options_requests_column(opt_cols, "tags", "pv_tags");
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

  std::vector<PvReportRow> rows;
  std::set<std::string> seen_pv_uuids;
  for (const auto& dev : pos) {
    std::string local_err;
    auto storage = open_storage_source(dev, false, local_err);
    if (!storage) {
      if (!auto_scan) std::cerr << "error: " << dev << ": " << local_err << "\n";
      continue;
    }

    auto label = read_pv_label(*storage, local_err);
    if (!label) {
      if (!auto_scan) std::cerr << "error: " << dev << ": " << local_err << "\n";
      continue;
    }
    const std::string raw_pv_uuid = normalize_uuid_raw32(label->pv_uuid);
    if (auto_scan && !seen_pv_uuids.insert(raw_pv_uuid).second) {
      continue;
    }

    PvReportRow row;
    row.pv_name = dev;
    row.pv_fmt = "lvm2";
    row.pv_attr = "---";
    row.pv_size = format_size_human(label->device_size);
    row.pv_free = row.pv_size;
    row.pv_uuid = format_uuid(label->pv_uuid);

    auto meta = read_vg_metadata(*storage, *label, local_err);
    if (meta) {
      auto vg = parse_vg_metadata(*meta, local_err);
      if (vg) {
        row.vg_name = vg->name;
        const PhysicalVolume* pv_entry = vg->find_pv_by_uuid(label->pv_uuid);
        if (pv_entry) {
          row.pv_attr[0] = pv_entry->is_allocatable() ? 'a' : '-';
          row.pv_attr[1] = vg->is_exported() ? 'x' : '-';
          row.pv_attr[2] = pv_entry->is_missing() ? 'm' : '-';
          row.pv_tags = pv_entry->tags;
        }
        auto maps = build_vg_extent_maps(*vg);
        const std::string raw_uuid = normalize_uuid_raw32(label->pv_uuid);
        for (const auto& m : maps) {
          if (normalize_uuid_raw32(m.pv_uuid) == raw_uuid) {
            row.pv_free = format_size_human(m.free_extents * vg->extent_size_sectors * 512ULL);
            break;
          }
        }
      }
    }

    if (!matches_tag_filters(row.pv_tags, tag_filters)) continue;
    rows.push_back(std::move(row));
  }

  if (fmt == ReportFormat::Json || fmt == ReportFormat::JsonStd) {
    print_pvs_json(rows, fmt);
    return 0;
  }

  if (!noheadings) {
    std::cout << "  PV         VG         Fmt  Attr PSize   PFree   PV UUID";
    if (show_tags) std::cout << "   PV Tags";
    std::cout << "\n";
  }

  for (const auto& r : rows) {
    std::cout << "  " << r.pv_name << sep << (r.vg_name.empty() ? "[orphan]" : r.vg_name)
              << sep << r.pv_fmt << " " << r.pv_attr << sep << r.pv_size
              << sep << r.pv_free << sep << r.pv_uuid;
    if (show_tags) std::cout << sep << join_tags_csv(r.pv_tags);
    std::cout << "\n";
  }

  return 0;
}

int cmd_pvdisplay(const std::vector<std::string>& args) {
  if (check_help_requested(args, "pvdisplay")) return 0;

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

  std::vector<PvReportRow> json_rows;
  std::set<std::string> seen_pv_uuids;
  for (const auto& dev : pos) {
    std::string local_err;
    auto storage = open_storage_source(dev, false, local_err);
    if (!storage) {
      if (!auto_scan) std::cerr << "error: " << dev << ": " << local_err << "\n";
      continue;
    }

    auto label = read_pv_label(*storage, local_err);
    if (!label) {
      if (!auto_scan) std::cerr << "error: " << dev << ": " << local_err << "\n";
      continue;
    }
    const std::string raw_pv_uuid = normalize_uuid_raw32(label->pv_uuid);
    if (auto_scan && !seen_pv_uuids.insert(raw_pv_uuid).second) {
      continue;
    }

    std::optional<VolumeGroup> vg_opt;
    auto meta = read_vg_metadata(*storage, *label, local_err);
    if (meta) vg_opt = parse_vg_metadata(*meta, local_err);

    const PhysicalVolume* pv_entry = vg_opt ? vg_opt->find_pv_by_uuid(label->pv_uuid) : nullptr;
    const std::vector<std::string> pv_tags = pv_entry ? pv_entry->tags : std::vector<std::string>{};
    if (!matches_tag_filters(pv_tags, tag_filters)) continue;

    if (fmt == ReportFormat::Json || fmt == ReportFormat::JsonStd) {
      PvReportRow row;
      row.pv_name = dev;
      row.pv_fmt = "lvm2";
      row.pv_attr = "---";
      row.pv_size = format_size_human(label->device_size);
      row.pv_free = row.pv_size;
      row.pv_uuid = format_uuid(label->pv_uuid);
      row.pv_tags = pv_tags;
      if (vg_opt) {
        row.vg_name = vg_opt->name;
        if (pv_entry) {
          row.pv_attr[0] = pv_entry->is_allocatable() ? 'a' : '-';
          row.pv_attr[1] = vg_opt->is_exported() ? 'x' : '-';
          row.pv_attr[2] = pv_entry->is_missing() ? 'm' : '-';
        }
        auto maps = build_vg_extent_maps(*vg_opt);
        for (const auto& m : maps) {
          if (normalize_uuid_raw32(m.pv_uuid) == raw_pv_uuid) {
            row.pv_free = format_size_human(m.free_extents * vg_opt->extent_size_sectors * 512ULL);
            break;
          }
        }
      }
      json_rows.push_back(std::move(row));
      continue;
    }

    std::cout << "  --- Physical volume ---\n"
              << "  PV Name               " << dev << "\n"
              << "  PV UUID               " << format_uuid(label->pv_uuid) << "\n"
              << "  PV Size               " << format_size_human(label->device_size) << "\n";

    if (vg_opt) {
      std::cout << "  VG Name               " << vg_opt->name << "\n"
                << "  VG UUID               " << format_uuid(vg_opt->id) << "\n"
                << "  Allocatable           "
                << ((pv_entry && pv_entry->is_allocatable()) ? "yes" : "no") << "\n"
                << "  PE Size               "
                << format_size_human(vg_opt->extent_size_sectors * 512ULL) << "\n";
      auto maps = build_vg_extent_maps(*vg_opt);
      for (const auto& m : maps) {
        if (normalize_uuid_raw32(m.pv_uuid) == raw_pv_uuid) {
          std::cout << "  Total PE              " << m.total_extents << "\n"
                    << "  Allocated PE          " << (m.total_extents - m.free_extents) << "\n"
                    << "  Free PE               " << m.free_extents << "\n";
          break;
        }
      }
      if (!pv_tags.empty()) {
        std::cout << "  PV Tags               " << join_tags_csv(pv_tags) << "\n";
      }
    }
    std::cout << "\n";
  }

  if (fmt == ReportFormat::Json || fmt == ReportFormat::JsonStd) {
    print_pvs_json(json_rows, fmt);
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

  bool has_alloc = false;
  bool alloc_val = true;
  if (auto x_val = get_opt_value(args, "-x", "--allocatable")) {
    has_alloc = true;
    if (*x_val == "y" || *x_val == "yes") {
      alloc_val = true;
    } else if (*x_val == "n" || *x_val == "no") {
      alloc_val = false;
    } else {
      std::cerr << "error: invalid --allocatable value \"" << *x_val << "\" (expected y or n)\n";
      return 1;
    }
  }

  const bool change_uuid = has_flag(args, "-u", "--uuid");
  const bool apply_all = has_flag(args, "-a", "--all");

  std::vector<std::string> add_tags;
  std::vector<std::string> del_tags;
  std::string err;
  if (!parse_tag_mutations(args, add_tags, del_tags, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  const bool has_tags = !add_tags.empty() || !del_tags.empty();
  if (!has_alloc && !change_uuid && !has_tags) {
    std::cerr << "error: pvchange requires -x/--allocatable, -u/--uuid, --addtag, or --deltag\n";
    print_command_help("pvchange");
    return 1;
  }

  const std::set<std::string> val_flags = {"-x", "--allocatable", "--addtag", "--deltag"};
  auto pos = get_positional_args_for(args, val_flags);

#ifdef _WIN32
  if (pos.empty() && apply_all) {
    for (const auto& c : discover_win32_candidate_devices()) {
      std::string probe_err;
      auto s = open_storage_source(c.open_path, false, probe_err);
      if (s && read_pv_label(*s, probe_err)) {
        pos.push_back(c.open_path);
      }
    }
  }
#endif

  if (pos.empty()) {
    std::cerr << "error: pvchange requires physical volume path(s)\n";
    return 1;
  }

  // Probe all passed PVs to classify VG groups vs orphan PVs cleanly.
  struct PvProbe {
    std::string path;
    std::string pv_uuid;
    std::string vg_uuid;  // empty if orphan
  };
  std::vector<PvProbe> probes;
  for (const auto& p : pos) {
    auto src = open_storage_source(p, false, err);
    if (!src) {
      std::cerr << "error: cannot open " << p << ": " << err << "\n";
      return 1;
    }
    auto lbl = read_pv_label(*src, err);
    if (!lbl) {
      std::cerr << "error: cannot read PV label on " << p << ": " << err << "\n";
      return 1;
    }
    PvProbe pr;
    pr.path = p;
    pr.pv_uuid = normalize_uuid_raw32(lbl->pv_uuid);
    std::string meta_err;
    if (auto meta = read_vg_metadata(*src, *lbl, meta_err)) {
      if (auto vg = parse_vg_metadata(*meta, meta_err)) {
        pr.vg_uuid = normalize_uuid_raw32(vg->id);
      }
    }
    probes.push_back(std::move(pr));
  }

  // Determine which PVs are targeted vs sibling context PVs.
  // If -a/--all is absent and all passed PVs belong to the SAME VG (size >= 2),
  // pos[0] is the target PV and pos[1..] provide the sibling PVs for VG commit.
  bool single_target_with_siblings = false;
  if (!apply_all && probes.size() >= 2 && !probes[0].vg_uuid.empty()) {
    bool all_same_vg = true;
    for (std::size_t i = 1; i < probes.size(); ++i) {
      if (probes[i].vg_uuid != probes[0].vg_uuid) {
        all_same_vg = false;
        break;
      }
    }
    if (all_same_vg) single_target_with_siblings = true;
  }

  // Group PVs by VG UUID (preserving order) + orphan PVs.
  std::map<std::string, std::vector<std::string>> vg_groups;
  std::vector<std::string> vg_order;
  std::vector<std::string> orphan_paths;
  for (const auto& pr : probes) {
    if (pr.vg_uuid.empty()) {
      orphan_paths.push_back(pr.path);
    } else {
      if (!vg_groups.count(pr.vg_uuid)) vg_order.push_back(pr.vg_uuid);
      vg_groups[pr.vg_uuid].push_back(pr.path);
    }
  }

  std::size_t changed_count = 0;

  // Process VG-backed PVs.
  for (const auto& vg_id : vg_order) {
    const auto& group_paths = vg_groups[vg_id];
    auto loaded = load_vg_from_pvs(group_paths, true, err, true);
    if (!loaded) {
      std::cerr << "error: " << err << "\n";
      return 1;
    }
    if (!check_vg_mutable(loaded->vg, err)) {
      std::cerr << "error: " << err << "\n";
      return 1;
    }

    std::set<std::string> target_pv_uuids;
    if (single_target_with_siblings) {
      target_pv_uuids.insert(probes[0].pv_uuid);
    } else {
      for (const auto& pr : probes) {
        if (pr.vg_uuid == vg_id) target_pv_uuids.insert(pr.pv_uuid);
      }
    }

    struct PendingUuidBackup {
      std::size_t storage_idx = 0;
      std::uint64_t label_offset = 0;
      std::vector<std::uint8_t> orig_sector;
      std::string new_uuid;
    };
    std::vector<PendingUuidBackup> uuid_updates;

    for (auto& pv : loaded->vg.pvs) {
      const std::string raw_old = normalize_uuid_raw32(pv.id);
      if (!target_pv_uuids.count(raw_old)) continue;

      if (has_alloc) {
        pv.has_status = true;
        set_status_flag(pv.status, "ALLOCATABLE", alloc_val);
      }
      if (has_tags) {
        apply_tag_mutations(pv.tags, add_tags, del_tags);
      }
      if (change_uuid) {
        std::size_t sidx = loaded->pv_labels.size();
        for (std::size_t i = 0; i < loaded->pv_labels.size(); ++i) {
          if (normalize_uuid_raw32(loaded->pv_labels[i].pv_uuid) == raw_old) {
            sidx = i;
            break;
          }
        }
        if (sidx >= loaded->storages.size()) {
          std::cerr << "error: backing storage for target PV not found\n";
          return 1;
        }
        const std::string fresh_uuid = generate_random_lvm_uuid();
        pv.id = fresh_uuid;
        uuid_updates.push_back({sidx, loaded->pv_labels[sidx].label_offset, {}, fresh_uuid});
      }
      changed_count++;
    }

    loaded->vg.seqno++;

    // Validate serialization before modifying any PV LABELONE sector.
    if (!serialize_vg_to_text(loaded->vg, err)) {
      std::cerr << "error: " << err << "\n";
      return 1;
    }

    // Rewrite PV LABELONE UUIDs first (keeping rollback snapshots).
    for (auto& upd : uuid_updates) {
      if (!pv_rewrite_label_uuid(*loaded->storages[upd.storage_idx],
                                 loaded->pv_labels[upd.storage_idx],
                                 upd.new_uuid, upd.orig_sector, err)) {
        std::cerr << "error: " << err << "\n";
        for (const auto& prev : uuid_updates) {
          if (!prev.orig_sector.empty()) {
            std::string rb_err;
            loaded->storages[prev.storage_idx]->write(
                prev.label_offset, prev.orig_sector.data(), prev.orig_sector.size(), rb_err);
          }
        }
        return 1;
      }
    }

    if (!commit_vg_metadata(loaded->vg, loaded->raw_storages(), err)) {
      for (const auto& prev : uuid_updates) {
        if (!prev.orig_sector.empty()) {
          std::string rb_err;
          loaded->storages[prev.storage_idx]->write(
              prev.label_offset, prev.orig_sector.data(), prev.orig_sector.size(), rb_err);
        }
      }
      std::cerr << "error: failed to commit pvchange metadata: " << err << "\n";
      return 1;
    }

#ifdef _WIN32
    driver_refresh();
#endif
  }

  // Process orphan PVs.
  for (const auto& opath : orphan_paths) {
    if (has_alloc || has_tags) {
      std::cerr << "error: PV \"" << opath
                << "\" does not belong to a volume group; -x and --addtag/--deltag require VG metadata\n";
      return 1;
    }
    auto src = open_storage_source(opath, true, err);
    if (!src) {
      std::cerr << "error: cannot open " << opath << ": " << err << "\n";
      return 1;
    }
    auto lbl = read_pv_label(*src, err);
    if (!lbl) {
      std::cerr << "error: cannot read PV label on " << opath << ": " << err << "\n";
      return 1;
    }
    std::vector<std::uint8_t> orig;
    const std::string fresh_uuid = generate_random_lvm_uuid();
    if (!pv_rewrite_label_uuid(*src, *lbl, fresh_uuid, orig, err)) {
      std::cerr << "error: " << err << "\n";
      return 1;
    }
    changed_count++;
  }

  std::cout << "  " << changed_count << " physical volume(s) changed\n";
  return 0;
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
