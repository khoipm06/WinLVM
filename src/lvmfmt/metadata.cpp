#include "lvmfmt/metadata.h"

#include <algorithm>
#include <set>

#include "lvmfmt/bytes.h"
#include "lvmfmt/config.h"

namespace lvmfmt {
namespace {

// Segment types supported by WinLVM.
bool supported_segment_type(const std::string& type) {
  return type == "striped" || type == "linear" || type == "thin-pool" || type == "thin" ||
         type == "snapshot" || type == "raid1" || type == "mirror";
}

// LV status flags that mean "do not touch this, even if the segments look
// simple".
const char* const kRefusedStatus[] = {
    "PVMOVE", "LOCKED", "CONVERTING", "MERGING", "VIRTUAL",
};

bool has_status_flag(const std::vector<std::string>& status, std::string_view flag) {
  return std::find(status.begin(), status.end(), flag) != status.end();
}

bool ends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool is_snapshot_wrapper_name(std::string_view name) {
  if (name.size() < 9 || name.substr(0, 8) != "snapshot") return false;
  for (std::size_t i = 8; i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  return true;
}

std::optional<std::uint64_t> non_negative(std::optional<std::int64_t> v) {
  if (!v || *v < 0) return std::nullopt;
  return static_cast<std::uint64_t>(*v);
}

void parse_segment(const Section& sec, Segment& seg) {
  seg.start_extent = non_negative(sec.integer("start_extent")).value_or(0);
  seg.extent_count = non_negative(sec.integer("extent_count")).value_or(0);
  seg.type = sec.string("type").value_or("");
  seg.stripe_count = non_negative(sec.integer("stripe_count")).value_or(1);
  seg.stripe_size_sectors = non_negative(sec.integer("stripe_size")).value_or(0);

  if (auto v = sec.string("metadata")) seg.metadata_lv = *v;
  if (auto v = sec.string("pool")) seg.pool_lv = *v;
  if (auto v = sec.string("thin_pool")) seg.pool_lv = *v;
  if (auto v = non_negative(sec.integer("chunk_size"))) seg.chunk_size_sectors = *v;
  if (auto v = non_negative(sec.integer("transaction_id"))) {
    seg.transaction_id = *v;
    seg.has_transaction_id = true;
  }
  if (auto v = sec.integer("zero_new_blocks")) {
    seg.zero_new_blocks = (*v != 0);
    seg.has_zero_new_blocks = true;
  }
  if (auto v = sec.string("discards")) seg.discards = *v;
  if (auto v = non_negative(sec.integer("device_id"))) {
    seg.device_id = *v;
    seg.has_device_id = true;
  }
  if (auto v = sec.string("origin")) seg.origin_lv = *v;
  if (auto v = sec.string("cow_store")) seg.cow_store_lv = *v;
  else if (auto vc = sec.string("cow")) seg.cow_store_lv = *vc;
  if (auto v = non_negative(sec.integer("region_size"))) seg.region_size_sectors = *v;
  if (auto v = non_negative(sec.integer("device_count"))) seg.device_count = *v;
  else if (auto v2 = non_negative(sec.integer("raid_count"))) seg.device_count = *v2;
  else if (auto v3 = non_negative(sec.integer("area_count"))) seg.device_count = *v3;
  if (auto v = non_negative(sec.integer("mirror_count"))) seg.mirror_count = *v;
  if (auto v = sec.string("mirror_log")) seg.mirror_log_lv = *v;

  // `stripes = ["pv0", 0, "pv1", 0]` — alternating target and start extent.
  if (const auto* entry = sec.find_entry("stripes")) {
    for (std::size_t i = 0; i + 1 < entry->size(); i += 2) {
      const Value& target = (*entry)[i];
      const Value& start = (*entry)[i + 1];
      if (!target.is_string() || !start.is_integer() || start.integer_value < 0) continue;
      seg.stripes.push_back(
          SegmentStripe{target.string_value, static_cast<std::uint64_t>(start.integer_value)});
    }
  }

  // `raids = ["lv_rmeta_0", "lv_rimage_0", ...]` — pairs of meta + image sub-LV names.
  seg.raids = sec.string_list("raids");

  // `mirrors = ["lv_mimage_0", 0, "lv_mimage_1", 0]` — alternating image sub-LV and extent.
  if (const auto* entry = sec.find_entry("mirrors")) {
    for (std::size_t i = 0; i + 1 < entry->size(); i += 2) {
      const Value& target = (*entry)[i];
      const Value& start = (*entry)[i + 1];
      if (!target.is_string() || !start.is_integer() || start.integer_value < 0) continue;
      seg.mirrors.push_back(
          SegmentStripe{target.string_value, static_cast<std::uint64_t>(start.integer_value)});
    }
  }
  if (seg.type == "mirror" && seg.mirror_count == 0) {
    seg.mirror_count = seg.mirrors.size();
  }
}

// Names this LV's segments point at that are internal sub-volumes (not PVs).
// "thin_pool" references a top-level thin-pool LV, not an internal sub-volume;
// only its "metadata" (_tmeta) and "pool" (_tdata) are internal.
void collect_references(const Section& lv_sec, std::set<std::string>& internal_out) {
  static const char* const kInternalKeys[] = {
      "pool_metadata", "metadata", "pool", "data", "mirror_log",
      "cache_pool", "integrity", "vdo_pool", "writecache",
  };
  for (const auto& seg : lv_sec.sections) {
    for (const char* key : kInternalKeys) {
      if (auto v = seg.string(key)) internal_out.insert(*v);
    }
    if (const auto* entry = seg.find_entry("raids")) {
      for (const auto& item : *entry) {
        if (item.is_string() && !item.string_value.empty()) internal_out.insert(item.string_value);
      }
    }
    if (const auto* entry = seg.find_entry("mirrors")) {
      for (const auto& item : *entry) {
        if (item.is_string() && !item.string_value.empty()) internal_out.insert(item.string_value);
      }
    }
  }
}

}  // namespace

std::uint64_t LogicalVolume::extent_count() const {
  std::uint64_t total = 0;
  for (const auto& seg : segments) {
    auto next = checked_add(total, seg.extent_count);
    if (!next) return total;  // saturate rather than wrap
    total = *next;
  }
  return total;
}

bool LogicalVolume::is_read_only() const {
  return !status.empty() && !has_status_flag(status, "WRITE");
}

bool LogicalVolume::is_visible() const {
  if (is_snapshot_cow) return true;
  return status.empty() || has_status_flag(status, "VISIBLE");
}

bool PhysicalVolume::is_missing() const {
  return has_status_flag(status, "MISSING") || has_status_flag(flags, "MISSING");
}

bool PhysicalVolume::is_allocatable() const {
  if (is_missing()) return false;
  if (has_status) return has_status_flag(status, "ALLOCATABLE");
  return status.empty() || has_status_flag(status, "ALLOCATABLE");
}

bool VolumeGroup::is_read_only() const {
  return !status.empty() && !has_status_flag(status, "WRITE");
}

bool VolumeGroup::is_exported() const {
  return has_status_flag(status, "EXPORTED");
}

bool VolumeGroup::is_resizeable() const {
  return status.empty() || has_status_flag(status, "RESIZEABLE");
}

const PhysicalVolume* VolumeGroup::find_pv(std::string_view key) const {
  for (const auto& pv : pvs) {
    if (pv.key == key) return &pv;
  }
  return nullptr;
}

const PhysicalVolume* VolumeGroup::find_pv_by_uuid(std::string_view uuid) const {
  auto strip_hyphens = [](std::string_view s) {
    std::string out;
    out.reserve(32);
    for (char c : s) {
      if (c != '-') out.push_back(c);
    }
    return out;
  };
  std::string target = strip_hyphens(uuid);
  for (const auto& pv : pvs) {
    if (strip_hyphens(pv.id) == target) return &pv;
  }
  return nullptr;
}

const LogicalVolume* VolumeGroup::find_lv(std::string_view lv_name) const {
  for (const auto& lv : lvs) {
    if (lv.name == lv_name) return &lv;
  }
  return nullptr;
}

std::optional<VolumeGroup> parse_vg_metadata(std::string_view text, std::string& error) {
  auto root = parse_config(text, error);
  if (!root) return std::nullopt;

  // The volume group is the sole top-level section; the remaining top-level
  // entries are bookkeeping (contents, version, creation_host, ...).
  const Section* vg_sec = nullptr;
  for (const auto& s : root->sections) {
    if (s.find_entry("id") && s.find_entry("extent_size")) {
      vg_sec = &s;
      break;
    }
  }
  if (!vg_sec) {
    error = "no volume group section found in metadata";
    return std::nullopt;
  }

  VolumeGroup vg;
  vg.name = vg_sec->name;
  vg.id = vg_sec->string("id").value_or("");
  vg.seqno = non_negative(vg_sec->integer("seqno")).value_or(0);
  vg.extent_size_sectors = non_negative(vg_sec->integer("extent_size")).value_or(0);
  vg.system_id = vg_sec->string("system_id").value_or("");
  vg.status = vg_sec->string_list("status");
  vg.has_status = (vg_sec->find_entry("status") != nullptr);
  vg.flags = vg_sec->string_list("flags");
  vg.tags = vg_sec->string_list("tags");
  vg.max_lv = non_negative(vg_sec->integer("max_lv")).value_or(0);
  vg.max_pv = non_negative(vg_sec->integer("max_pv")).value_or(0);
  vg.metadata_copies = non_negative(vg_sec->integer("metadata_copies")).value_or(0);

  if (vg.extent_size_sectors == 0) {
    error = "volume group declares a zero extent size";
    return std::nullopt;
  }

  if (const Section* pvs_sec = vg_sec->find_section("physical_volumes")) {
    for (const auto& pv_sec : pvs_sec->sections) {
      PhysicalVolume pv;
      pv.key = pv_sec.name;
      pv.id = pv_sec.string("id").value_or("");
      pv.device = pv_sec.string("device").value_or("");
      pv.status = pv_sec.string_list("status");
      pv.has_status = (pv_sec.find_entry("status") != nullptr);
      pv.flags = pv_sec.string_list("flags");
      pv.tags = pv_sec.string_list("tags");
      pv.pe_start_sectors = non_negative(pv_sec.integer("pe_start")).value_or(0);
      pv.pe_count = non_negative(pv_sec.integer("pe_count")).value_or(0);
      pv.dev_size_sectors = non_negative(pv_sec.integer("dev_size")).value_or(0);
      if (pv.id.empty()) {
        error = "physical volume '" + pv.key + "' has no id";
        return std::nullopt;
      }
      vg.pvs.push_back(std::move(pv));
    }
  }
  if (vg.pvs.empty()) {
    error = "volume group lists no physical volumes";
    return std::nullopt;
  }

  std::set<std::string> referenced;
  const Section* lvs_sec = vg_sec->find_section("logical_volumes");
  if (lvs_sec) {
    for (const auto& lv_sec : lvs_sec->sections) {
      collect_references(lv_sec, referenced);

      LogicalVolume lv;
      lv.name = lv_sec.name;
      lv.id = lv_sec.string("id").value_or("");
      lv.status = lv_sec.string_list("status");
      lv.has_status = (lv_sec.find_entry("status") != nullptr);
      lv.flags = lv_sec.string_list("flags");
      lv.tags = lv_sec.string_list("tags");
      lv.creation_time = non_negative(lv_sec.integer("creation_time")).value_or(0);
      lv.creation_host = lv_sec.string("creation_host").value_or("");

      for (const auto& seg_sec : lv_sec.sections) {
        if (seg_sec.name.rfind("segment", 0) != 0) continue;
        Segment seg;
        parse_segment(seg_sec, seg);
        lv.segments.push_back(std::move(seg));
      }
      std::stable_sort(lv.segments.begin(), lv.segments.end(),
                       [](const Segment& a, const Segment& b) {
                         return a.start_extent < b.start_extent;
                       });

      for (const char* flag : kRefusedStatus) {
        if (has_status_flag(lv.status, flag)) {
          lv.supported = false;
          lv.unsupported_reason = std::string("logical volume has status ") + flag;
          break;
        }
      }
      if (lv.supported) {
        if (lv.segments.empty()) {
          lv.supported = false;
          lv.unsupported_reason = "logical volume has no segments";
        }
        for (const auto& seg : lv.segments) {
          if (!supported_segment_type(seg.type)) {
            lv.supported = false;
            lv.unsupported_reason = "unsupported segment type '" + seg.type + "'";
            break;
          }
          if (seg.type == "thin-pool") {
            lv.is_thin_pool = true;
          }
        }
      }

      vg.lvs.push_back(std::move(lv));
    }
  }

  // Second pass: mark internal sub-volumes, thin-pool containers, and snapshot wrappers/origins.
  for (auto& lv : vg.lvs) {
    if (!lv.status.empty() && !has_status_flag(lv.status, "VISIBLE")) {
      lv.internal = true;
    }
    if (referenced.count(lv.name) || ends_with(lv.name, "_pmspare")) {
      lv.internal = true;
    }
  }

  for (auto& lv : vg.lvs) {
    for (const auto& seg : lv.segments) {
      if (seg.type == "snapshot") {
        if (auto* orig = const_cast<LogicalVolume*>(vg.find_lv(seg.origin_lv))) {
          orig->is_snapshot_origin = true;
        }
        auto* cow = const_cast<LogicalVolume*>(vg.find_lv(seg.cow_store_lv));
        if (is_snapshot_wrapper_name(lv.name) && cow && !ends_with(cow->name, "_cow")) {
          lv.internal = true;
          cow->is_snapshot_cow = true;
          cow->snapshot_wrapper = lv.name;
          cow->internal = false;
        } else if (cow) {
          cow->internal = true;
        }
      }
      for (const auto& stripe : seg.stripes) {
        if (!vg.find_pv(stripe.target)) {
          if (auto* target = const_cast<LogicalVolume*>(vg.find_lv(stripe.target))) {
            target->internal = true;
          }
          if (lv.supported) {
            lv.supported = false;
            lv.unsupported_reason =
                "segment maps onto logical volume '" + stripe.target + "', not a physical volume";
          }
        }
      }
    }
  }

  return vg;
}

}  // namespace lvmfmt
