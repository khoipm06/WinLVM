#pragma once

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cmd_common.h"
#include "lvmfmt/allocator.h"
#include "lvmfmt/label.h"
#include "lvmfmt/metadata.h"

namespace lvmfmt {

enum class ReportFormat {
  Default,
  Json,
  JsonStd,
};

inline bool parse_report_format(const std::vector<std::string>& args,
                                ReportFormat& out,
                                std::string& error) {
  out = ReportFormat::Default;
  auto val = get_opt_value(args, "", "--reportformat");
  if (!val) return true;
  if (*val == "basic") {
    out = ReportFormat::Default;
    return true;
  }
  if (*val == "json") {
    out = ReportFormat::Json;
    return true;
  }
  if (*val == "json_std") {
    out = ReportFormat::JsonStd;
    return true;
  }
  error = "unsupported --reportformat value \"" + *val + "\" (expected basic, json, or json_std)";
  return false;
}

inline std::string json_escape(std::string_view input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (char raw : input) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20u) {
          char hex[8];
          std::snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned int>(c));
          out += hex;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

inline std::string join_tags_csv(const std::vector<std::string>& tags) {
  std::string out;
  for (std::size_t i = 0; i < tags.size(); ++i) {
    if (i > 0) out.push_back(',');
    out += tags[i];
  }
  return out;
}

inline std::string json_string_value(std::string_view s) {
  return "\"" + json_escape(s) + "\"";
}

inline std::string json_tags_value(const std::vector<std::string>& tags, ReportFormat fmt) {
  if (fmt == ReportFormat::JsonStd) {
    std::string out = "[";
    for (std::size_t i = 0; i < tags.size(); ++i) {
      if (i > 0) out += ", ";
      out += json_string_value(tags[i]);
    }
    out += "]";
    return out;
  }
  return json_string_value(join_tags_csv(tags));
}

struct PvReportRow {
  std::string pv_name;
  std::string vg_name;
  std::string pv_fmt = "lvm2";
  std::string pv_attr = "---";
  std::string pv_size;
  std::string pv_free;
  std::string pv_uuid;
  std::vector<std::string> pv_tags;
};

struct VgReportRow {
  std::string vg_name;
  std::uint64_t pv_count = 0;
  std::uint64_t lv_count = 0;
  std::uint64_t snap_count = 0;
  std::string vg_attr = "wz--n-";
  std::string vg_size;
  std::string vg_free;
  std::string vg_uuid;
  std::vector<std::string> vg_tags;
};

struct LvReportRow {
  std::string lv_name;
  std::string vg_name;
  std::string lv_attr = "-wi-------";
  std::string lv_size;
  std::string pool_lv;
  std::string origin;
  std::string lv_uuid;
  std::string segtype;
  std::vector<std::string> lv_tags;
  std::uint64_t extents = 0;
  std::size_t segment_count = 0;
  bool read_only = false;
  bool active = false;
  std::uint64_t stripe_count = 1;
  std::uint64_t stripe_size_sectors = 0;
};

inline std::string format_vg_attr_str(const VolumeGroup& vg) {
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

inline std::vector<LvReportRow> build_lv_report_rows(
    const VolumeGroup& vg,
    const std::set<std::string>& active_uuids) {
  std::set<std::string> snapshot_origins;
  std::map<std::string, std::string> cow_to_origin;
  for (const auto& lv : vg.lvs) {
    for (const auto& seg : lv.segments) {
      if (seg.type == "snapshot") {
        if (!seg.origin_lv.empty()) snapshot_origins.insert(seg.origin_lv);
        if (!seg.cow_store_lv.empty()) cow_to_origin[seg.cow_store_lv] = seg.origin_lv;
      }
    }
  }

  const std::uint64_t ext_bytes = vg.extent_size_sectors * 512ULL;
  std::vector<LvReportRow> rows;

  for (const auto& lv : vg.lvs) {
    const std::string primary_type = lv.segments.empty() ? "" : lv.segments[0].type;
    // LVM2 merges synthetic `snapshot<N>` wrapper LVs onto their COW LV row.
    if (primary_type == "snapshot") continue;

    const auto cow_it = cow_to_origin.find(lv.name);
    const bool is_cow_snap = (cow_it != cow_to_origin.end());
    const bool is_thin_pool = (primary_type == "thin-pool");

    if (lv.internal && !is_cow_snap && !is_thin_pool) {
      continue;
    }

    LvReportRow row;
    row.lv_name = lv.name;
    row.vg_name = vg.name;
    row.lv_uuid = format_uuid(lv.id);
    row.lv_tags = lv.tags;
    row.extents = lv.extent_count();
    row.lv_size = format_size_human(row.extents * ext_bytes);
    row.segment_count = lv.segments.size();
    row.read_only = vg.is_read_only() || lv.is_read_only();
    row.active = active_uuids.count(normalize_uuid_raw32(lv.id)) > 0;
    if (!lv.segments.empty()) {
      row.stripe_count = lv.segments[0].stripe_count;
      row.stripe_size_sectors = lv.segments[0].stripe_size_sectors;
    }

    std::string attr = "-wi-------";
    attr[1] = row.read_only ? 'r' : 'w';
    attr[4] = row.active ? 'a' : '-';

    if (is_cow_snap) {
      attr[0] = 's';
      attr[6] = 's';
      row.origin = cow_it->second;
      row.segtype = (row.stripe_count > 1) ? "striped" : "linear";
    } else if (is_thin_pool) {
      attr[0] = 't';
      attr[6] = 't';
      if (!lv.segments.empty() && lv.segments[0].zero_new_blocks) attr[7] = 'z';
      row.segtype = "thin-pool";
    } else if (primary_type == "thin") {
      attr[0] = 'V';
      attr[6] = 't';
      row.pool_lv = lv.segments[0].pool_lv;
      row.origin = lv.segments[0].origin_lv;
      row.segtype = "thin";
    } else if (primary_type == "raid1") {
      attr[0] = 'r';
      attr[6] = 'r';
      row.segtype = "raid1";
    } else if (primary_type == "mirror") {
      attr[0] = 'm';
      attr[6] = 'm';
      row.segtype = "mirror";
    } else if (primary_type == "linear" || primary_type == "striped") {
      if (snapshot_origins.count(lv.name)) {
        attr[0] = 'o';
        attr[6] = 's';
      }
      row.segtype = (row.stripe_count > 1) ? "striped" : "linear";
    } else {
      row.segtype = primary_type;
    }
    row.lv_attr = attr;
    rows.push_back(std::move(row));
  }

  return rows;
}

inline VgReportRow build_vg_report_row(const VolumeGroup& vg) {
  auto maps = build_vg_extent_maps(vg);
  std::uint64_t total_extents = 0;
  std::uint64_t free_extents = 0;
  for (const auto& m : maps) {
    total_extents += m.total_extents;
    free_extents += m.free_extents;
  }
  const std::uint64_t ext_bytes = vg.extent_size_sectors * 512ULL;
  const auto lv_rows = build_lv_report_rows(vg, {});
  std::uint64_t snap_count = 0;
  for (const auto& r : lv_rows) {
    if (!r.lv_attr.empty() && r.lv_attr[0] == 's') snap_count++;
  }

  VgReportRow row;
  row.vg_name = vg.name;
  row.pv_count = vg.pvs.size();
  row.lv_count = lv_rows.size();
  row.snap_count = snap_count;
  row.vg_attr = format_vg_attr_str(vg);
  row.vg_size = format_size_human(total_extents * ext_bytes);
  row.vg_free = format_size_human(free_extents * ext_bytes);
  row.vg_uuid = format_uuid(vg.id);
  row.vg_tags = vg.tags;
  return row;
}

inline void emit_pv_json_array(std::ostream& os,
                               const std::vector<PvReportRow>& rows,
                               ReportFormat fmt,
                               const std::string& indent) {
  os << indent << "\"pv\": [";
  if (!rows.empty()) os << "\n";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& r = rows[i];
    os << indent << "  { "
       << "\"pv_name\": " << json_string_value(r.pv_name) << ", "
       << "\"vg_name\": " << json_string_value(r.vg_name) << ", "
       << "\"pv_fmt\": " << json_string_value(r.pv_fmt) << ", "
       << "\"pv_attr\": " << json_string_value(r.pv_attr) << ", "
       << "\"pv_size\": " << json_string_value(r.pv_size) << ", "
       << "\"pv_free\": " << json_string_value(r.pv_free) << ", "
       << "\"pv_uuid\": " << json_string_value(r.pv_uuid) << ", "
       << "\"pv_tags\": " << json_tags_value(r.pv_tags, fmt)
       << " }";
    if (i + 1 < rows.size()) os << ",";
    os << "\n";
  }
  os << (rows.empty() ? "]" : (indent + "]"));
}

inline void emit_vg_json_array(std::ostream& os,
                               const std::vector<VgReportRow>& rows,
                               ReportFormat fmt,
                               const std::string& indent) {
  os << indent << "\"vg\": [";
  if (!rows.empty()) os << "\n";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& r = rows[i];
    const auto fmt_num = [&](std::uint64_t n) -> std::string {
      return (fmt == ReportFormat::JsonStd) ? std::to_string(n)
                                            : json_string_value(std::to_string(n));
    };
    os << indent << "  { "
       << "\"vg_name\": " << json_string_value(r.vg_name) << ", "
       << "\"pv_count\": " << fmt_num(r.pv_count) << ", "
       << "\"lv_count\": " << fmt_num(r.lv_count) << ", "
       << "\"snap_count\": " << fmt_num(r.snap_count) << ", "
       << "\"vg_attr\": " << json_string_value(r.vg_attr) << ", "
       << "\"vg_size\": " << json_string_value(r.vg_size) << ", "
       << "\"vg_free\": " << json_string_value(r.vg_free) << ", "
       << "\"vg_uuid\": " << json_string_value(r.vg_uuid) << ", "
       << "\"vg_tags\": " << json_tags_value(r.vg_tags, fmt)
       << " }";
    if (i + 1 < rows.size()) os << ",";
    os << "\n";
  }
  os << (rows.empty() ? "]" : (indent + "]"));
}

