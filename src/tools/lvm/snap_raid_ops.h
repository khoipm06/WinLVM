#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "lv_io.h"
#include "lvmfmt/bytes.h"

namespace lvmfmt {

namespace snap_raid_detail {

inline constexpr std::uint32_t kSnapMagic = 0x70416e53u;      // "SnAp" LE
inline constexpr std::uint32_t kSnapVersion = 1u;
inline constexpr std::uint32_t kSnapMinChunkSectors = 8u;     // 4 KiB
inline constexpr std::uint32_t kSnapMaxChunkSectors = 2048u;  // 1 MiB
inline constexpr std::uint32_t kDmRaidMagic = 0x64526d44u;    // "DmRd" LE
inline constexpr std::uint32_t kMdBitmapMagic = 0x6d746962u;  // "bitm" LE

inline void put32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xffu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xffu);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xffu);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xffu);
}

inline void put64(std::uint8_t* p, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<std::uint8_t>((v >> (i * 8)) & 0xffu);
  }
}

}  // namespace snap_raid_detail

inline bool init_snapshot_cow_header(const LoadedVG& loaded,
                                     const LogicalVolume& cow_lv,
                                     std::uint32_t chunk_size_sectors,
                                     std::string& error) {
  using namespace snap_raid_detail;
  if (chunk_size_sectors < kSnapMinChunkSectors || chunk_size_sectors > kSnapMaxChunkSectors ||
      (chunk_size_sectors & (chunk_size_sectors - 1u)) != 0u) {
    error = "snapshot chunk size must be a power of 2 between 4 KiB and 1 MiB";
    return false;
  }
  auto chunk_bytes_opt = checked_mul(static_cast<std::uint64_t>(chunk_size_sectors), 512ULL);
  if (!chunk_bytes_opt) {
    error = "snapshot chunk size arithmetic overflow";
    return false;
  }
  const std::uint64_t chunk_bytes = *chunk_bytes_opt;
  std::uint64_t cow_bytes = 0;
  if (!lv_byte_size_checked(loaded.vg, cow_lv, cow_bytes, error)) return false;

  auto two_chunks = checked_mul(chunk_bytes, 2ULL);
  if (!two_chunks || cow_bytes < *two_chunks) {
    error = "snapshot COW volume is too small for header and first exception area";
    return false;
  }

  // Write chunk 0 (disk_header) + chunk 1 (zeroed first exception area).
  std::vector<std::uint8_t> init_buf(static_cast<std::size_t>(*two_chunks), 0);
  put32(init_buf.data() + 0, kSnapMagic);
  put32(init_buf.data() + 4, 1u);  // valid = 1
  put32(init_buf.data() + 8, kSnapVersion);
  put32(init_buf.data() + 12, chunk_size_sectors);

  return write_lv_bytes(loaded, cow_lv, 0, init_buf.data(), init_buf.size(), error);
}

