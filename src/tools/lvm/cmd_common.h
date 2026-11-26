#pragma once

#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cmd_table.h"
#include "win32_driver.h"
#include "lvmfmt/allocator.h"
#include "lvmfmt/commit.h"
#include "lvmfmt/label.h"
#include "lvmfmt/metadata.h"
#include "lvmfmt/source.h"

namespace lvmfmt {

inline bool print_command_help(std::string_view cmd_name) {
  const auto& cmds = get_all_commands();
  for (const auto& cmd : cmds) {
    if (cmd.name == cmd_name) {
      std::cout << "Usage:\n  " << cmd.syntax << "\n\n";
      std::cout << cmd.description << "\n\n";
      if (!cmd.options.empty()) {
        std::cout << "Options:\n";
        for (const auto& opt : cmd.options) {
          std::cout << "  " << opt << "\n";
        }
        std::cout << "\n";
      }
      return true;
    }
  }
  return false;
}

inline bool has_flag(const std::vector<std::string>& args, const std::string& opt,
                     const std::string& long_opt = "") {
  for (const auto& a : args) {
    if (a == opt || (!long_opt.empty() && a == long_opt)) return true;
    if (a == "--help" || a == "-h") return false;
  }
  return false;
}

inline bool check_help_requested(const std::vector<std::string>& args, std::string_view cmd_name) {
  for (const auto& a : args) {
    if (a == "--help" || a == "-h") {
      print_command_help(cmd_name);
      return true;
    }
  }
  return false;
}

inline std::optional<std::string> get_opt_value(const std::vector<std::string>& args,
                                                const std::string& opt,
                                                const std::string& long_opt = "") {
  for (std::size_t i = 0; i < args.size(); ++i) {
    if ((!opt.empty() && args[i] == opt) || (!long_opt.empty() && args[i] == long_opt)) {
      if (i + 1 < args.size()) return args[i + 1];
    }
    if (!long_opt.empty() && args[i].rfind(long_opt + "=", 0) == 0) {
      return args[i].substr(long_opt.size() + 1);
    }
    if ((opt == "-L" || opt == "-l" || opt == "-V" || opt == "-m" || opt == "-x" || opt == "-c") &&
        args[i].size() > 2 && args[i].rfind(opt, 0) == 0) {
      return args[i].substr(2);
    }
  }
  return std::nullopt;
}

inline std::vector<std::string> get_opt_values(const std::vector<std::string>& args,
                                               const std::string& opt,
                                               const std::string& long_opt = "") {
  std::vector<std::string> values;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if ((!opt.empty() && args[i] == opt) || (!long_opt.empty() && args[i] == long_opt)) {
      if (i + 1 < args.size()) {
        values.push_back(args[i + 1]);
        ++i;
      }
    } else if (!long_opt.empty() && args[i].rfind(long_opt + "=", 0) == 0) {
      values.push_back(args[i].substr(long_opt.size() + 1));
    }
  }
  return values;
}

inline std::string generate_random_lvm_uuid() {
  static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<std::size_t> dis(0, sizeof(chars) - 2);
  std::string s;
  s.reserve(32);
  for (int i = 0; i < 32; ++i) s.push_back(chars[dis(gen)]);
  return s;
}

inline bool validate_lvm_tag(std::string_view tag, std::string& err) {
  if (tag.empty()) {
    err = "tag cannot be empty";
    return false;
  }
  if (tag.size() > 128) {
    err = "tag exceeds maximum length of 128 characters";
    return false;
  }
  for (char c : tag) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '.' || c == '-';
    if (!ok) {
      err = "invalid tag character '" + std::string(1, c) + "' in tag \"" + std::string(tag) + "\"";
      return false;
    }
  }
  return true;
}

inline bool parse_tag_mutations(const std::vector<std::string>& args,
                                std::vector<std::string>& add_tags,
                                std::vector<std::string>& del_tags,
                                std::string& error) {
  add_tags.clear();
  del_tags.clear();
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--addtag" || args[i] == "--deltag") {
      if (i + 1 >= args.size()) {
        error = args[i] + " requires a tag argument";
        return false;
      }
      const std::string& val = args[i + 1];
      if (!validate_lvm_tag(val, error)) return false;
      if (args[i] == "--addtag") add_tags.push_back(val);
      else del_tags.push_back(val);
      ++i;
    } else if (args[i].rfind("--addtag=", 0) == 0) {
      const std::string val = args[i].substr(9);
      if (!validate_lvm_tag(val, error)) return false;
      add_tags.push_back(val);
    } else if (args[i].rfind("--deltag=", 0) == 0) {
      const std::string val = args[i].substr(9);
      if (!validate_lvm_tag(val, error)) return false;
      del_tags.push_back(val);
    }
  }
  return true;
}

