#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "cmd_common.h"
#include "lvmfmt/bytes.h"
#include "lvmfmt/metadata.h"

namespace lvmfmt {

inline bool lv_byte_size_checked(const VolumeGroup& vg,
                                 const LogicalVolume& lv,
                                 std::uint64_t& out_bytes,
                                 std::string& error) {
  auto ext_bytes = checked_mul(vg.extent_size_sectors, 512ULL);
  if (!ext_bytes || *ext_bytes == 0) {
    error = "volume group extent size overflow or zero";
    return false;
  }
  auto total = checked_mul(lv.extent_count(), *ext_bytes);
  if (!total) {
    error = "logical volume byte size overflow on LV '" + lv.name + "'";
    return false;
  }
  out_bytes = *total;
  return true;
}

inline ByteSource* find_pv_storage(const LoadedVG& loaded, const PhysicalVolume& pv) {
  const std::string target = normalize_uuid_raw32(pv.id);
  for (std::size_t i = 0; i < loaded.pv_labels.size(); ++i) {
    if (normalize_uuid_raw32(loaded.pv_labels[i].pv_uuid) == target) {
      return loaded.storages[i].get();
    }
  }
  return nullptr;
}

inline bool map_lv_byte_chunk(const LoadedVG& loaded,
                              const LogicalVolume& lv,
                              std::uint64_t lv_byte_offset,
                              std::uint64_t max_len,
                              ByteSource*& out_storage,
                              std::uint64_t& out_pv_offset,
                              std::uint64_t& out_chunk_len,
                              std::string& error) {
  auto ext_bytes_opt = checked_mul(loaded.vg.extent_size_sectors, 512ULL);
  if (!ext_bytes_opt || *ext_bytes_opt == 0) {
    error = "volume group extent size is zero or overflows";
    return false;
  }
  const std::uint64_t ext_bytes = *ext_bytes_opt;

  for (const auto& seg : lv.segments) {
    if (seg.type != "linear" && seg.type != "striped") {
      error = "cannot perform raw extent I/O on composite segment type '" + seg.type + "'";
      return false;
    }
    auto seg_start_opt = checked_mul(seg.start_extent, ext_bytes);
    auto seg_len_opt = checked_mul(seg.extent_count, ext_bytes);
    if (!seg_start_opt || !seg_len_opt) {
      error = "segment extent byte offset overflow on LV '" + lv.name + "'";
      return false;
    }
    auto seg_end_opt = checked_add(*seg_start_opt, *seg_len_opt);
    if (!seg_end_opt) {
      error = "segment byte range overflow on LV '" + lv.name + "'";
      return false;
    }
    const std::uint64_t seg_start_b = *seg_start_opt;
    const std::uint64_t seg_len_b = *seg_len_opt;
    if (lv_byte_offset < seg_start_b || lv_byte_offset >= *seg_end_opt) {
      continue;
    }

    const std::uint64_t rel = lv_byte_offset - seg_start_b;
    const std::uint64_t sc = (seg.stripe_count == 0) ? 1ULL : seg.stripe_count;
    if (seg.stripes.size() != sc) {
      error = "segment stripe list mismatch on LV '" + lv.name + "'";
      return false;
    }

    if (sc == 1) {
      const auto& st = seg.stripes[0];
      const PhysicalVolume* pv = loaded.vg.find_pv(st.target);
      if (!pv) {
        error = "PV '" + st.target + "' not found in VG";
        return false;
      }
      ByteSource* src = find_pv_storage(loaded, *pv);
      if (!src) {
        error = "backing storage for PV '" + st.target + "' is not open";
        return false;
      }
      auto pe_start_b = checked_mul(pv->pe_start_sectors, 512ULL);
      auto st_ext_b = checked_mul(st.start_extent, ext_bytes);
      if (!pe_start_b || !st_ext_b) {
        error = "physical volume offset overflow on PV '" + pv->key + "'";
        return false;
      }
      auto base_b = checked_add(*pe_start_b, *st_ext_b);
      if (!base_b) {
        error = "physical volume base offset overflow on PV '" + pv->key + "'";
        return false;
      }
      auto pv_off = checked_add(*base_b, rel);
      if (!pv_off) {
        error = "physical volume byte offset overflow on PV '" + pv->key + "'";
        return false;
      }
      out_storage = src;
      out_pv_offset = *pv_off;
      out_chunk_len = std::min<std::uint64_t>(max_len, seg_len_b - rel);
      return true;
    }

    auto stripe_bytes_opt = checked_mul(seg.stripe_size_sectors, 512ULL);
    if (!stripe_bytes_opt || *stripe_bytes_opt == 0) {
      error = "striped segment has invalid stripe size on LV '" + lv.name + "'";
      return false;
    }
    const std::uint64_t stripe_bytes = *stripe_bytes_opt;
    const std::uint64_t chunk_idx = rel / stripe_bytes;
    const std::uint64_t within = rel % stripe_bytes;
    const std::uint64_t stripe_idx = chunk_idx % sc;
    const std::uint64_t stripe_row = chunk_idx / sc;

    const auto& st = seg.stripes[static_cast<std::size_t>(stripe_idx)];
    const PhysicalVolume* pv = loaded.vg.find_pv(st.target);
    if (!pv) {
      error = "PV '" + st.target + "' not found in VG";
      return false;
    }
    ByteSource* src = find_pv_storage(loaded, *pv);
    if (!src) {
      error = "backing storage for PV '" + st.target + "' is not open";
      return false;
    }
    auto pe_start_b = checked_mul(pv->pe_start_sectors, 512ULL);
    auto st_ext_b = checked_mul(st.start_extent, ext_bytes);
    auto row_b = checked_mul(stripe_row, stripe_bytes);
    if (!pe_start_b || !st_ext_b || !row_b) {
      error = "striped physical offset overflow on PV '" + pv->key + "'";
      return false;
    }
    auto base_b = checked_add(*pe_start_b, *st_ext_b);
    auto row_off = base_b ? checked_add(*base_b, *row_b) : std::nullopt;
    auto pv_off = row_off ? checked_add(*row_off, within) : std::nullopt;
    if (!pv_off) {
      error = "striped byte offset overflow on PV '" + pv->key + "'";
      return false;
    }
    out_storage = src;
    out_pv_offset = *pv_off;
    const std::uint64_t avail_in_stripe = stripe_bytes - within;
    const std::uint64_t avail_in_seg = seg_len_b - rel;
    out_chunk_len = std::min<std::uint64_t>(max_len, std::min(avail_in_stripe, avail_in_seg));
    return true;
  }

  error = "offset " + std::to_string(lv_byte_offset) + " out of bounds for LV '" + lv.name + "'";
  return false;
}

inline bool read_lv_bytes(const LoadedVG& loaded,
                          const LogicalVolume& lv,
                          std::uint64_t lv_byte_offset,
                          void* dst,
                          std::size_t len,
                          std::string& error) {
  auto* out = static_cast<std::uint8_t*>(dst);
  std::uint64_t cur = lv_byte_offset;
  std::size_t rem = len;
  while (rem > 0) {
    ByteSource* storage = nullptr;
    std::uint64_t pv_off = 0;
    std::uint64_t chunk = 0;
    if (!map_lv_byte_chunk(loaded, lv, cur, rem, storage, pv_off, chunk, error)) return false;
    if (!storage->read(pv_off, out, static_cast<std::size_t>(chunk), error)) {
      error = "read failed on LV '" + lv.name + "': " + error;
      return false;
    }
    out += chunk;
    cur += chunk;
    rem -= static_cast<std::size_t>(chunk);
  }
  return true;
}

inline bool write_lv_bytes(const LoadedVG& loaded,
                           const LogicalVolume& lv,
                           std::uint64_t lv_byte_offset,
                           const void* src,
                           std::size_t len,
                           std::string& error) {
  const auto* in = static_cast<const std::uint8_t*>(src);
  std::uint64_t cur = lv_byte_offset;
  std::size_t rem = len;
  while (rem > 0) {
    ByteSource* storage = nullptr;
    std::uint64_t pv_off = 0;
    std::uint64_t chunk = 0;
    if (!map_lv_byte_chunk(loaded, lv, cur, rem, storage, pv_off, chunk, error)) return false;
    if (!storage->write(pv_off, in, static_cast<std::size_t>(chunk), error)) {
      error = "write failed on LV '" + lv.name + "': " + error;
      return false;
    }
    in += chunk;
    cur += chunk;
    rem -= static_cast<std::size_t>(chunk);
  }
  return true;
}

inline bool copy_lv_bytes(const LoadedVG& loaded,
                          const LogicalVolume& src_lv,
                          const LogicalVolume& dst_lv,
                          std::uint64_t byte_len,
                          std::string& error) {
  std::vector<std::uint8_t> buf(64 * 1024);
  std::uint64_t off = 0;
  while (off < byte_len) {
    const std::size_t step =
        static_cast<std::size_t>(std::min<std::uint64_t>(byte_len - off, buf.size()));
    if (!read_lv_bytes(loaded, src_lv, off, buf.data(), step, error)) return false;
    if (!write_lv_bytes(loaded, dst_lv, off, buf.data(), step, error)) return false;
    off += step;
  }
  return true;
}

}  // namespace lvmfmt