inline void emit_lv_json_array(std::ostream& os,
                               const std::vector<LvReportRow>& rows,
                               ReportFormat fmt,
                               const std::string& indent) {
  os << indent << "\"lv\": [";
  if (!rows.empty()) os << "\n";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& r = rows[i];
    os << indent << "  { "
       << "\"lv_name\": " << json_string_value(r.lv_name) << ", "
       << "\"vg_name\": " << json_string_value(r.vg_name) << ", "
       << "\"lv_attr\": " << json_string_value(r.lv_attr) << ", "
       << "\"lv_size\": " << json_string_value(r.lv_size) << ", "
       << "\"pool_lv\": " << json_string_value(r.pool_lv) << ", "
       << "\"origin\": " << json_string_value(r.origin) << ", "
       << "\"lv_uuid\": " << json_string_value(r.lv_uuid) << ", "
       << "\"segtype\": " << json_string_value(r.segtype) << ", "
       << "\"lv_tags\": " << json_tags_value(r.lv_tags, fmt)
       << " }";
    if (i + 1 < rows.size()) os << ",";
    os << "\n";
  }
  os << (rows.empty() ? "]" : (indent + "]"));
}

inline void print_pvs_json(const std::vector<PvReportRow>& rows, ReportFormat fmt) {
  std::cout << "{\n  \"report\": [\n    {\n";
  emit_pv_json_array(std::cout, rows, fmt, "      ");
  std::cout << "\n    }\n  ]\n}\n";
}

