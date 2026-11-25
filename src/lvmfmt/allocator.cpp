#include "lvmfmt/allocator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace lvmfmt {

namespace {

std::string trim_view(std::string_view str) {
  while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) str.remove_prefix(1);
  while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) str.remove_suffix(1);
  return std::string(str);
}

std::string strip_uuid_hyphens(std::string_view s) {
  std::string out;
  out.reserve(32);
  for (char c : s) {
    if (c != '-') out.push_back(c);
  }
  return out;
}

bool pv_matches_ref(const PvExtentMap& m, std::string_view ref) {
  if (m.pv_key == ref || m.pv_uuid == ref) return true;
  if (ref.size() >= 32 && strip_uuid_hyphens(m.pv_uuid) == strip_uuid_hyphens(ref)) return true;
  return false;
}

bool append_striped_chunks(std::vector<Segment>& target_segments,
                           std::uint64_t start_le,
                           std::uint64_t total_extents,
                           std::uint32_t stripe_count,
                           std::uint64_t stripe_size_sectors,
                           const std::vector<std::vector<ExtentAllocation>>& stripe_allocs,
                           std::string& error) {
  std::vector<std::size_t> chunk_idx(stripe_count, 0);
  std::vector<std::uint64_t> chunk_off(stripe_count, 0);
  std::uint64_t cur_le = start_le;
  const std::uint64_t end_le = start_le + total_extents;

  while (cur_le < end_le) {
    std::uint64_t run_len = UINT64_MAX;
    for (std::uint32_t s = 0; s < stripe_count; ++s) {
      if (chunk_idx[s] >= stripe_allocs[s].size()) {
        run_len = 0;
        break;
      }
      const std::uint64_t avail = stripe_allocs[s][chunk_idx[s]].count - chunk_off[s];
      if (avail < run_len) run_len = avail;
    }
    if (run_len == 0 || run_len == UINT64_MAX) {
      error = "internal allocation mismatch during striped segment construction";
      return false;
    }

    Segment seg;
    seg.start_extent = cur_le;
    seg.extent_count = run_len * stripe_count;
    seg.type = "striped";
    seg.stripe_count = stripe_count;
    seg.stripe_size_sectors = stripe_size_sectors;
    for (std::uint32_t s = 0; s < stripe_count; ++s) {
      const std::uint64_t pe = stripe_allocs[s][chunk_idx[s]].start_pe + chunk_off[s];
      seg.stripes.push_back({stripe_allocs[s][chunk_idx[s]].pv_key, pe});
      chunk_off[s] += run_len;
      if (chunk_off[s] == stripe_allocs[s][chunk_idx[s]].count) {
        chunk_idx[s]++;
        chunk_off[s] = 0;
      }
    }

    // Coalesce with the previous segment if every stripe continues contiguously
    // on the same PVs in the same order.
    bool coalesced = false;
    if (!target_segments.empty()) {
      Segment& prev = target_segments.back();
      if (prev.stripe_count == seg.stripe_count &&
          prev.stripe_size_sectors == seg.stripe_size_sectors &&
          prev.stripes.size() == seg.stripes.size() &&
          prev.start_extent + prev.extent_count == seg.start_extent) {
        const std::uint64_t prev_per_stripe = prev.extent_count / prev.stripe_count;
        bool all_contig = true;
        for (std::size_t s = 0; s < prev.stripes.size(); ++s) {
          if (prev.stripes[s].target != seg.stripes[s].target ||
              prev.stripes[s].start_extent + prev_per_stripe != seg.stripes[s].start_extent) {
            all_contig = false;
            break;
          }
        }
        if (all_contig) {
          prev.extent_count += seg.extent_count;
          coalesced = true;
        }
      }
    }

    if (!coalesced) {
      target_segments.push_back(std::move(seg));
    }
    cur_le += run_len * stripe_count;
  }
  return true;
}

}  // namespace

