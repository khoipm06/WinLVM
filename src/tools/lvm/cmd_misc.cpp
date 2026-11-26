#include "subcommands.h"
#include "cmd_common.h"
#include "report_json.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include "win32_driver.h"
#else
#include <unistd.h>
#endif

namespace lvmfmt {

namespace {

struct ConfigEntry {
  std::string key;
  std::string value;     // LVM config formatted value (quoted string, int, or array)
  std::string raw_json;  // unquoted string for JSON output
  std::string comment;
};

struct ConfigSection {
  std::string name;
  std::string comment;
  std::vector<ConfigEntry> entries;
};

const std::vector<ConfigSection>& default_lvm_config() {
  static const std::vector<ConfigSection> sections = {
      {
          "config",
          "How LVM configuration settings are handled.",
          {
              {"checks", "1", "1", "If enabled, any LVM configuration mismatch is reported."},
              {"abort_on_errors", "0", "0", "Abort the LVM process if a configuration error is encountered."},
          },
      },
      {
          "devices",
          "How LVM uses block devices.",
          {
              {"dir", "\"/dev\"", "/dev", "Directory in which to create volume group device nodes."},
              {"scan", "[ \"/dev\" ]", "/dev", "An array of directories that contain the device nodes you wish to use with LVM."},
              {"obtain_device_list_from_udev", "0", "0", "Obtain the list of available devices from udev."},
              {"external_device_info_source", "\"none\"", "none", "Select an external device information source."},
              {"write_cache_state", "0", "0", "Enable/disable writing the cache file."},
              {"sysfs_scan", "0", "0", "Restrict device scanning to block devices appearing in sysfs."},
              {"md_component_detection", "1", "1", "Ignore devices that are components of software RAID (md) devices."},
              {"fw_raid_component_detection", "0", "0", "Ignore devices that are components of firmware RAID devices."},
              {"md_chunk_alignment", "1", "1", "Align the start of a PV data area with md device's stripe-width."},
              {"data_alignment_detection", "1", "1", "Detect data alignment from device topology."},
              {"data_alignment", "0", "0", "Default alignment of the start of a PV data area in KiB."},
              {"data_alignment_offset_detection", "1", "1", "Detect data alignment offset from device topology."},
              {"ignore_suspended_devices", "0", "0", "Ignore DM devices that have I/O suspended while scanning devices."},
              {"ignore_lvm_mirrors", "1", "1", "Do not scan 'mirror' LVs to avoid possible deadlocks."},
              {"disable_after_error_count", "0", "0", "Number of I/O errors before disabling a device."},
              {"require_restorefile_with_uuid", "1", "1", "Allow use of pvcreate --uuid without requiring --restorefile."},
              {"pv_min_size", "2048", "2048", "Minimum size in KiB of block devices which can be used as PVs."},
              {"issue_discards", "0", "0", "Issue discards to PVs that are no longer used by an LV."},
              {"allow_changes_with_duplicate_pvs", "0", "0", "Allow VG modification while a PV appears on multiple devices."},
          },
      },
      {
          "allocation",
          "How LVM selects space and applies properties to LVs.",
          {
              {"maximise_cling", "1", "1", "Use a previous allocation algorithm."},
              {"use_blkid_wiping", "0", "0", "Use blkid to detect existing signatures on new PVs and LVs."},
              {"wipe_signatures_when_zeroing_new_lvs", "1", "1", "Look for and erase any signatures while zeroing a new LV."},
              {"mirror_logs_require_separate_pvs", "0", "0", "Mirror logs and images will always use different PVs."},
              {"thin_pool_metadata_require_separate_pvs", "0", "0", "Thin pool metadata and data will always use different PVs."},
              {"thin_pool_zero", "1", "1", "Thin pool data chunks are zeroed before they are first used."},
              {"thin_pool_discards", "\"passdown\"", "passdown", "The discards behaviour of thin pools."},
              {"thin_pool_chunk_size_policy", "\"generic\"", "generic", "The chunk size calculation policy for thin pool logical volumes."},
              {"thin_pool_chunk_size", "64", "64", "The minimal chunk size in KiB for thin pool volumes."},
          },
      },
      {
          "log",
          "How LVM log information is reported.",
          {
              {"verbose", "0", "0", "Controls the messages sent to stdout or stderr."},
              {"silent", "0", "0", "Suppress all non-essential messages from stdout."},
              {"syslog", "0", "0", "Send log messages through syslog."},
              {"overwrite", "0", "0", "Overwrite the log file each time the program is run."},
              {"level", "0", "0", "The level of log messages that are sent to the log file or syslog."},
              {"indent", "1", "1", "Indent messages according to their severity."},
              {"command_names", "0", "0", "Display the command name on each line of output."},
              {"prefix", "\"  \"", "  ", "A prefix to use before the log message text."},
              {"activation", "0", "0", "Log messages during activation."},
              {"debug_classes", "[ \"memory\", \"devices\", \"io\", \"activation\", \"allocation\", \"metadata\", \"cache\", \"locking\", \"lvmpolld\", \"dbus\" ]",
               "memory,devices,io,activation,allocation,metadata,cache,locking,lvmpolld,dbus",
               "Select log messages by class."},
          },
      },
      {
          "backup",
          "How LVM metadata is backed up and archived.",
          {
              {"backup", "1", "1", "Maintain a backup of the current metadata configuration."},
              {"backup_dir", "\"/etc/lvm/backup\"", "/etc/lvm/backup", "Location of the metadata backup files."},
              {"archive", "1", "1", "Maintain an archive of old metadata configurations."},
              {"archive_dir", "\"/etc/lvm/archive\"", "/etc/lvm/archive", "Location of the metadata archive files."},
              {"retain_min", "10", "10", "Minimum number of archives to keep."},
              {"retain_days", "30", "30", "Minimum number of days to keep archive files."},
          },
      },
      {
          "global",
          "Miscellaneous global LVM settings.",
          {
              {"umask", "077", "077", "The file creation mask for any files and directories created."},
              {"test", "0", "0", "No on-disk metadata changes will be made in test mode."},
              {"units", "\"r\"", "r", "Default value for --units argument."},
              {"si_unit_consistency", "1", "1", "Distinguish between powers of 1024 and 1000 bytes."},
              {"suffix", "1", "1", "Display unit suffix for sizes."},
              {"activation", "1", "1", "Enable/disable communication with the device-mapper driver."},
              {"proc", "\"/proc\"", "/proc", "Location of proc filesystem."},
              {"etc", "\"/etc\"", "/etc", "Location of /etc system configuration directory."},
              {"locking_type", "1", "1", "Type of locking to use."},
              {"wait_for_locks", "1", "1", "When disabled, fail if a lock request would block."},
              {"fallback_to_clustered_locking", "1", "1", "Attempt to use built-in cluster locking if external locking fails."},
              {"fallback_to_local_locking", "1", "1", "Use locking_type 1 (local) if locking_type 2 or 3 fail."},
              {"locking_dir", "\"/run/lock/lvm\"", "/run/lock/lvm", "Directory to use for LVM command file locks."},
              {"prioritise_write_locks", "1", "1", "Allow quicker VG write access during high volume read access."},
              {"abort_on_errors", "0", "0", "Abort the LVM process if a configuration error is encountered."},
              {"metadata_read_only", "0", "0", "No operations that change on-disk metadata are permitted."},
              {"mirror_segtype_default", "\"raid1\"", "raid1", "The segment type used by the short mirroring option -m."},
              {"raid10_segtype_default", "\"raid10\"", "raid10", "The segment type used by the -i -m combination."},
              {"sparse_segtype_default", "\"thin\"", "thin", "The default segment type used for sparse volumes."},
              {"use_lvmetad", "0", "0", "Use lvmetad to cache metadata and reduce disk scanning."},
              {"use_lvmpolld", "0", "0", "Use lvmpolld to supervise long running LVM commands."},
              {"system_id_source", "\"none\"", "none", "The method LVM uses to set the local system ID."},
          },
      },
      {
          "activation",
          "How LVM activates logical volumes.",
          {
              {"checks", "0", "0", "Perform internal checks of libdevmapper operations."},
              {"udev_sync", "0", "0", "Use udev notifications to synchronize udev and LVM."},
              {"udev_rules", "0", "0", "Use udev rules to manage LV device nodes and symlinks."},
              {"verify_udev_operations", "0", "0", "Use extra checks in LVM to verify udev operations."},
              {"retry_deactivation", "1", "1", "Retry failed LV deactivation."},
              {"missing_stripe_filler", "\"error\"", "error", "Method to fill missing stripes when activating an incomplete LV."},
              {"use_linear_target", "1", "1", "Use the linear target to optimize single stripe LVs."},
              {"reserved_stack", "64", "64", "Stack size in KiB to reserve for use while devices are suspended."},
              {"reserved_memory", "8192", "8192", "Memory size in KiB to reserve for use while devices are suspended."},
              {"process_priority", "-18", "-18", "The nice value used while devices are suspended."},
              {"raid_region_size", "2048", "2048", "Size in KiB of each raid or mirror synchronization region."},
              {"raid_fault_policy", "\"warn\"", "warn", "Defines how a device failure in a RAID LV is handled."},
              {"mirror_image_fault_policy", "\"remove\"", "remove", "Defines how a device failure in a 'mirror' LV is handled."},
              {"mirror_log_fault_policy", "\"allocate\"", "allocate", "Defines how a device failure in a 'mirror' log is handled."},
              {"snapshot_autoextend_threshold", "100", "100", "Auto-extend a snapshot when its usage exceeds this percent."},
              {"snapshot_autoextend_percent", "20", "20", "Auto-extending a snapshot adds this percent extra space."},
              {"thin_pool_autoextend_threshold", "100", "100", "Auto-extend a thin pool when its usage exceeds this percent."},
              {"thin_pool_autoextend_percent", "20", "20", "Auto-extending a thin pool adds this percent extra space."},
              {"auto_activation_volume_list", "[]", "", "Only LVs matching this list are auto-activated."},
              {"read_only_volume_list", "[]", "", "LVs matching this list are activated read-only."},
          },
      },
      {
          "report",
          "LVM reporting command settings.",
          {
              {"output_format", "\"basic\"", "basic", "Format of LVM command's report output."},
              {"compact_output", "0", "0", "Do not print empty report fields."},
              {"aligned", "1", "1", "Align columns in report output."},
              {"buffered", "1", "1", "Buffer report output."},
              {"headings", "1", "1", "Show headings for columns on report."},
              {"separator", "\" \"", " ", "A separator to use on report after each field."},
              {"list_item_separator", "\",\"", ",", "A separator to use for list items when reported."},
              {"prefixes", "0", "0", "Use a field name prefix for each field reported."},
              {"quoted", "1", "1", "Quote field values when using field name prefixes."},
          },
      },
  };
  return sections;
}

int run_config_command(const std::vector<std::string>& args, std::string_view cmd_name) {
  if (check_help_requested(args, cmd_name)) return 0;

  ReportFormat fmt = ReportFormat::Default;
  std::string err;
  if (!parse_report_format(args, fmt, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  std::string cfg_type = "current";
  if (auto t = get_opt_value(args, "", "--type")) {
    cfg_type = *t;
    if (cfg_type != "default" && cfg_type != "current" && cfg_type != "full" &&
        cfg_type != "new" && cfg_type != "diff" && cfg_type != "missing" &&
        cfg_type != "list") {
      std::cerr << "error: unsupported --type value \"" << cfg_type
                << "\" (expected default, current, or full)\n";
      return 1;
    }
  }

  const bool with_comments = has_flag(args, "", "--withcomments");
  const bool with_versions = has_flag(args, "", "--withversions");
  const auto out_file = get_opt_value(args, "-f", "--file");

  const std::set<std::string> val_flags = {"--type", "--file", "-f", "--reportformat"};
  const auto queries = get_positional_args_for(args, val_flags);

  const auto& all_sections = default_lvm_config();

  struct ResolvedSection {
    const ConfigSection* section = nullptr;
    std::vector<const ConfigEntry*> entries;
    bool single_setting_query = false;
  };
  std::vector<ResolvedSection> resolved;

  if (queries.empty()) {
    for (const auto& sec : all_sections) {
      ResolvedSection rs;
      rs.section = &sec;
      for (const auto& e : sec.entries) rs.entries.push_back(&e);
      resolved.push_back(std::move(rs));
    }
  } else {
    for (const auto& q : queries) {
      const auto slash = q.find('/');
      const std::string sec_name = (slash == std::string::npos) ? q : q.substr(0, slash);
      const std::string key_name = (slash == std::string::npos) ? "" : q.substr(slash + 1);

      const ConfigSection* found_sec = nullptr;
      for (const auto& sec : all_sections) {
        if (sec.name == sec_name) {
          found_sec = &sec;
          break;
        }
      }
      if (!found_sec) {
        std::cerr << "error: Configuration node \"" << q << "\" not found\n";
        return 1;
      }

      ResolvedSection rs;
      rs.section = found_sec;
      if (key_name.empty()) {
        for (const auto& e : found_sec->entries) rs.entries.push_back(&e);
      } else {
        const ConfigEntry* found_entry = nullptr;
        for (const auto& e : found_sec->entries) {
          if (e.key == key_name) {
            found_entry = &e;
            break;
          }
        }
        if (!found_entry) {
          std::cerr << "error: Configuration node \"" << q << "\" not found\n";
          return 1;
        }
        rs.entries.push_back(found_entry);
        rs.single_setting_query = true;
      }
      resolved.push_back(std::move(rs));
    }
  }

  std::ostringstream out;
  if (fmt == ReportFormat::Json || fmt == ReportFormat::JsonStd) {
    out << "{\n";
    for (std::size_t si = 0; si < resolved.size(); ++si) {
      const auto& rs = resolved[si];
      out << "  \"" << json_escape(rs.section->name) << "\": {\n";
      for (std::size_t ei = 0; ei < rs.entries.size(); ++ei) {
        const auto* e = rs.entries[ei];
        out << "    \"" << json_escape(e->key) << "\": " << json_string_value(e->raw_json);
        if (ei + 1 < rs.entries.size()) out << ",";
        out << "\n";
      }
      out << "  }";
      if (si + 1 < resolved.size()) out << ",";
      out << "\n";
    }
    out << "}\n";
  } else if (cfg_type == "list") {
    for (const auto& rs : resolved) {
      for (const auto* e : rs.entries) {
        out << rs.section->name << "/" << e->key << "\n";
      }
    }
  } else {
    for (const auto& rs : resolved) {
      if (rs.single_setting_query) {
        const auto* e = rs.entries[0];
        if (with_comments) {
          out << "# Configuration option " << rs.section->name << "/" << e->key << ".\n";
          out << "# " << e->comment << "\n";
        }
        if (with_versions) {
          out << "# This configuration option is available since LVM2 2.03.22.\n";
        }
        out << e->key << "=" << e->value << "\n";
      } else {
        if (with_comments) {
          out << "# Configuration section " << rs.section->name << ".\n";
          out << "# " << rs.section->comment << "\n";
        }
        if (with_versions) {
          out << "# This configuration section is available since LVM2 2.03.22.\n";
        }
        out << rs.section->name << " {\n";
        for (const auto* e : rs.entries) {
          if (with_comments) {
            out << "\t# " << e->comment << "\n";
          }
          if (with_versions) {
            out << "\t# Available since LVM2 2.03.22.\n";
          }
          out << "\t" << e->key << "=" << e->value << "\n";
        }
        out << "}\n";
      }
    }
  }

  if (out_file) {
    std::ofstream f(*out_file, std::ios::binary | std::ios::trunc);
    if (!f) {
      std::cerr << "error: cannot open output file \"" << *out_file << "\"\n";
      return 1;
    }
    f << out.str();
    f.close();
    if (!f) {
      std::cerr << "error: failed to write configuration to \"" << *out_file << "\"\n";
      return 1;
    }
  } else {
    std::cout << out.str();
  }
  return 0;
}

}  // namespace

int cmd_dumpconfig(const std::vector<std::string>& args) {
  return run_config_command(args, "dumpconfig");
}

int cmd_lvmconfig(const std::vector<std::string>& args) {
  return run_config_command(args, "lvmconfig");
}

int cmd_fullreport(const std::vector<std::string>& args) {
  if (check_help_requested(args, "fullreport")) return 0;

  ReportFormat fmt = ReportFormat::Default;
  std::string err;
  if (!parse_report_format(args, fmt, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  const bool noheadings = has_flag(args, "--noheadings");
  const std::set<std::string> val_flags = {"--reportformat", "-o", "--options", "--separator"};
  auto raw_pos = get_positional_args_for(args, val_flags);

  std::vector<std::string> pos;
  std::set<std::string> tag_filters;
  if (!split_tag_filters(raw_pos, pos, tag_filters, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  bool auto_scan = false;
  std::string vg_name_filter;
#ifdef _WIN32
  std::set<std::string> active_uuids;
  {
    std::vector<WINLVM_INVENTORY_LV_ENTRY> entries;
    std::uint32_t records = 0;
    if (driver_query_inventory(entries, records)) {
      for (const auto& e : entries) {
        if ((e.flags & WINLVM_INV_LV_ACTIVE) != 0) {
          active_uuids.insert(
              normalize_uuid_raw32(std::string(e.lv_uuid, strnlen(e.lv_uuid, sizeof(e.lv_uuid)))));
        }
      }
    }
  }
  if (pos.empty()) {
    auto_scan = true;
    for (const auto& c : discover_win32_candidate_devices()) pos.push_back(c.open_path);
  }
#else
  std::set<std::string> active_uuids;
  if (pos.empty()) {
    std::cerr << "error: fullreport requires physical volume or image path(s) in offline mode\n";
    return 1;
  }
#endif

  // If pos[0] is a VG name rather than a readable device path, filter by that VG.
  std::vector<std::string> scan_paths = pos;
  if (!auto_scan && !pos.empty()) {
    std::string probe_err;
    if (!open_storage_source(pos[0], false, probe_err)) {
      vg_name_filter = pos[0];
      if (pos.size() >= 2) {
        scan_paths.assign(pos.begin() + 1, pos.end());
      } else {
#ifdef _WIN32
        scan_paths = discover_win32_pv_paths_for_vg(vg_name_filter);
#else
        std::cerr << "error: cannot open " << pos[0] << ": " << probe_err << "\n";
        return 1;
#endif
      }
    }
  }

  std::map<std::string, VolumeGroup> vgs_by_uuid;
  struct OpenedPv {
    std::string path;
    PvLabel label;
  };
  std::vector<OpenedPv> opened_pvs;
  std::set<std::string> seen_pv_uuids;

  for (const auto& dev : scan_paths) {
    std::string open_err;
    auto src = open_storage_source(dev, false, open_err);
    if (!src) {
      if (!auto_scan) {
        std::cerr << "error: cannot open " << dev << ": " << open_err << "\n";
        return 1;
      }
      continue;
    }
    auto label = read_pv_label(*src, open_err);
    if (!label) {
      if (!auto_scan) {
        std::cerr << "error: cannot read PV label from " << dev << ": " << open_err << "\n";
        return 1;
      }
      continue;
    }
    const std::string raw_pv = normalize_uuid_raw32(label->pv_uuid);
    if (!seen_pv_uuids.insert(raw_pv).second) continue;

    std::string meta_err;
    if (auto meta = read_vg_metadata(*src, *label, meta_err)) {
      if (auto vg = parse_vg_metadata(*meta, meta_err)) {
        const std::string vg_key = normalize_uuid_raw32(vg->id);
        auto it = vgs_by_uuid.find(vg_key);
        if (it == vgs_by_uuid.end() || vg->seqno > it->second.seqno) {
          vgs_by_uuid[vg_key] = std::move(*vg);
        }
      }
    }
    opened_pvs.push_back({dev, std::move(*label)});
  }

  std::vector<VgReportRow> vg_rows;
  std::vector<PvReportRow> pv_rows;
  std::vector<LvReportRow> lv_rows;

  for (const auto& kv : vgs_by_uuid) {
    const auto& vg = kv.second;
    if (!vg_name_filter.empty() && vg.name != vg_name_filter) continue;
    if (!matches_tag_filters(vg.tags, tag_filters)) continue;
    vg_rows.push_back(build_vg_report_row(vg));
  }

  for (const auto& opv : opened_pvs) {
    PvReportRow row;
    row.pv_name = opv.path;
    row.pv_fmt = "lvm2";
    row.pv_attr = "---";
    row.pv_size = format_size_human(opv.label.device_size);
    row.pv_free = row.pv_size;
    row.pv_uuid = format_uuid(opv.label.pv_uuid);

    const VolumeGroup* owner_vg = nullptr;
    for (const auto& kv : vgs_by_uuid) {
      if (kv.second.find_pv_by_uuid(opv.label.pv_uuid)) {
        owner_vg = &kv.second;
        break;
      }
    }
    if (owner_vg) {
      if (!vg_name_filter.empty() && owner_vg->name != vg_name_filter) continue;
      row.vg_name = owner_vg->name;
      if (const PhysicalVolume* pve = owner_vg->find_pv_by_uuid(opv.label.pv_uuid)) {
        row.pv_attr[0] = pve->is_allocatable() ? 'a' : '-';
        row.pv_attr[1] = owner_vg->is_exported() ? 'x' : '-';
        row.pv_attr[2] = pve->is_missing() ? 'm' : '-';
        row.pv_tags = pve->tags;
      }
      auto maps = build_vg_extent_maps(*owner_vg);
      const std::string raw_uuid = normalize_uuid_raw32(opv.label.pv_uuid);
      for (const auto& m : maps) {
        if (normalize_uuid_raw32(m.pv_uuid) == raw_uuid) {
          row.pv_free = format_size_human(m.free_extents * owner_vg->extent_size_sectors * 512ULL);
          break;
        }
      }
    } else if (!vg_name_filter.empty()) {
      continue;
    }

    if (!matches_tag_filters(row.pv_tags, tag_filters)) continue;
    pv_rows.push_back(std::move(row));
  }

  for (const auto& kv : vgs_by_uuid) {
    const auto& vg = kv.second;
    if (!vg_name_filter.empty() && vg.name != vg_name_filter) continue;
    for (auto& r : build_lv_report_rows(vg, active_uuids)) {
      if (!matches_tag_filters(r.lv_tags, tag_filters)) continue;
      lv_rows.push_back(std::move(r));
    }
  }

  if (fmt == ReportFormat::Json || fmt == ReportFormat::JsonStd) {
    print_fullreport_json(vg_rows, pv_rows, lv_rows, fmt);
    return 0;
  }

  if (!noheadings) {
    std::cout << "  VG         #PV #LV #SN Attr   VSize    VFree\n";
  }
  for (const auto& r : vg_rows) {
    std::cout << "  " << r.vg_name << "  " << r.pv_count << "   " << r.lv_count
              << "   " << r.snap_count << " " << r.vg_attr << " "
              << r.vg_size << " " << r.vg_free << "\n";
  }
  if (!noheadings) {
    std::cout << "  PV         VG         Fmt  Attr PSize   PFree   PV UUID\n";
  }
  for (const auto& r : pv_rows) {
    std::cout << "  " << r.pv_name << "  " << (r.vg_name.empty() ? "[orphan]" : r.vg_name)
              << "  " << r.pv_fmt << " " << r.pv_attr << "  " << r.pv_size
              << "  " << r.pv_free << "  " << r.pv_uuid << "\n";
  }
  if (!noheadings) {
    std::cout << "  LV   VG   Attr       LSize\n";
  }
  for (const auto& r : lv_rows) {
    std::cout << "  " << r.lv_name << " " << r.vg_name << " "
              << r.lv_attr << " " << r.lv_size << "\n";
  }
  return 0;
}

int cmd_formats(const std::vector<std::string>& args) {
  if (check_help_requested(args, "formats")) return 0;
  std::cout << "  lvm2  LVM2 format (circular ring MDA)\n";
  return 0;
}

int cmd_segtypes(const std::vector<std::string>& args) {
  if (check_help_requested(args, "segtypes")) return 0;
  std::cout << "  linear     Linear mapping segment type\n"
            << "  striped    Striped mapping segment type\n"
            << "  thin-pool  Thin provisioning pool segment type\n"
            << "  thin       Thinly provisioned logical volume segment type\n"
            << "  snapshot   Copy-on-write snapshot segment type\n"
            << "  raid1      Two-leg RAID1 mirrored segment type\n"
            << "  mirror     Two-leg core-log mirror segment type\n";
  return 0;
}

int cmd_tags(const std::vector<std::string>& args) {
  if (check_help_requested(args, "tags")) return 0;

  ReportFormat fmt = ReportFormat::Default;
  std::string err;
  if (!parse_report_format(args, fmt, err)) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }

  const std::set<std::string> val_flags = {"--reportformat"};
  auto pos = get_positional_args_for(args, val_flags);
  bool auto_scan = false;

#ifdef _WIN32
  if (pos.empty()) {
    auto_scan = true;
    for (const auto& c : discover_win32_candidate_devices()) pos.push_back(c.open_path);
  }
#endif

  std::map<std::string, VolumeGroup> vgs_by_uuid;
  for (const auto& dev : pos) {
    std::string open_err;
    auto src = open_storage_source(dev, false, open_err);
    if (!src) {
      if (!auto_scan) {
        std::cerr << "error: cannot open " << dev << ": " << open_err << "\n";
        return 1;
      }
      continue;
    }
    auto label = read_pv_label(*src, open_err);
    if (!label) continue;
    auto meta = read_vg_metadata(*src, *label, open_err);
    if (!meta) continue;
    auto vg = parse_vg_metadata(*meta, open_err);
    if (!vg) continue;
    const std::string key = normalize_uuid_raw32(vg->id);
    auto it = vgs_by_uuid.find(key);
    if (it == vgs_by_uuid.end() || vg->seqno > it->second.seqno) {
      vgs_by_uuid[key] = std::move(*vg);
    }
  }

  std::set<std::string> unique_tags;
  for (const auto& kv : vgs_by_uuid) {
    const auto& vg = kv.second;
    for (const auto& t : vg.tags) unique_tags.insert(t);
    for (const auto& pv : vg.pvs) {
      for (const auto& t : pv.tags) unique_tags.insert(t);
    }
    for (const auto& lv : vg.lvs) {
      for (const auto& t : lv.tags) unique_tags.insert(t);
    }
  }

  if (fmt == ReportFormat::Json || fmt == ReportFormat::JsonStd) {
    std::cout << "{\n  \"tags\": [";
    std::size_t idx = 0;
    for (const auto& t : unique_tags) {
      if (idx++ > 0) std::cout << ", ";
      std::cout << json_string_value(t);
    }
    std::cout << "]\n}\n";
    return 0;
  }

  for (const auto& t : unique_tags) {
    std::cout << "  " << t << "\n";
  }
  return 0;
}

int cmd_systemid(const std::vector<std::string>& args) {
  if (check_help_requested(args, "systemid")) return 0;
  std::string host;
#if defined(_WIN32)
  char buf[256]{};
  DWORD sz = sizeof(buf);
  if (GetComputerNameA(buf, &sz)) host.assign(buf, sz);
#else
  char buf[256]{};
  if (gethostname(buf, sizeof(buf) - 1) == 0) host.assign(buf);
#endif
  std::cout << "  system ID: " << host << "\n";
  return 0;
}

int cmd_lvmdiskscan(const std::vector<std::string>& args) {
  if (check_help_requested(args, "lvmdiskscan")) return 0;

  auto pos = get_positional_args(args);
#if defined(_WIN32)
  if (pos.empty()) {
    for (const auto& c : discover_win32_candidate_devices()) {
      pos.push_back(c.open_path);
    }
  }
#else
  if (pos.empty()) {
    std::cerr << "error: lvmdiskscan requires device or image path(s) in offline mode\n";
    return 1;
  }
#endif

  std::uint64_t disks = 0;
  std::uint64_t lvm_pvs = 0;
  for (const auto& dev : pos) {
    std::string err;
    auto storage = open_storage_source(dev, false, err);
    if (!storage) continue;
    disks++;

    auto label = read_pv_label(*storage, err);
    if (label) {
      lvm_pvs++;
      std::cout << "  " << dev << " [ " << format_size_human(label->device_size)
                << "] LVM physical volume\n";
    } else {
      std::cout << "  " << dev << " [ " << format_size_human(storage->size()) << "] disk\n";
    }
  }
  std::cout << "  " << disks << " disks\n"
            << "  " << lvm_pvs << " LVM physical volumes\n";
  return 0;
}

int cmd_lvmdump(const std::vector<std::string>& args) {
  if (check_help_requested(args, "lvmdump")) return 0;
  std::cerr << "error: lvmdump archive subcommand is not supported; use the standalone lvmdump tool\n";
  return 1;
}

int cmd_help(const std::vector<std::string>& args) {
  if (check_help_requested(args, "help")) return 0;

  auto pos = get_positional_args(args);
  if (!pos.empty()) {
    if (print_command_help(pos[0])) return 0;
    std::cerr << "Unknown command: " << pos[0] << "\n";
    return 1;
  }

  std::cout << "Available LVM2 commands:\n";
  const auto& cmds = get_all_commands();
  for (const auto& cmd : cmds) {
    std::cout << "  " << std::left << std::setw(16) << cmd.name << " " << cmd.summary << "\n";
  }
  std::cout << "\nUse 'lvm help <command>' or 'lvm <command> --help' for details.\n";
  return 0;
}

int cmd_version(const std::vector<std::string>& args) {
  if (check_help_requested(args, "version")) return 0;

  std::cout << "WinLVM CLI version 1.0 (Windows LVM2 Subsystem)\n"
            << "LVM2 format compatible: version 2.03.22 (library 1.02.196)\n";

#if defined(_WIN32)
  HANDLE h = open_winlvm_control();
  if (h != INVALID_HANDLE_VALUE) {
    std::cout << "Driver status: online (\\Device\\WinLvmControl connected)\n";
    CloseHandle(h);
  } else {
    std::cout << "Driver status: offline / service stopped (error " << GetLastError() << ")\n";
  }
#else
  std::cout << "Driver status: host user-mode simulation\n";
#endif

  return 0;
}

}  // namespace lvmfmt