inline void apply_tag_mutations(std::vector<std::string>& tags,
                                const std::vector<std::string>& add_tags,
                                const std::vector<std::string>& del_tags) {
  for (const auto& t : del_tags) {
    tags.erase(std::remove(tags.begin(), tags.end(), t), tags.end());
  }
  for (const auto& t : add_tags) {
    if (std::find(tags.begin(), tags.end(), t) == tags.end()) {
      tags.push_back(t);
    }
  }
}

inline bool split_tag_filters(const std::vector<std::string>& pos,
                              std::vector<std::string>& non_tag_pos,
                              std::set<std::string>& tag_filters,
                              std::string& error) {
  non_tag_pos.clear();
  tag_filters.clear();
  for (const auto& p : pos) {
    if (!p.empty() && p[0] == '@') {
      const std::string tag = p.substr(1);
      if (!validate_lvm_tag(tag, error)) return false;
      tag_filters.insert(tag);
    } else {
      non_tag_pos.push_back(p);
    }
  }
  return true;
}

inline bool matches_tag_filters(const std::vector<std::string>& obj_tags,
                                const std::set<std::string>& tag_filters) {
  if (tag_filters.empty()) return true;
  for (const auto& t : obj_tags) {
    if (tag_filters.count(t)) return true;
  }
  return false;
}

inline std::optional<bool> parse_activate_flag(const std::vector<std::string>& args,
                                               bool& has_flag_out,
                                               std::string& error) {
  has_flag_out = false;
  auto val = get_opt_value(args, "-a", "--activate");
  if (!val) {
    for (const auto& a : args) {
      if (a == "-ay" || a == "-aey" || a == "-asy") {
        val = "y";
        break;
      }
      if (a == "-an" || a == "-aen" || a == "-asn") {
        val = "n";
        break;
      }
    }
  }
  if (!val) return std::nullopt;
  has_flag_out = true;
  if (*val == "y" || *val == "ay" || *val == "ey" || *val == "sy") return true;
  if (*val == "n" || *val == "an" || *val == "en" || *val == "sn") return false;
  error = "invalid activation mode: " + *val + " (expected y|n|ay|an)";
  return std::nullopt;
}

inline std::optional<bool> parse_permission_flag(const std::vector<std::string>& args,
                                                 bool& has_flag_out,
                                                 std::string& error) {
  has_flag_out = false;
  auto val = get_opt_value(args, "-p", "--permission");
  if (!val) {
    for (const auto& a : args) {
      if (a == "-pr") {
        val = "r";
        break;
      }
      if (a == "-prw") {
        val = "rw";
        break;
      }
    }
  }
  if (!val) return std::nullopt;
  has_flag_out = true;
  if (*val == "r") return false;  // read-only
  if (*val == "rw") return true;  // read-write
  error = "invalid permission mode: " + *val + " (expected r or rw)";
  return std::nullopt;
}

inline std::string strip_lv_path(std::string_view path) {
  const auto pos = path.find_last_of("/\\");
  if (pos != std::string_view::npos) {
    return std::string(path.substr(pos + 1));
  }
  return std::string(path);
}

inline std::pair<std::string, std::string> split_vg_lv_spec(std::string_view spec) {
  std::string_view s = spec;
  if (s.rfind("/dev/", 0) == 0) s.remove_prefix(5);
  const auto sep = s.find_last_of("/\\");
  if (sep != std::string_view::npos) {
    return {std::string(s.substr(0, sep)), std::string(s.substr(sep + 1))};
  }
  return {"", std::string(s)};
}

inline bool validate_lvm_name(std::string_view name, std::string_view kind, std::string& err) {
  if (name.empty()) {
    err = std::string(kind) + " name cannot be empty";
    return false;
  }
  if (name.size() > 128) {
    err = std::string(kind) + " name exceeds maximum length of 128 characters";
    return false;
  }
  if (name[0] == '-') {
    err = std::string(kind) + " name cannot begin with a hyphen";
    return false;
  }
  if (name == "." || name == "..") {
    err = std::string(kind) + " name cannot be '.' or '..'";
    return false;
  }
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-' || c == '.';
    if (!ok) {
      err = std::string(kind) + " name contains invalid character: '" + c + "'";
      return false;
    }
  }
  return true;
}

inline bool validate_lv_name(std::string_view name, std::string& err) {
  return validate_lvm_name(name, "logical volume", err);
}

inline std::vector<std::string> get_positional_args_for(
    const std::vector<std::string>& args,
    const std::set<std::string>& value_flags) {
  std::vector<std::string> pos;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--") {
      for (std::size_t j = i + 1; j < args.size(); ++j) pos.push_back(args[j]);
      break;
    }
    if (args[i].rfind("-", 0) == 0) {
      if (args[i].find('=') == std::string::npos && value_flags.count(args[i]) > 0) {
        if (i + 1 < args.size()) ++i;
      }
      continue;
    }
    pos.push_back(args[i]);
  }
  return pos;
}