std::optional<SizeSpec> parse_size_spec(std::string_view raw, std::string& error) {
  std::string trimmed = trim_view(raw);
  std::string_view str = trimmed;
  if (str.empty()) {
    error = "empty size string";
    return std::nullopt;
  }

  SizeSpec spec;
  if (str.front() == '+') {
    spec.sign = SizeSign::Plus;
    str.remove_prefix(1);
  } else if (str.front() == '-') {
    spec.sign = SizeSign::Minus;
    str.remove_prefix(1);
  }

  std::size_t num_end = 0;
  bool seen_dot = false;
  while (num_end < str.size()) {
    const char c = str[num_end];
    if (std::isdigit(static_cast<unsigned char>(c))) {
      num_end++;
    } else if (c == '.' && !seen_dot) {
      seen_dot = true;
      num_end++;
    } else {
      break;
    }
  }
  if (num_end == 0 || (num_end == 1 && seen_dot)) {
    error = "no numeric digits in size string: " + trimmed;
    return std::nullopt;
  }

  double value = 0.0;
  try {
    value = std::stod(std::string(str.substr(0, num_end)));
  } catch (...) {
    error = "failed to parse numeric value: " + std::string(str.substr(0, num_end));
    return std::nullopt;
  }
  if (!std::isfinite(value) || value < 0.0) {
    error = "size value out of range: " + trimmed;
    return std::nullopt;
  }

  std::string suffix;
  for (std::size_t i = num_end; i < str.size(); ++i) {
    suffix.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(str[i]))));
  }

  std::uint64_t multiplier = 1;
  if (suffix.empty() || suffix == "B") {
    multiplier = 1ULL;
  } else if (suffix == "S") {
    multiplier = 512ULL;
  } else if (suffix == "K" || suffix == "KB" || suffix == "KIB") {
    multiplier = 1024ULL;
  } else if (suffix == "M" || suffix == "MB" || suffix == "MIB") {
    multiplier = 1024ULL * 1024ULL;
  } else if (suffix == "G" || suffix == "GB" || suffix == "GIB") {
    multiplier = 1024ULL * 1024ULL * 1024ULL;
  } else if (suffix == "T" || suffix == "TB" || suffix == "TIB") {
    multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
  } else if (suffix == "P" || suffix == "PB" || suffix == "PIB") {
    multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
  } else {
    error = "unknown size unit suffix: " + suffix;
    return std::nullopt;
  }

  const double total = value * static_cast<double>(multiplier);
  if (!std::isfinite(total) || total < 0.0 || total > static_cast<double>(UINT64_MAX)) {
    error = "size out of bounds";
    return std::nullopt;
  }
  spec.value = static_cast<std::uint64_t>(total);
  return spec;
}

std::optional<SizeSpec> parse_extents_spec(std::string_view raw, std::string& error) {
  std::string trimmed = trim_view(raw);
  std::string_view str = trimmed;
  if (str.empty()) {
    error = "empty extent specification";
    return std::nullopt;
  }

  SizeSpec spec;
  if (str.front() == '+') {
    spec.sign = SizeSign::Plus;
    str.remove_prefix(1);
  } else if (str.front() == '-') {
    spec.sign = SizeSign::Minus;
    str.remove_prefix(1);
  }

  std::size_t num_end = 0;
  while (num_end < str.size() && std::isdigit(static_cast<unsigned char>(str[num_end]))) {
    num_end++;
  }
  if (num_end == 0) {
    error = "invalid extent count: " + trimmed;
    return std::nullopt;
  }

  try {
    spec.value = std::stoull(std::string(str.substr(0, num_end)));
  } catch (...) {
    error = "extent count out of range: " + trimmed;
    return std::nullopt;
  }

  if (num_end == str.size()) {
    return spec;
  }

  if (str[num_end] != '%') {
    error = "unexpected trailing characters in extent specification: " + trimmed;
    return std::nullopt;
  }

  std::string origin;
  for (std::size_t i = num_end + 1; i < str.size(); ++i) {
    origin.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(str[i]))));
  }
  if (origin != "FREE" && origin != "VG" && origin != "PVS" && origin != "LV" &&
      origin != "ORIGIN") {
    error = "unsupported extent percentage origin: %" + origin;
    return std::nullopt;
  }
  if (spec.value == 0 || spec.value > 100) {
    error = "extent percentage must be between 1 and 100";
    return std::nullopt;
  }

  spec.is_percent = true;
  spec.percent_origin = std::move(origin);
  return spec;
}