inline bool merge_snapshot_cow_to_origin(const LoadedVG& loaded,
                                         const LogicalVolume& origin_lv,
                                         const LogicalVolume& cow_lv,
                                         std::uint64_t& merged_chunks_out,
                                         std::string& error) {
  using namespace snap_raid_detail;
  merged_chunks_out = 0;

  std::uint8_t hdr[512]{};
  if (!read_lv_bytes(loaded, cow_lv, 0, hdr, sizeof(hdr), error)) return false;

  const std::uint32_t magic = le32(hdr + 0);
  const std::uint32_t valid = le32(hdr + 4);
  const std::uint32_t version = le32(hdr + 8);
  const std::uint32_t chunk_sectors = le32(hdr + 12);

  if (magic != kSnapMagic) {
    error = "snapshot COW header magic mismatch";
    return false;
  }
  if (valid != 1u) {
    error = "snapshot COW store is marked invalid; refusing to merge";
    return false;
  }
  if (version != kSnapVersion) {
    error = "unsupported snapshot COW header version " + std::to_string(version);
    return false;
  }
  if (chunk_sectors < kSnapMinChunkSectors || chunk_sectors > kSnapMaxChunkSectors ||
      (chunk_sectors & (chunk_sectors - 1u)) != 0u) {
    error = "invalid snapshot chunk size in COW header: " + std::to_string(chunk_sectors);
    return false;
  }

  auto chunk_bytes_opt = checked_mul(static_cast<std::uint64_t>(chunk_sectors), 512ULL);
  if (!chunk_bytes_opt) {
    error = "snapshot chunk size arithmetic overflow";
    return false;
  }
  const std::uint64_t chunk_bytes = *chunk_bytes_opt;

  std::uint64_t origin_bytes = 0;
  std::uint64_t cow_bytes = 0;
  if (!lv_byte_size_checked(loaded.vg, origin_lv, origin_bytes, error)) return false;
  if (!lv_byte_size_checked(loaded.vg, cow_lv, cow_bytes, error)) return false;

  const std::uint64_t origin_chunks = origin_bytes / chunk_bytes;
  const std::uint64_t cow_chunks = cow_bytes / chunk_bytes;
  const std::uint64_t exceptions_per_area = chunk_bytes / 16ULL;
  const std::uint64_t area_stride = exceptions_per_area + 1ULL;

  // Phase 1: Validate and collect all (old_chunk, new_chunk) pairs before mutating origin.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> exceptions;
  std::vector<std::uint8_t> area_buf(static_cast<std::size_t>(chunk_bytes), 0);

  for (std::uint64_t area = 0; ; ++area) {
    auto area_chunks = checked_mul(area, area_stride);
    auto meta_chunk_opt = area_chunks ? checked_add(1ULL, *area_chunks) : std::nullopt;
    if (!meta_chunk_opt || *meta_chunk_opt >= cow_chunks) break;
    auto meta_off_opt = checked_mul(*meta_chunk_opt, chunk_bytes);
    if (!meta_off_opt || !range_fits(*meta_off_opt, chunk_bytes, cow_bytes)) break;

    if (!read_lv_bytes(loaded, cow_lv, *meta_off_opt, area_buf.data(), area_buf.size(), error)) {
      return false;
    }

    bool done = false;
    for (std::uint64_t i = 0; i < exceptions_per_area; ++i) {
      const std::uint64_t old_chunk = le64(area_buf.data() + static_cast<std::size_t>(i * 16ULL));
      const std::uint64_t new_chunk =
          le64(area_buf.data() + static_cast<std::size_t>(i * 16ULL + 8ULL));
      if (new_chunk == 0ULL) {
        done = true;
        break;
      }
      // Check chunk indices BEFORE multiplying by chunk_bytes to prevent 64-bit wraparound.
      if (old_chunk >= origin_chunks) {
        error = "snapshot exception old_chunk " + std::to_string(old_chunk) +
                " exceeds origin LV bounds (" + std::to_string(origin_chunks) + " chunks)";
        return false;
      }
      if (new_chunk < 2ULL || new_chunk >= cow_chunks) {
        error = "snapshot exception new_chunk " + std::to_string(new_chunk) +
                " is out of COW data chunk bounds (" + std::to_string(cow_chunks) + " chunks)";
        return false;
      }
      if ((new_chunk - 1ULL) % area_stride == 0ULL) {
        error = "snapshot exception new_chunk " + std::to_string(new_chunk) +
                " collides with a COW exception metadata chunk";
        return false;
      }
      exceptions.emplace_back(old_chunk, new_chunk);
    }
    if (done) break;
  }

  // Phase 2: Copy each validated exception chunk back into the origin LV.
  std::vector<std::uint8_t> chunk_buf(static_cast<std::size_t>(chunk_bytes), 0);
  for (const auto& [old_chunk, new_chunk] : exceptions) {
    auto src_off = checked_mul(new_chunk, chunk_bytes);
    auto dst_off = checked_mul(old_chunk, chunk_bytes);
    if (!src_off || !dst_off || !range_fits(*src_off, chunk_bytes, cow_bytes) ||
        !range_fits(*dst_off, chunk_bytes, origin_bytes)) {
      error = "snapshot exception offset arithmetic overflow";
      return false;
    }
    if (!read_lv_bytes(loaded, cow_lv, *src_off, chunk_buf.data(), chunk_buf.size(), error)) {
      return false;
    }
    if (!write_lv_bytes(loaded, origin_lv, *dst_off, chunk_buf.data(), chunk_buf.size(), error)) {
      return false;
    }
    merged_chunks_out++;
  }
  return true;
}

struct DmRaid1SuperblockInfo {
  std::uint64_t events = 0;
  std::uint64_t array_sectors = 0;
  std::uint32_t array_position = 0;
  std::uint32_t num_devices = 0;
};