inline void print_vgs_json(const std::vector<VgReportRow>& rows, ReportFormat fmt) {
  std::cout << "{\n  \"report\": [\n    {\n";
  emit_vg_json_array(std::cout, rows, fmt, "      ");
  std::cout << "\n    }\n  ]\n}\n";
}

inline void print_lvs_json(const std::vector<LvReportRow>& rows, ReportFormat fmt) {
  std::cout << "{\n  \"report\": [\n    {\n";
  emit_lv_json_array(std::cout, rows, fmt, "      ");
  std::cout << "\n    }\n  ]\n}\n";
}

inline void print_fullreport_json(const std::vector<VgReportRow>& vgs,
                                  const std::vector<PvReportRow>& pvs,
                                  const std::vector<LvReportRow>& lvs,
                                  ReportFormat fmt) {
  std::cout << "{\n  \"report\": [\n    {\n";
  emit_vg_json_array(std::cout, vgs, fmt, "      ");
  std::cout << ",\n";
  emit_pv_json_array(std::cout, pvs, fmt, "      ");
  std::cout << ",\n";
  emit_lv_json_array(std::cout, lvs, fmt, "      ");
  std::cout << "\n    }\n  ]\n}\n";
}

// Parse -o / --options column spec. Returns extra columns if "+..." or full list if explicit.
inline std::vector<std::string> parse_csv_tokens(std::string_view spec) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= spec.size()) {
    const auto comma = spec.find(',', start);
    const std::string_view part = (comma == std::string_view::npos)
                                      ? spec.substr(start)
                                      : spec.substr(start, comma - start);
    if (!part.empty()) out.emplace_back(part);
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return out;
}

inline bool options_requests_column(const std::optional<std::string>& opt_spec,
                                    std::string_view col_a,
                                    std::string_view col_b = "") {
  if (!opt_spec || opt_spec->empty()) return false;
  std::string_view s = *opt_spec;
  if (s.front() == '+') s.remove_prefix(1);
  for (const auto& tok : parse_csv_tokens(s)) {
    if (tok == col_a || (!col_b.empty() && tok == col_b)) return true;
  }
  return false;
}

}  // namespace lvmfmt