std::optional<std::uint64_t> parse_size_bytes(std::string_view str, std::string& error) {
  auto spec = parse_size_spec(str, error);
  if (!spec) return std::nullopt;
  if (spec->sign == SizeSign::Minus) {
    error = "negative size is not allowed here";
    return std::nullopt;
  }
  return spec->value;
}

std::string format_size_human(std::uint64_t bytes) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(2);
  if (bytes >= 1024ULL * 1024ULL * 1024ULL * 1024ULL) {
    ss << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0 * 1024.0)) << " TiB";
  } else if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
    ss << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << " GiB";
  } else if (bytes >= 1024ULL * 1024ULL) {
    ss << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MiB";
  } else if (bytes >= 1024ULL) {
    ss << (static_cast<double>(bytes) / 1024.0) << " KiB";
  } else {
    ss << bytes << " B";
  }
  return ss.str();
}

std::vector<PvExtentMap> build_vg_extent_maps(const VolumeGroup& vg) {
  std::vector<PvExtentMap> maps;
  maps.reserve(vg.pvs.size());
  for (const auto& pv : vg.pvs) {
    PvExtentMap m;
    m.pv_key = pv.key;
    m.pv_uuid = pv.id;
    m.total_extents = pv.pe_count;
    m.extent_used.resize(static_cast<std::size_t>(pv.pe_count), false);
    m.free_extents = pv.pe_count;
    m.allocatable = pv.is_allocatable();
    maps.push_back(std::move(m));
  }

  for (const auto& lv : vg.lvs) {
    for (const auto& seg : lv.segments) {
      const std::uint64_t stripe_extents =
          (seg.stripe_count > 1) ? (seg.extent_count / seg.stripe_count) : seg.extent_count;
      for (const auto& stripe : seg.stripes) {
        for (auto& m : maps) {
          if (m.pv_key == stripe.target) {
            for (std::uint64_t e = 0; e < stripe_extents; ++e) {
              const std::uint64_t pe = stripe.start_extent + e;
              if (pe < m.total_extents && !m.extent_used[static_cast<std::size_t>(pe)]) {
                m.extent_used[static_cast<std::size_t>(pe)] = true;
                if (m.free_extents > 0) m.free_extents--;
              }
            }
            break;
          }
        }
      }
    }
  }
  return maps;
}

bool allocate_linear(const VolumeGroup& vg,
                     std::uint64_t extents_needed,
                     const std::vector<std::string>& preferred_pvs,
                     std::vector<ExtentAllocation>& out_allocations,
                     std::string& error) {
  out_allocations.clear();
  if (extents_needed == 0) return true;

  auto maps = build_vg_extent_maps(vg);
  std::uint64_t remaining = extents_needed;

  std::vector<std::size_t> candidate_indices;
  if (!preferred_pvs.empty()) {
    for (const auto& pref : preferred_pvs) {
      for (std::size_t i = 0; i < maps.size(); ++i) {
        if (pv_matches_ref(maps[i], pref) && maps[i].allocatable) {
          candidate_indices.push_back(i);
          break;
        }
      }
    }
  } else {
    for (std::size_t i = 0; i < maps.size(); ++i) {
      if (maps[i].allocatable) candidate_indices.push_back(i);
    }
  }

  for (std::size_t idx : candidate_indices) {
    auto& m = maps[idx];
    if (m.free_extents == 0) continue;

    std::uint64_t pe = 0;
    while (pe < m.total_extents && remaining > 0) {
      if (!m.extent_used[static_cast<std::size_t>(pe)]) {
        const std::uint64_t run_start = pe;
        std::uint64_t run_len = 0;
        while (pe < m.total_extents && !m.extent_used[static_cast<std::size_t>(pe)] && remaining > 0) {
          m.extent_used[static_cast<std::size_t>(pe)] = true;
          m.free_extents--;
          run_len++;
          remaining--;
          pe++;
        }
        out_allocations.push_back({m.pv_key, run_start, run_len});
      } else {
        pe++;
      }
    }
    if (remaining == 0) break;
  }

  if (remaining > 0) {
    out_allocations.clear();
    error = "insufficient free extents in volume group: requested " +
            std::to_string(extents_needed) + ", short by " + std::to_string(remaining);
    return false;
  }
  return true;
}