inline bool validate_dm_raid1_superblock(const LoadedVG& loaded,
                                         const LogicalVolume& rmeta_lv,
                                         std::uint32_t expected_pos,
                                         std::uint32_t expected_num_devices,
                                         std::uint64_t expected_array_sectors,
                                         DmRaid1SuperblockInfo& out_info,
                                         std::string& error) {
  using namespace snap_raid_detail;

  std::uint64_t rmeta_bytes = 0;
  if (!lv_byte_size_checked(loaded.vg, rmeta_lv, rmeta_bytes, error)) return false;
  if (rmeta_bytes < 4096ULL) {
    error = "RAID metadata sub-volume '" + rmeta_lv.name + "' is too small";
    return false;
  }

  std::vector<std::uint8_t> buf(4096, 0);
  if (!read_lv_bytes(loaded, rmeta_lv, 0, buf.data(), buf.size(), error)) return false;

  const std::uint32_t magic = le32(buf.data() + 0x00);
  const std::uint32_t num_devices = le32(buf.data() + 0x08);
  const std::uint32_t array_pos = le32(buf.data() + 0x0c);
  const std::uint64_t events = le64(buf.data() + 0x10);
  const std::uint64_t failed_devices = le64(buf.data() + 0x18);
  const std::uint64_t disk_recovery_off = le64(buf.data() + 0x20);
  const std::uint64_t array_resync_off = le64(buf.data() + 0x28);
  const std::uint32_t level = le32(buf.data() + 0x30);
  const std::uint64_t reshape_pos = le64(buf.data() + 0x40);
  const std::uint64_t array_sectors = le64(buf.data() + 0x58);
  const std::uint64_t ext_failed0 = le64(buf.data() + 0x78);
  const std::uint64_t ext_failed1 = le64(buf.data() + 0x80);
  const std::uint64_t ext_failed2 = le64(buf.data() + 0x88);

  if (magic != kDmRaidMagic) {
    error = "corrupt dm-raid superblock magic on '" + rmeta_lv.name + "'";
    return false;
  }
  if (level != 1u) {
    error = "dm-raid superblock on '" + rmeta_lv.name + "' is not RAID1 (level=" +
            std::to_string(level) + ")";
    return false;
  }
  if (num_devices != expected_num_devices || array_pos != expected_pos) {
    error = "dm-raid superblock geometry mismatch on '" + rmeta_lv.name + "'";
    return false;
  }
  if (failed_devices != 0ULL || ext_failed0 != 0ULL || ext_failed1 != 0ULL ||
      ext_failed2 != 0ULL) {
    error = "RAID1 array reports failed device(s) in '" + rmeta_lv.name +
            "'; refusing unsafe collapse to linear";
    return false;
  }
  if (reshape_pos != UINT64_MAX) {
    error = "RAID1 array has an active reshape in '" + rmeta_lv.name + "'";
    return false;
  }
  if (array_sectors != expected_array_sectors) {
    error = "dm-raid superblock array_sectors mismatch on '" + rmeta_lv.name + "'";
    return false;
  }
  if (disk_recovery_off != UINT64_MAX && disk_recovery_off < expected_array_sectors) {
    error = "RAID1 leg '" + rmeta_lv.name +
            "' is not fully recovered; refusing unsafe collapse to linear";
    return false;
  }
  if (array_resync_off != UINT64_MAX && array_resync_off < expected_array_sectors) {
    error = "RAID1 array resync incomplete in '" + rmeta_lv.name +
            "'; refusing unsafe collapse to linear";
    return false;
  }

  out_info.events = events;
  out_info.array_sectors = array_sectors;
  out_info.array_position = array_pos;
  out_info.num_devices = num_devices;
  return true;
}

inline bool write_dm_raid1_superblock(const LoadedVG& loaded,
                                      const LogicalVolume& rmeta_lv,
                                      std::uint32_t array_position,
                                      std::uint32_t num_devices,
                                      std::uint64_t array_sectors,
                                      std::uint32_t region_size_sectors,
                                      std::string& error) {
  using namespace snap_raid_detail;

  std::uint64_t rmeta_bytes = 0;
  if (!lv_byte_size_checked(loaded.vg, rmeta_lv, rmeta_bytes, error)) return false;
  if (rmeta_bytes < 8192ULL) {
    error = "RAID metadata sub-volume is too small";
    return false;
  }

  const std::size_t write_sz =
      static_cast<std::size_t>(std::min<std::uint64_t>(rmeta_bytes, 65536ULL));
  std::vector<std::uint8_t> buf(write_sz, 0);

  // Block 0 (offset 0): struct dm_raid_superblock
  put32(buf.data() + 0x00, kDmRaidMagic);
  put32(buf.data() + 0x04, 1u);
  put32(buf.data() + 0x08, num_devices);
  put32(buf.data() + 0x0c, array_position);
  put64(buf.data() + 0x10, 1ULL);
  put64(buf.data() + 0x18, 0ULL);
  put64(buf.data() + 0x20, UINT64_MAX);
  put64(buf.data() + 0x28, UINT64_MAX);
  put32(buf.data() + 0x30, 1u);
  put32(buf.data() + 0x34, 0u);
  put32(buf.data() + 0x38, 0u);
  put32(buf.data() + 0x3c, 0u);
  put64(buf.data() + 0x40, UINT64_MAX);
  put32(buf.data() + 0x48, 1u);
  put32(buf.data() + 0x4c, 0u);
  put32(buf.data() + 0x50, 0u);
  put32(buf.data() + 0x54, 0u);
  put64(buf.data() + 0x58, array_sectors);
  put64(buf.data() + 0x60, 0ULL);
  put64(buf.data() + 0x68, 0ULL);
  put64(buf.data() + 0x70, array_sectors);

  // Block 1 (offset 4096): struct bitmap_super_t (v4, all bitmap bits 0 = clean/in-sync)
  std::uint8_t* bm = buf.data() + 4096;
  put32(bm + 0x00, kMdBitmapMagic);
  put32(bm + 0x04, 4u);
  put64(bm + 0x18, 1ULL);
  put64(bm + 0x20, 1ULL);
  put64(bm + 0x28, array_sectors);
  put32(bm + 0x30, 0u);
  put32(bm + 0x34, region_size_sectors * 512u);
  put32(bm + 0x38, 5u);

  return write_lv_bytes(loaded, rmeta_lv, 0, buf.data(), buf.size(), error);
}

}  // namespace lvmfmt
