#include "lvmfmt/mapping.h"

#include "lvmfmt/bytes.h"

namespace lvmfmt {
namespace {

// extents -> sectors, refusing to wrap.
std::optional<std::uint64_t> extents_to_sectors(std::uint64_t extents,
                                                std::uint64_t extent_size_sectors) {
  return checked_mul(extents, extent_size_sectors);
}

}  // namespace

std::optional<std::vector<DmTarget>> build_dm_table(const VolumeGroup& vg,
                                                    const LogicalVolume& lv,
                                                    std::string& error) {
  if (!lv.supported) {
    error = lv.unsupported_reason.empty() ? "logical volume is not supported"
                                          : lv.unsupported_reason;
    return std::nullopt;
  }
  if (lv.is_thin_pool) {
    error = "thin-pool container LV is not directly mountable";
    return std::nullopt;
  }

  // Classic LVM2 snapshot LV backed by a snapshot0 wrapper segment.
  if (lv.is_snapshot_cow && !lv.snapshot_wrapper.empty()) {
    const LogicalVolume* wrap = vg.find_lv(lv.snapshot_wrapper);
    if (!wrap || wrap->segments.empty() || wrap->segments[0].type != "snapshot") {
      error = "snapshot wrapper '" + lv.snapshot_wrapper + "' not found or invalid";
      return std::nullopt;
    }
    const Segment& wseg = wrap->segments[0];
    const LogicalVolume* orig = vg.find_lv(wseg.origin);
    if (!orig) {
      error = "snapshot origin '" + wseg.origin + "' not found";
      return std::nullopt;
    }
    const auto len_sec = extents_to_sectors(orig->extent_count(), vg.extent_size_sectors);
    if (!len_sec || *len_sec == 0 || wseg.chunk_size_sectors == 0) {
      error = "invalid snapshot geometry";
      return std::nullopt;
    }
    DmTarget t;
    t.start_sector = 0;
    t.length_sectors = *len_sec;
    t.type = "snapshot";
    t.chunk_sectors = wseg.chunk_size_sectors;
    t.origin_lv = wseg.origin;
    t.cow_lv = lv.name;
    return std::vector<DmTarget>{std::move(t)};
  }

  // Snapshot origin LV: exposed as snapshot-origin wrapping its backing extents.
  if (lv.is_snapshot_origin) {
    const auto len_sec = extents_to_sectors(lv.extent_count(), vg.extent_size_sectors);
    if (!len_sec || *len_sec == 0) {
      error = "snapshot origin has invalid extent geometry";
      return std::nullopt;
    }
    DmTarget t;
    t.start_sector = 0;
    t.length_sectors = *len_sec;
    t.type = "snapshot-origin";
    t.origin_lv = lv.name;
    return std::vector<DmTarget>{std::move(t)};
  }

  std::vector<DmTarget> table;
  std::uint64_t expected_start = 0;

  for (const auto& seg : lv.segments) {
    if (seg.extent_count == 0) {
      error = "segment has zero extents";
      return std::nullopt;
    }
    if (seg.start_extent != expected_start) {
      error = "segments do not tile the logical volume: expected extent " +
              std::to_string(expected_start) + ", found " + std::to_string(seg.start_extent);
      return std::nullopt;
    }

    const auto start_sector = extents_to_sectors(seg.start_extent, vg.extent_size_sectors);
    const auto length_sector = extents_to_sectors(seg.extent_count, vg.extent_size_sectors);
    if (!start_sector || !length_sector) {
      error = "segment extent arithmetic overflows";
      return std::nullopt;
    }

    if (seg.type == "thin") {
      if (seg.pool_lv.empty() || !seg.has_device_id || !vg.find_lv(seg.pool_lv)) {
        error = "thin segment references missing thin-pool '" + seg.pool_lv + "'";
        return std::nullopt;
      }
      DmTarget target;
      target.start_sector = *start_sector;
      target.length_sectors = *length_sector;
      target.type = "thin";
      target.pool_lv = seg.pool_lv;
      target.device_id = seg.device_id;
      table.push_back(std::move(target));
      expected_start = seg.start_extent + seg.extent_count;
      continue;
    }

    if (seg.type == "snapshot") {
      if (seg.origin.empty() || seg.cow_store.empty() || seg.chunk_size_sectors == 0 ||
          !vg.find_lv(seg.origin) || !vg.find_lv(seg.cow_store)) {
        error = "snapshot segment missing origin, cow_store, or chunk_size";
        return std::nullopt;
      }
      DmTarget target;
      target.start_sector = *start_sector;
      target.length_sectors = *length_sector;
      target.type = "snapshot";
      target.origin_lv = seg.origin;
      target.cow_lv = seg.cow_store;
      target.chunk_sectors = seg.chunk_size_sectors;
      table.push_back(std::move(target));
      expected_start = seg.start_extent + seg.extent_count;
      continue;
    }

    if (seg.type == "raid1" || seg.type == "mirror") {
      if (seg.raid_legs.empty()) {
        error = seg.type + " segment has no legs";
        return std::nullopt;
      }
      DmTarget target;
      target.start_sector = *start_sector;
      target.length_sectors = *length_sector;
      target.type = seg.type;
      target.region_size_sectors = seg.region_size_sectors;
      target.mirror_log = seg.mirror_log;
      for (const auto& leg : seg.raid_legs) {
        if (!leg.meta_lv.empty()) target.sub_lvs.push_back(leg.meta_lv);
        target.sub_lvs.push_back(leg.image_lv);
      }
      table.push_back(std::move(target));
      expected_start = seg.start_extent + seg.extent_count;
      continue;
    }

    const std::uint64_t stripes = seg.stripe_count == 0 ? 1 : seg.stripe_count;
    if (seg.stripes.size() != stripes) {
      error = "segment declares " + std::to_string(stripes) + " stripes but lists " +
              std::to_string(seg.stripes.size());
      return std::nullopt;
    }
    if (stripes > 1 && *length_sector % stripes != 0) {
      error = "striped segment length is not divisible by the stripe count";
      return std::nullopt;
    }

    DmTarget target;
    target.start_sector = *start_sector;
    target.length_sectors = *length_sector;
    target.stripe_count = stripes;
    target.type = stripes > 1 ? "striped" : "linear";
    target.chunk_sectors = stripes > 1 ? seg.stripe_size_sectors : 0;

    if (stripes > 1 && target.chunk_sectors == 0) {
      error = "striped segment has no stripe size";
      return std::nullopt;
    }

    // Each stripe contributes an equal share of the segment.
    const std::uint64_t per_stripe_sectors = *length_sector / stripes;

    for (const auto& stripe : seg.stripes) {
      const PhysicalVolume* pv = vg.find_pv(stripe.target);
      if (!pv) {
        error = "segment references unknown physical volume '" + stripe.target + "'";
        return std::nullopt;
      }
      if (pv->is_missing()) {
        error = "segment references missing physical volume '" + stripe.target + "'";
        return std::nullopt;
      }
      const auto pv_offset_extents = extents_to_sectors(stripe.start_extent, vg.extent_size_sectors);
      if (!pv_offset_extents) {
        error = "stripe offset arithmetic overflows";
        return std::nullopt;
      }
      const auto absolute = checked_add(pv->pe_start_sectors, *pv_offset_extents);
      if (!absolute) {
        error = "stripe offset exceeds addressable range";
        return std::nullopt;
      }

      // The mapped region must lie inside the PV's extent area.
      const auto pv_capacity = extents_to_sectors(pv->pe_count, vg.extent_size_sectors);
      if (!pv_capacity) {
        error = "physical volume capacity arithmetic overflows";
        return std::nullopt;
      }
      const auto pv_end = checked_add(pv->pe_start_sectors, *pv_capacity);
      if (!pv_end || !range_fits(*absolute, per_stripe_sectors, *pv_end)) {
        error = "segment maps outside physical volume '" + pv->key + "'";
        return std::nullopt;
      }

      target.destinations.push_back(DmDestination{pv->key, pv->id, *absolute});
    }

    table.push_back(std::move(target));
    expected_start = seg.start_extent + seg.extent_count;
  }

  if (table.empty()) {
    error = "logical volume has no mappable segments";
    return std::nullopt;
  }
  return table;
}

std::string format_dm_target(const DmTarget& target,
                             const std::vector<std::string>& device_names) {
  std::string out;
  out += std::to_string(target.start_sector);
  out += ' ';
  out += std::to_string(target.length_sectors);
  out += ' ';
  out += target.type;

  if (target.type == "thin") {
    out += ' ' + target.pool_lv + ' ' + std::to_string(target.device_id);
    return out;
  }
  if (target.type == "snapshot") {
    out += ' ' + target.origin_lv + ' ' + target.cow_lv + " P " + std::to_string(target.chunk_sectors);
    return out;
  }
  if (target.type == "snapshot-origin") {
    out += ' ' + target.origin_lv;
    return out;
  }
  if (target.type == "raid1" || target.type == "mirror") {
    out += " region_size " + std::to_string(target.region_size_sectors);
    for (const auto& sub : target.sub_lvs) out += ' ' + sub;
    return out;
  }

  if (target.type == "striped") {
    out += ' ';
    out += std::to_string(target.stripe_count);
    out += ' ';
    out += std::to_string(target.chunk_sectors);
  }

  for (std::size_t i = 0; i < target.destinations.size(); ++i) {
    out += ' ';
    out += (i < device_names.size()) ? device_names[i] : target.destinations[i].pv_key;
    out += ' ';
    out += std::to_string(target.destinations[i].offset_sectors);
  }
  return out;
}

}  // namespace lvmfmt