inline std::vector<std::string> get_positional_args(const std::vector<std::string>& args,
                                                    bool f_takes_value = false) {
  std::vector<std::string> pos;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--") {
      for (std::size_t j = i + 1; j < args.size(); ++j) pos.push_back(args[j]);
      break;
    }
    if (args[i].rfind("-", 0) == 0) {
      if (args[i] == "-L" || args[i] == "--size" ||
          args[i] == "-l" || args[i] == "--extents" ||
          args[i] == "-n" || args[i] == "--name" ||
          args[i] == "-s" || args[i] == "--physicalextentsize" ||
          args[i] == "-u" || args[i] == "--uuid" ||
          args[i] == "-i" || args[i] == "--stripes" ||
          args[i] == "-I" || args[i] == "--stripesize" ||
          args[i] == "-o" || args[i] == "--options" ||
          args[i] == "-a" || args[i] == "--activate" ||
          args[i] == "-p" || args[i] == "--permission" ||
          args[i] == "--file" ||
          args[i] == "--setphysicalvolumesize" ||
          args[i] == "--metadatasize" ||
          args[i] == "--dataalignment" ||
          args[i] == "--separator" ||
          args[i] == "--reportformat" ||
          args[i] == "--addtag" ||
          args[i] == "--deltag" ||
          args[i] == "--type" ||
          args[i] == "--poolmetadatasize" ||
          args[i] == "--thinpool" ||
          args[i] == "--virtualsize" ||
          args[i] == "--chunksize" ||
          args[i] == "--mirrorlog" ||
          args[i] == "--allocatable" ||
          args[i] == "--mirrors" ||
          args[i] == "-V" ||
          args[i] == "-c" ||
          args[i] == "-m" ||
          args[i] == "-x" ||
          (f_takes_value && args[i] == "-f" && i + 1 < args.size() && args[i + 1][0] != '-')) {
        if (i + 1 < args.size()) {
          ++i;
        }
      }
      continue;
    }
    pos.push_back(args[i]);
  }
  return pos;
}

struct LoadedVG {
  VolumeGroup vg;
  std::vector<std::unique_ptr<ByteSource>> storages;
  std::vector<std::string> pv_paths;
  std::vector<PvLabel> pv_labels;

  std::vector<ByteSource*> raw_storages() const {
    std::vector<ByteSource*> out;
    out.reserve(storages.size());
    for (const auto& s : storages) out.push_back(s.get());
    return out;
  }

  void max_sector_geometry(std::uint32_t& logical, std::uint32_t& physical) const {
    logical = 512;
    physical = 512;
    for (const auto& s : storages) {
      if (!s) continue;
      if (s->logical_sector_size() > logical) logical = s->logical_sector_size();
      if (s->physical_sector_size() > physical) physical = s->physical_sector_size();
    }
    if (physical < logical) physical = logical;
  }
};

inline bool check_vg_mutable(const VolumeGroup& vg, std::string& error,
                             bool allow_exported = false, bool allow_readonly = false) {
  if (!allow_exported && vg.is_exported()) {
    error = "volume group \"" + vg.name + "\" is exported; run vgimport before modifying";
    return false;
  }
  if (!allow_readonly && vg.is_read_only()) {
    error = "volume group \"" + vg.name + "\" is read-only; run vgchange -p rw first";
    return false;
  }
  return true;
}