bool allocate_striped(const VolumeGroup& vg,
                      std::uint64_t extents_needed,
                      std::uint32_t stripe_count,
                      const std::vector<std::string>& preferred_pvs,
                      std::vector<std::vector<ExtentAllocation>>& out_stripe_allocations,
                      std::string& error) {
  out_stripe_allocations.clear();
  if (stripe_count == 0) {
    error = "stripe count must be >= 1";
    return false;
  }
  if (extents_needed % stripe_count != 0) {
    error = "extent count (" + std::to_string(extents_needed) +
            ") must be a multiple of stripe count (" + std::to_string(stripe_count) + ")";
    return false;
  }

  auto maps = build_vg_extent_maps(vg);
  std::size_t allocatable_count = 0;
  for (const auto& m : maps) {
    if (m.allocatable) allocatable_count++;
  }
  if (allocatable_count < stripe_count) {
    error = "volume group only has " + std::to_string(allocatable_count) +
            " allocatable PVs, but " + std::to_string(stripe_count) + " stripes requested";
    return false;
  }

  const std::uint64_t extents_per_stripe = extents_needed / stripe_count;
  std::vector<std::string> target_pvs;
  std::vector<bool> pv_used(maps.size(), false);

  // 1. Honor preferred_pvs if provided.
  for (std::size_t i = 0; i < preferred_pvs.size() && target_pvs.size() < stripe_count; ++i) {
    const auto& pref = preferred_pvs[i];
    bool found = false;
    for (std::size_t m = 0; m < maps.size(); ++m) {
      if (pv_matches_ref(maps[m], pref) && maps[m].allocatable) {
        if (pv_used[m]) {
          error = "duplicate physical volume in stripe specification: " + pref;
          return false;
        }
        pv_used[m] = true;
        target_pvs.push_back(maps[m].pv_key);
        found = true;
        break;
      }
    }
    if (!found) {
      error = "preferred physical volume not found or not allocatable: " + pref;
      return false;
    }
  }

  // 2. Select distinct allocatable PVs with sufficient free capacity.
  for (std::size_t m = 0; m < maps.size() && target_pvs.size() < stripe_count; ++m) {
    if (!pv_used[m] && maps[m].allocatable && maps[m].free_extents >= extents_per_stripe) {
      target_pvs.push_back(maps[m].pv_key);
      pv_used[m] = true;
    }
  }

  // 3. Fallback to remaining allocatable PVs so allocate_linear reports exact shortage.
  for (std::size_t m = 0; m < maps.size() && target_pvs.size() < stripe_count; ++m) {
    if (!pv_used[m] && maps[m].allocatable) {
      target_pvs.push_back(maps[m].pv_key);
      pv_used[m] = true;
    }
  }

  std::vector<std::vector<ExtentAllocation>> stripe_allocs(stripe_count);
  for (std::uint32_t s = 0; s < stripe_count; ++s) {
    std::vector<std::string> single_pv = {target_pvs[s]};
    if (!allocate_linear(vg, extents_per_stripe, single_pv, stripe_allocs[s], error)) {
      out_stripe_allocations.clear();
      return false;
    }
  }

  out_stripe_allocations = std::move(stripe_allocs);
  return true;
}

bool extend_lv(VolumeGroup& vg,
               std::string_view lv_name,
               std::uint64_t additional_extents,
               const std::vector<std::string>& preferred_pvs,
               std::string& error) {
  LogicalVolume* target = nullptr;
  for (auto& lv : vg.lvs) {
    if (lv.name == lv_name) {
      target = &lv;
      break;
    }
  }
  if (!target) {
    error = "logical volume not found: " + std::string(lv_name);
    return false;
  }
  if (!target->supported) {
    error = "cannot extend unsupported logical volume: " + target->unsupported_reason;
    return false;
  }
  if (additional_extents == 0) return true;

  const std::uint64_t cur_le = target->extent_count();

  // Preserve striped layout when the LV's trailing segment is striped.
  if (!target->segments.empty() && target->segments.back().stripe_count > 1) {
    const Segment& last_seg = target->segments.back();
    const std::uint32_t sc = static_cast<std::uint32_t>(last_seg.stripe_count);
    const std::uint64_t ss = last_seg.stripe_size_sectors;
    if (additional_extents % sc != 0) {
      error = "additional extent count (" + std::to_string(additional_extents) +
              ") must be a multiple of stripe count (" + std::to_string(sc) + ")";
      return false;
    }

    // Prefer the existing stripe PVs first when no explicit preferred PVs were given
    // and they can accommodate the extension, falling back to any allocatable PVs.
    std::vector<std::vector<ExtentAllocation>> stripe_allocs;
    bool ok = false;
    if (preferred_pvs.empty()) {
      std::vector<std::string> existing_pvs;
      existing_pvs.reserve(last_seg.stripes.size());
      for (const auto& st : last_seg.stripes) existing_pvs.push_back(st.target);
      std::string try_err;
      ok = allocate_striped(vg, additional_extents, sc, existing_pvs, stripe_allocs, try_err);
    }
    if (!ok) {
      if (!allocate_striped(vg, additional_extents, sc, preferred_pvs, stripe_allocs, error)) {
        return false;
      }
    }

    if (!append_striped_chunks(target->segments, cur_le, additional_extents, sc, ss,
                               stripe_allocs, error)) {
      return false;
    }
    vg.seqno++;
    return true;
  }

  std::vector<ExtentAllocation> allocs;
  if (!allocate_linear(vg, additional_extents, preferred_pvs, allocs, error)) {
    return false;
  }

  std::uint64_t next_le = cur_le;
  for (const auto& a : allocs) {
    if (!target->segments.empty()) {
      Segment& prev = target->segments.back();
      if (prev.stripe_count == 1 && prev.stripes.size() == 1 &&
          prev.start_extent + prev.extent_count == next_le &&
          prev.stripes[0].target == a.pv_key &&
          prev.stripes[0].start_extent + prev.extent_count == a.start_pe) {
        prev.extent_count += a.count;
        next_le += a.count;
        continue;
      }
    }
    Segment seg;
    seg.start_extent = next_le;
    seg.extent_count = a.count;
    seg.type = "linear";
    seg.stripe_count = 1;
    seg.stripe_size_sectors = 0;
    seg.stripes.push_back({a.pv_key, a.start_pe});
    target->segments.push_back(std::move(seg));
    next_le += a.count;
  }
  vg.seqno++;
  return true;
}

bool reduce_lv(VolumeGroup& vg,
               std::string_view lv_name,
               std::uint64_t extents_to_remove,
               std::string& error) {
  LogicalVolume* target = nullptr;
  for (auto& lv : vg.lvs) {
    if (lv.name == lv_name) {
      target = &lv;
      break;
    }
  }
  if (!target) {
    error = "logical volume not found: " + std::string(lv_name);
    return false;
  }
  if (!target->supported) {
    error = "cannot reduce unsupported logical volume: " +
            (target->unsupported_reason.empty() ? "composite or non-mappable layout"
                                                : target->unsupported_reason);
    return false;
  }
  if (target->internal) {
    error = "cannot reduce internal logical volume: " + std::string(lv_name);
    return false;
  }
  for (const auto& seg : target->segments) {
    if (seg.type != "linear" && seg.type != "striped") {
      error = "cannot reduce logical volume with segment type '" + seg.type + "'";
      return false;
    }
  }
  for (const auto& other : vg.lvs) {
    for (const auto& seg : other.segments) {
      if (seg.type == "snapshot" &&
          (seg.origin_lv == target->name || seg.cow_store_lv == target->name)) {
        error = "cannot reduce snapshot or snapshot origin logical volume: " + target->name;
        return false;
      }
    }
  }

  if (extents_to_remove == 0) return true;

  const std::uint64_t total = target->extent_count();
  if (extents_to_remove >= total) {
    error = "cannot reduce LV by " + std::to_string(extents_to_remove) +
            " extents; total size is only " + std::to_string(total) + " extents";
    return false;
  }

  const std::uint64_t target_extents = total - extents_to_remove;
  std::uint64_t accumulated = 0;
  std::vector<Segment> kept_segments;

  for (const auto& seg : target->segments) {
    if (accumulated + seg.extent_count <= target_extents) {
      accumulated += seg.extent_count;
      kept_segments.push_back(seg);
    } else if (accumulated < target_extents) {
      const std::uint64_t keep = target_extents - accumulated;
      if (seg.stripe_count > 1 && (keep % seg.stripe_count) != 0) {
        error = "cannot reduce striped segment to non-multiple of stripe count (" +
                std::to_string(seg.stripe_count) + ")";
        return false;
      }
      Segment truncated = seg;
      truncated.extent_count = keep;
      accumulated += keep;
      kept_segments.push_back(std::move(truncated));
      break;
    } else {
      break;
    }
  }
  target->segments = std::move(kept_segments);
  vg.seqno++;
  return true;
}