inline std::optional<LoadedVG> load_vg_from_pvs(const std::vector<std::string>& pv_paths,
                                                bool read_write,
                                                std::string& error,
                                                bool require_all_pvs = false) {
  if (pv_paths.empty()) {
    error = "no physical volume paths specified";
    return std::nullopt;
  }

  LoadedVG loaded;
  std::optional<VolumeGroup> best_vg;
  std::set<std::string> seen_uuids;

  auto open_and_merge = [&](const std::string& path) -> bool {
    std::string err;
    auto src = open_storage_source(path, read_write, err);
    if (!src) {
      error = "cannot open " + path + ": " + err;
      return false;
    }
    auto label = read_pv_label(*src, err);
    if (!label) {
      error = "cannot read PV label from " + path + ": " + err;
      return false;
    }

    const std::string raw_pv_uuid = normalize_uuid_raw32(label->pv_uuid);
    if (seen_uuids.count(raw_pv_uuid)) {
      return true;  // duplicate reference to the same PV
    }

    std::string meta_err;
    auto meta = read_vg_metadata(*src, *label, meta_err);
    if (meta) {
      std::string parse_err;
      auto parsed = parse_vg_metadata(*meta, parse_err);
      if (!parsed) {
        error = "cannot parse VG metadata from " + path + ": " + parse_err;
        return false;
      }
      if (best_vg) {
        if (normalize_uuid_raw32(parsed->id) != normalize_uuid_raw32(best_vg->id)) {
          error = "physical volume " + path + " belongs to VG \"" + parsed->name +
                  "\", which conflicts with VG \"" + best_vg->name + "\"";
          return false;
        }
        if (parsed->seqno > best_vg->seqno) {
          best_vg = std::move(*parsed);
        }
      } else {
        best_vg = std::move(*parsed);
      }
    }

    seen_uuids.insert(raw_pv_uuid);
    loaded.storages.push_back(std::move(src));
    loaded.pv_paths.push_back(path);
    loaded.pv_labels.push_back(std::move(*label));
    return true;
  };

  for (const auto& path : pv_paths) {
    if (!open_and_merge(path)) return std::nullopt;
  }

  if (!best_vg) {
    error = "failed to find valid volume group metadata on specified PV(s)";
    return std::nullopt;
  }

  // Verify every opened PV belongs to this VG.
  for (std::size_t i = 0; i < loaded.pv_labels.size(); ++i) {
    if (!best_vg->find_pv_by_uuid(loaded.pv_labels[i].pv_uuid)) {
      error = "physical volume " + loaded.pv_paths[i] + " is not a member of VG \"" +
              best_vg->name + "\"";
      return std::nullopt;
    }
  }

  if (require_all_pvs) {
    std::vector<std::string> missing_uuids;
    for (const auto& pv : best_vg->pvs) {
      if (pv.is_missing()) continue;
      const std::string raw = normalize_uuid_raw32(pv.id);
      if (!seen_uuids.count(raw)) missing_uuids.push_back(raw);
    }

#ifdef _WIN32
    if (!missing_uuids.empty()) {
      for (const auto& auto_path : discover_win32_pv_paths_by_uuids(missing_uuids)) {
        if (!open_and_merge(auto_path)) return std::nullopt;
      }
      missing_uuids.clear();
      for (const auto& pv : best_vg->pvs) {
        if (pv.is_missing()) continue;
        const std::string raw = normalize_uuid_raw32(pv.id);
        if (!seen_uuids.count(raw)) missing_uuids.push_back(raw);
      }
    }
#endif

    if (!missing_uuids.empty()) {
      error = "VG \"" + best_vg->name + "\" requires all " + std::to_string(best_vg->pvs.size()) +
              " member PVs for metadata commit; missing " + std::to_string(missing_uuids.size()) +
              " PV(s) (first missing UUID: " + format_uuid(missing_uuids.front()) + ")";
      return std::nullopt;
    }
  }

  loaded.vg = std::move(*best_vg);
  return loaded;
}

inline std::optional<LoadedVG> load_vg_flexible(const std::vector<std::string>& pos,
                                                bool read_write,
                                                std::string& error,
                                                bool require_all_pvs = false) {
  if (pos.empty()) {
    error = "no physical volume arguments provided";
    return std::nullopt;
  }

  // If pos.size() >= 2, check whether pos[0] is a VG/LV name rather than a PV path.
  std::string err1;
  auto loaded = load_vg_from_pvs(pos, read_write, err1, require_all_pvs);
  if (loaded) return loaded;

  if (pos.size() >= 2) {
    std::vector<std::string> pvs(pos.begin() + 1, pos.end());
    std::string err2;
    auto loaded2 = load_vg_from_pvs(pvs, read_write, err2, require_all_pvs);
    if (loaded2) {
      auto [vg_hint, rest] = split_vg_lv_spec(pos[0]);
      const std::string expected_vg = vg_hint.empty() ? pos[0] : vg_hint;
      if (loaded2->vg.name != expected_vg) {
        error = "volume group name mismatch: requested \"" + expected_vg +
                "\", but physical volume(s) belong to VG \"" + loaded2->vg.name + "\"";
        return std::nullopt;
      }
      return loaded2;
    }
    error = err2;
    return std::nullopt;
  }

#ifdef _WIN32
  // Single VGName or VGName/LVName on Windows: auto-discover member PVs.
  auto [vg_hint, lv_hint] = split_vg_lv_spec(pos[0]);
  const std::string target_vg = vg_hint.empty() ? pos[0] : vg_hint;
  auto auto_pvs = discover_win32_pv_paths_for_vg(target_vg);
  if (!auto_pvs.empty()) {
    return load_vg_from_pvs(auto_pvs, read_write, error, require_all_pvs);
  }
#endif

  error = err1;
  return std::nullopt;
}

inline void set_status_flag(std::vector<std::string>& status, const std::string& flag, bool present) {
  auto it = std::find(status.begin(), status.end(), flag);
  if (present) {
    if (it == status.end()) status.push_back(flag);
  } else {
    if (it != status.end()) status.erase(it);
  }
}

}  // namespace lvmfmt