bool move_pv_extents(VolumeGroup& vg,
                     std::string_view source_pv_key_or_name,
                     std::string_view dest_pv_key_or_name,
                     std::optional<std::string_view> target_lv_name,
                     ByteSource* source_storage,
                     ByteSource* dest_storage,
                     std::string& error) {
  const PhysicalVolume* src_pv = vg.find_pv(source_pv_key_or_name);
  if (!src_pv) src_pv = vg.find_pv_by_uuid(source_pv_key_or_name);
  if (!src_pv) {
    error = "source PV not found in VG: " + std::string(source_pv_key_or_name);
    return false;
  }

  const PhysicalVolume* dst_pv = vg.find_pv(dest_pv_key_or_name);
  if (!dst_pv) dst_pv = vg.find_pv_by_uuid(dest_pv_key_or_name);
  if (!dst_pv) {
    error = "destination PV not found in VG: " + std::string(dest_pv_key_or_name);
    return false;
  }

  if (src_pv->key == dst_pv->key) {
    error = "source and destination PV cannot be the same";
    return false;
  }

  auto maps = build_vg_extent_maps(vg);
  std::size_t dst_idx = maps.size();
  for (std::size_t i = 0; i < maps.size(); ++i) {
    if (maps[i].pv_key == dst_pv->key) {
      dst_idx = i;
      break;
    }
  }
  if (dst_idx >= maps.size() || !maps[dst_idx].allocatable) {
    error = "destination PV " + dst_pv->key + " is not allocatable";
    return false;
  }

  const std::uint64_t extent_bytes = vg.extent_size_sectors * 512ULL;
  std::vector<std::uint8_t> buffer(64 * 1024);

  // Build sibling-leg conflict groups for raid1/mirror so pvmove never places
  // two redundant legs onto the same PV, and reject targeting composite LVs.
  std::vector<std::vector<std::string>> redundant_groups;
  for (const auto& lv : vg.lvs) {
    for (const auto& seg : lv.segments) {
      if (seg.type == "raid1") {
        std::vector<std::string> images;
        std::vector<std::string> metas;
        for (std::size_t i = 0; i + 1 < seg.raids.size(); i += 2) {
          metas.push_back(seg.raids[i]);
          images.push_back(seg.raids[i + 1]);
        }
        if (images.size() >= 2) redundant_groups.push_back(std::move(images));
        if (metas.size() >= 2) redundant_groups.push_back(std::move(metas));
      } else if (seg.type == "mirror") {
        std::vector<std::string> images;
        for (const auto& m : seg.mirrors) images.push_back(m.target);
        if (images.size() >= 2) redundant_groups.push_back(std::move(images));
      }
    }
  }

  if (target_lv_name) {
    const LogicalVolume* tlv = vg.find_lv(*target_lv_name);
    if (!tlv) {
      error = "logical volume not found: " + std::string(*target_lv_name);
      return false;
    }
    for (const auto& seg : tlv->segments) {
      if (seg.type != "linear" && seg.type != "striped") {
        error = "pvmove does not support composite segment type '" + seg.type +
                "' on logical volume '" + tlv->name + "'";
        return false;
      }
    }
  }

  auto lv_uses_pv = [&](const std::string& lv_name, const std::string& pv_key) -> bool {
    const LogicalVolume* lv = vg.find_lv(lv_name);
    if (!lv) return false;
    for (const auto& s : lv->segments) {
      for (const auto& st : s.stripes) {
        if (st.target == pv_key) return true;
      }
    }
    return false;
  };

  // Pre-validate that moving extents from src_pv to dst_pv won't colocate redundant legs.
  for (const auto& lv : vg.lvs) {
    if (target_lv_name && lv.name != *target_lv_name) continue;
    if (!lv_uses_pv(lv.name, src_pv->key)) continue;
    for (const auto& grp : redundant_groups) {
      if (std::find(grp.begin(), grp.end(), lv.name) == grp.end()) continue;
      for (const auto& sibling : grp) {
        if (sibling != lv.name && lv_uses_pv(sibling, dst_pv->key)) {
          error = "refusing pvmove of '" + lv.name + "' onto '" + dst_pv->key +
                  "': destination PV already holds redundant leg '" + sibling + "'";
          return false;
        }
      }
    }
  }

  std::size_t moved_segments = 0;
  for (auto& lv : vg.lvs) {
    if (target_lv_name && lv.name != *target_lv_name) continue;

    for (auto& seg : lv.segments) {
      if (seg.type != "linear" && seg.type != "striped") continue;
      for (auto& stripe : seg.stripes) {
        if (stripe.target == src_pv->key) {
          const std::uint64_t count =
              (seg.stripe_count > 1) ? (seg.extent_count / seg.stripe_count) : seg.extent_count;
          std::uint64_t dst_pe = 0;
          bool allocated = false;
          for (std::uint64_t p = 0; p + count <= maps[dst_idx].total_extents; ++p) {
            bool run_free = true;
            for (std::uint64_t k = 0; k < count; ++k) {
              if (maps[dst_idx].extent_used[static_cast<std::size_t>(p + k)]) {
                run_free = false;
                p += k;
                break;
              }
            }
            if (run_free) {
              dst_pe = p;
              for (std::uint64_t k = 0; k < count; ++k) {
                maps[dst_idx].extent_used[static_cast<std::size_t>(dst_pe + k)] = true;
              }
              allocated = true;
              break;
            }
          }

          if (!allocated) {
            error = "destination PV " + dst_pv->key + " has insufficient contiguous extents";
            return false;
          }

          if (source_storage && dest_storage) {
            std::uint64_t src_byte_offset =
                (src_pv->pe_start_sectors * 512ULL) + (stripe.start_extent * extent_bytes);
            std::uint64_t dst_byte_offset =
                (dst_pv->pe_start_sectors * 512ULL) + (dst_pe * extent_bytes);
            std::uint64_t bytes_to_copy = count * extent_bytes;

            while (bytes_to_copy > 0) {
              const std::size_t chunk =
                  static_cast<std::size_t>(std::min<std::uint64_t>(bytes_to_copy, buffer.size()));
              if (!source_storage->read(src_byte_offset, buffer.data(), chunk, error)) {
                error = "pvmove read error at offset " + std::to_string(src_byte_offset) + ": " + error;
                return false;
              }
              if (!dest_storage->write(dst_byte_offset, buffer.data(), chunk, error)) {
                error = "pvmove write error at offset " + std::to_string(dst_byte_offset) + ": " + error;
                return false;
              }
              src_byte_offset += chunk;
              dst_byte_offset += chunk;
              bytes_to_copy -= chunk;
            }
          }

          stripe.target = dst_pv->key;
          stripe.start_extent = dst_pe;
          moved_segments++;
        }
      }
    }
  }

  if (moved_segments == 0) {
    error = "no allocated extents found on source PV '" + src_pv->key + "' to move";
    return false;
  }

  vg.seqno++;
  return true;
}

}  // namespace lvmfmt
