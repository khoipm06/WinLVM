#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "lv_io.h"
#include "lvmfmt/bytes.h"

namespace lvmfmt {

namespace thin_detail {

inline constexpr std::uint32_t kThinBlockSize = 4096u;
inline constexpr std::uint64_t kThinSuperblockMagic = 27022010ULL;
inline constexpr std::uint32_t kThinSuperblockVersion = 2u;
inline constexpr std::uint32_t kThinSuperblockNeedsCheckFlag = 0x1u;
inline constexpr std::uint32_t kSuperblockCsumXor = 160774u;
inline constexpr std::uint32_t kBtreeCsumXor = 121107u;
inline constexpr std::uint32_t kBitmapCsumXor = 240779u;
inline constexpr std::uint32_t kIndexCsumXor = 160478u;

inline constexpr std::uint32_t kBtreeInternalNode = 1u;
inline constexpr std::uint32_t kBtreeLeafNode = 2u;
inline constexpr std::uint32_t kEntriesPerBitmapBlock = 16320u;  // (4096 - 16) * 4
inline constexpr std::uint32_t kMaxMetadataBitmaps = 255u;

inline void put_le32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xffu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xffu);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xffu);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xffu);
}

inline void put_le64(std::uint8_t* p, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<std::uint8_t>((v >> (i * 8)) & 0xffu);
  }
}

// Linux dm_bm_checksum: Castagnoli CRC32c (0x82f63b78 reflected), init 0xffffffff,
// no final complement, XORed with block-type salt.
inline std::uint32_t dm_bm_checksum(const std::uint8_t* block, std::uint32_t xor_salt) {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 4; i < kThinBlockSize; ++i) {
    crc ^= block[i];
    for (int b = 0; b < 8; ++b) {
      if (crc & 1u) {
        crc = (crc >> 1) ^ 0x82f63b78u;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc ^ xor_salt;
}

inline std::uint32_t btree_max_entries(std::uint32_t value_size) {
  const std::uint32_t raw = (kThinBlockSize - 32u) / (8u + value_size);
  return (raw / 3u) * 3u;
}

inline void init_btree_node(std::uint8_t* blk,
                            std::uint32_t flags,
                            std::uint64_t blocknr,
                            std::uint32_t nr_entries,
                            std::uint32_t value_size) {
  std::memset(blk, 0, kThinBlockSize);
  put_le32(blk + 4, flags);
  put_le64(blk + 8, blocknr);
  put_le32(blk + 16, nr_entries);
  put_le32(blk + 20, btree_max_entries(value_size));
  put_le32(blk + 24, value_size);
  put_le32(blk + 28, 0u);
}

inline void seal_block(std::uint8_t* blk, std::uint32_t xor_salt) {
  put_le32(blk, dm_bm_checksum(blk, xor_salt));
}

// dm-space-map-common 2-bit refcount encoding: 0 -> 0b00, 1 -> 0b10, 2 -> 0b01, 3 -> 0b11.
inline std::uint8_t get_bitmap_entry(const std::uint8_t* blk, std::uint32_t idx) {
  const std::uint8_t byte = blk[16u + (idx / 4u)];
  const std::uint8_t raw = static_cast<std::uint8_t>((byte >> ((idx % 4u) * 2u)) & 0x3u);
  if (raw == 2u) return 1u;
  if (raw == 1u) return 2u;
  return raw;
}

inline void set_bitmap_entry(std::uint8_t* blk, std::uint32_t idx, std::uint8_t refcount) {
  std::uint8_t raw = 0u;
  if (refcount == 1u) raw = 2u;
  else if (refcount == 2u) raw = 1u;
  else if (refcount >= 3u) raw = 3u;

  std::uint8_t& byte = blk[16u + (idx / 4u)];
  const std::uint32_t shift = (idx % 4u) * 2u;
  const std::uint32_t merged =
      (static_cast<std::uint32_t>(byte) & ~(0x3u << shift)) |
      (static_cast<std::uint32_t>(raw) << shift);
  byte = static_cast<std::uint8_t>(merged);
}

inline bool read_meta_block_raw(const LoadedVG& loaded,
                                const LogicalVolume& tmeta_lv,
                                std::uint64_t tmeta_bytes,
                                std::uint64_t blocknr,
                                std::vector<std::uint8_t>& out,
                                std::string& error) {
  auto byte_off = checked_mul(blocknr, static_cast<std::uint64_t>(kThinBlockSize));
  if (!byte_off || !range_fits(*byte_off, kThinBlockSize, tmeta_bytes)) {
    error = "thin-pool metadata block " + std::to_string(blocknr) + " lies outside tmeta bounds";
    return false;
  }
  out.assign(kThinBlockSize, 0);
  return read_lv_bytes(loaded, tmeta_lv, *byte_off, out.data(), kThinBlockSize, error);
}

inline bool write_meta_block_raw(const LoadedVG& loaded,
                                 const LogicalVolume& tmeta_lv,
                                 std::uint64_t tmeta_bytes,
                                 std::uint64_t blocknr,
                                 const std::uint8_t* data,
                                 std::string& error) {
  auto byte_off = checked_mul(blocknr, static_cast<std::uint64_t>(kThinBlockSize));
  if (!byte_off || !range_fits(*byte_off, kThinBlockSize, tmeta_bytes)) {
    error = "thin-pool metadata block " + std::to_string(blocknr) + " lies outside tmeta bounds";
    return false;
  }
  return write_lv_bytes(loaded, tmeta_lv, *byte_off, data, kThinBlockSize, error);
}

inline bool validate_btree_node(const std::uint8_t* node,
                                std::uint64_t expected_blocknr,
                                std::uint32_t expected_leaf_vsz,
                                std::uint32_t& flags_out,
                                std::uint32_t& nr_out,
                                std::uint32_t& max_ent_out,
                                std::uint32_t& vsz_out,
                                std::string& error) {
  if (le32(node) != dm_bm_checksum(node, kBtreeCsumXor)) {
    error = "thin-pool B-tree node checksum mismatch at block " + std::to_string(expected_blocknr);
    return false;
  }
  const std::uint32_t flags = le32(node + 4);
  const std::uint64_t bnr = le64(node + 8);
  const std::uint32_t nr = le32(node + 16);
  const std::uint32_t max_ent = le32(node + 20);
  const std::uint32_t vsz = le32(node + 24);

  if (bnr != expected_blocknr) {
    error = "thin-pool B-tree node blocknr mismatch at block " + std::to_string(expected_blocknr);
    return false;
  }
  if (flags != kBtreeLeafNode && flags != kBtreeInternalNode) {
    error = "invalid B-tree node flags at block " + std::to_string(expected_blocknr);
    return false;
  }
  if (max_ent == 0u || nr > max_ent) {
    error = "corrupt B-tree node entry counts at block " + std::to_string(expected_blocknr);
    return false;
  }
  if (flags == kBtreeInternalNode) {
    if (vsz != 8u || nr == 0u) {
      error = "corrupt B-tree internal node header at block " + std::to_string(expected_blocknr);
      return false;
    }
  } else {
    if (vsz != expected_leaf_vsz) {
      error = "unexpected B-tree leaf value size at block " + std::to_string(expected_blocknr);
      return false;
    }
  }
  auto stride = checked_add(8ULL, static_cast<std::uint64_t>(vsz));
  auto body = stride ? checked_mul(static_cast<std::uint64_t>(max_ent), *stride) : std::nullopt;
  auto total = body ? checked_add(32ULL, *body) : std::nullopt;
  if (!total || *total > kThinBlockSize) {
    error = "corrupt B-tree node geometry exceeds 4 KiB block at block " +
            std::to_string(expected_blocknr);
    return false;
  }

  flags_out = flags;
  nr_out = nr;
  max_ent_out = max_ent;
  vsz_out = vsz;
  return true;
}

struct ThinMetaEditor {
  const LoadedVG& loaded;
  const LogicalVolume& tmeta_lv;
  std::uint64_t tmeta_bytes = 0;
  std::vector<std::uint8_t> sb{};
  std::uint64_t dsm_nr_blocks = 0;
  std::uint64_t dsm_nr_allocated = 0;
  std::uint64_t dsm_bitmap_root = 0;
  std::uint64_t dsm_ref_count_root = 0;
  std::uint64_t msm_nr_blocks = 0;
  std::uint64_t msm_nr_allocated = 0;
  std::uint64_t msm_bitmap_root = 0;
  std::uint64_t msm_ref_count_root = 0;
  // All mutations are staged in memory until flush_staged() commits non-superblock
  // blocks first and superblock block 0 last.
  std::map<std::uint64_t, std::vector<std::uint8_t>> staged{};

  bool read_block(std::uint64_t blocknr, std::vector<std::uint8_t>& out, std::string& error) {
    if (msm_nr_blocks > 0 && blocknr >= msm_nr_blocks) {
      error = "thin-pool metadata block " + std::to_string(blocknr) + " exceeds metadata_nr_blocks";
      return false;
    }
    auto it = staged.find(blocknr);
    if (it != staged.end()) {
      out = it->second;
      return true;
    }
    return read_meta_block_raw(loaded, tmeta_lv, tmeta_bytes, blocknr, out, error);
  }

  bool stage_block(std::uint64_t blocknr, const std::vector<std::uint8_t>& data, std::string& error) {
    if (data.size() != kThinBlockSize) {
      error = "internal error: invalid staged thin metadata block size";
      return false;
    }
    if (msm_nr_blocks > 0 && blocknr >= msm_nr_blocks) {
      error = "thin-pool metadata block " + std::to_string(blocknr) + " exceeds metadata_nr_blocks";
      return false;
    }
    auto byte_off = checked_mul(blocknr, static_cast<std::uint64_t>(kThinBlockSize));
    if (!byte_off || !range_fits(*byte_off, kThinBlockSize, tmeta_bytes)) {
      error = "thin-pool metadata block " + std::to_string(blocknr) + " exceeds tmeta bounds";
      return false;
    }
    staged[blocknr] = data;
    return true;
  }

  bool flush_staged(std::string& error) {
    // Write all non-superblock blocks first, then publish block 0 superblock last.
    for (const auto& kv : staged) {
      if (kv.first == 0ULL) continue;
      if (!write_meta_block_raw(loaded, tmeta_lv, tmeta_bytes, kv.first, kv.second.data(), error)) {
        return false;
      }
    }
    auto it0 = staged.find(0ULL);
    if (it0 != staged.end()) {
      if (!write_meta_block_raw(loaded, tmeta_lv, tmeta_bytes, 0ULL, it0->second.data(), error)) {
        return false;
      }
    }
    staged.clear();
    return true;
  }

  bool load_superblock(std::optional<std::uint64_t> expected_vg_trans_id,
                       bool require_clean_for_write,
                       std::string& error) {
    if (!lv_byte_size_checked(loaded.vg, tmeta_lv, tmeta_bytes, error)) return false;
    if (tmeta_bytes < static_cast<std::uint64_t>(kThinBlockSize) * 16ULL) {
      error = "thin-pool metadata volume is smaller than 64 KiB";
      return false;
    }
    if (!read_meta_block_raw(loaded, tmeta_lv, tmeta_bytes, 0ULL, sb, error)) return false;
    if (le32(sb.data()) != dm_bm_checksum(sb.data(), kSuperblockCsumXor)) {
      error = "thin-pool metadata superblock checksum mismatch";
      return false;
    }
    if (le64(sb.data() + 32) != kThinSuperblockMagic) {
      error = "thin-pool metadata superblock magic mismatch";
      return false;
    }
    if (le32(sb.data() + 40) != kThinSuperblockVersion) {
      error = "unsupported thin-pool metadata version (expected v2)";
      return false;
    }
    if (le64(sb.data() + 8) != 0ULL) {
      error = "thin-pool superblock has unexpected blocknr";
      return false;
    }

    const std::uint32_t sb_flags = le32(sb.data() + 4);
    const std::uint64_t sb_trans = le64(sb.data() + 48);
    const std::uint64_t held_root = le64(sb.data() + 56);
    if (require_clean_for_write) {
      if ((sb_flags & kThinSuperblockNeedsCheckFlag) != 0u || sb_flags != 0u) {
        error = "thin-pool metadata has NEEDS_CHECK or unsupported superblock flags (" +
                std::to_string(sb_flags) + "); run thin_check/thin_repair first";
        return false;
      }
      if (held_root != 0ULL) {
        error = "thin-pool metadata has an active held metadata snapshot (held_root != 0); "
                "refusing offline modification";
        return false;
      }
      if (expected_vg_trans_id && sb_trans != *expected_vg_trans_id) {
        error = "thin-pool transaction_id mismatch (VG metadata=" +
                std::to_string(*expected_vg_trans_id) + ", on-disk tmeta=" +
                std::to_string(sb_trans) + ")";
        return false;
      }
    }

    dsm_nr_blocks = le64(sb.data() + 64);
    dsm_nr_allocated = le64(sb.data() + 72);
    dsm_bitmap_root = le64(sb.data() + 80);
    dsm_ref_count_root = le64(sb.data() + 88);

    msm_nr_blocks = le64(sb.data() + 192);
    msm_nr_allocated = le64(sb.data() + 200);
    msm_bitmap_root = le64(sb.data() + 208);
    msm_ref_count_root = le64(sb.data() + 216);

    const std::uint64_t max_physical_blocks = tmeta_bytes / kThinBlockSize;
    if (msm_nr_blocks < 16ULL || msm_nr_blocks > max_physical_blocks) {
      error = "invalid metadata_nr_blocks (" + std::to_string(msm_nr_blocks) +
              ") in thin-pool superblock";
      return false;
    }
    const std::uint64_t data_map_root = le64(sb.data() + 320);
    const std::uint64_t dev_det_root = le64(sb.data() + 328);
    if (msm_bitmap_root >= msm_nr_blocks || dsm_bitmap_root >= msm_nr_blocks ||
        data_map_root >= msm_nr_blocks || dev_det_root >= msm_nr_blocks) {
      error = "thin-pool superblock root pointer exceeds metadata_nr_blocks";
      return false;
    }
    return true;
  }

  bool alloc_meta_block(std::uint64_t& out_blk, std::string& error) {
    std::vector<std::uint8_t> idx_blk;
    if (!read_block(msm_bitmap_root, idx_blk, error)) return false;
    if (le32(idx_blk.data()) != dm_bm_checksum(idx_blk.data(), kIndexCsumXor) ||
        le64(idx_blk.data() + 8) != msm_bitmap_root) {
      error = "thin-pool metadata space map index checksum/blocknr mismatch";
      return false;
    }

    const std::uint64_t n_bitmaps =
        (msm_nr_blocks + kEntriesPerBitmapBlock - 1ULL) / kEntriesPerBitmapBlock;
    if (n_bitmaps == 0 || n_bitmaps > kMaxMetadataBitmaps) {
      error = "thin-pool metadata bitmap count out of range";
      return false;
    }

    for (std::uint64_t bi = 0; bi < n_bitmaps; ++bi) {
      const std::size_t eoff = 16u + static_cast<std::size_t>(bi) * 16u;
      const std::uint64_t bm_blocknr = le64(idx_blk.data() + eoff);
      const std::uint32_t nr_free = le32(idx_blk.data() + eoff + 8);
      if (nr_free == 0) continue;
      if (bm_blocknr >= msm_nr_blocks) {
        error = "thin-pool metadata bitmap blocknr out of range";
        return false;
      }

      std::vector<std::uint8_t> bm_blk;
      if (!read_block(bm_blocknr, bm_blk, error)) return false;
      if (le32(bm_blk.data()) != dm_bm_checksum(bm_blk.data(), kBitmapCsumXor) ||
          le64(bm_blk.data() + 8) != bm_blocknr) {
        error = "thin-pool metadata bitmap checksum/blocknr mismatch";
        return false;
      }

      const std::uint64_t base_blk = bi * kEntriesPerBitmapBlock;
      const std::uint32_t limit = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(kEntriesPerBitmapBlock, msm_nr_blocks - base_blk));
      for (std::uint32_t slot = 0; slot < limit; ++slot) {
        if (get_bitmap_entry(bm_blk.data(), slot) == 0u) {
          set_bitmap_entry(bm_blk.data(), slot, 1u);
          seal_block(bm_blk.data(), kBitmapCsumXor);
          if (!stage_block(bm_blocknr, bm_blk, error)) return false;

          put_le32(idx_blk.data() + eoff + 8, nr_free - 1u);
          put_le32(idx_blk.data() + eoff + 12, slot + 1u);
          seal_block(idx_blk.data(), kIndexCsumXor);
          if (!stage_block(msm_bitmap_root, idx_blk, error)) return false;

          msm_nr_allocated++;
          put_le64(sb.data() + 200, msm_nr_allocated);
          out_blk = base_blk + slot;
          return true;
        }
      }
    }

    error = "thin-pool metadata space map is full";
    return false;
  }

  bool free_meta_block(std::uint64_t blk, std::string& error) {
    if (blk == 0ULL || blk >= msm_nr_blocks) {
      error = "cannot free invalid thin-pool metadata block " + std::to_string(blk);
      return false;
    }
    std::vector<std::uint8_t> idx_blk;
    if (!read_block(msm_bitmap_root, idx_blk, error)) return false;
    if (le32(idx_blk.data()) != dm_bm_checksum(idx_blk.data(), kIndexCsumXor) ||
        le64(idx_blk.data() + 8) != msm_bitmap_root) {
      error = "thin-pool metadata space map index checksum/blocknr mismatch";
      return false;
    }
    const std::uint64_t bi = blk / kEntriesPerBitmapBlock;
    const std::uint32_t slot = static_cast<std::uint32_t>(blk % kEntriesPerBitmapBlock);
    if (bi >= kMaxMetadataBitmaps) {
      error = "metadata block bitmap index out of range";
      return false;
    }
    const std::size_t eoff = 16u + static_cast<std::size_t>(bi) * 16u;
    const std::uint64_t bm_blocknr = le64(idx_blk.data() + eoff);
    const std::uint32_t nr_free = le32(idx_blk.data() + eoff + 8);
    const std::uint32_t none_free_before = le32(idx_blk.data() + eoff + 12);
    if (bm_blocknr >= msm_nr_blocks) {
      error = "thin-pool metadata bitmap blocknr out of range";
      return false;
    }

    std::vector<std::uint8_t> bm_blk;
    if (!read_block(bm_blocknr, bm_blk, error)) return false;
    if (le32(bm_blk.data()) != dm_bm_checksum(bm_blk.data(), kBitmapCsumXor) ||
        le64(bm_blk.data() + 8) != bm_blocknr) {
      error = "thin-pool metadata bitmap checksum/blocknr mismatch";
      return false;
    }

    const std::uint8_t rc = get_bitmap_entry(bm_blk.data(), slot);
    if (rc == 0u) {
      error = "metadata block " + std::to_string(blk) + " already has zero refcount";
      return false;
    }
    if (rc >= 3u) {
      error = "metadata block " + std::to_string(blk) +
              " has overflow refcount; refusing offline deletion";
      return false;
    }
    set_bitmap_entry(bm_blk.data(), slot, static_cast<std::uint8_t>(rc - 1u));
    seal_block(bm_blk.data(), kBitmapCsumXor);
    if (!stage_block(bm_blocknr, bm_blk, error)) return false;

    if (rc == 1u) {
      put_le32(idx_blk.data() + eoff + 8, nr_free + 1u);
      if (slot < none_free_before) {
        put_le32(idx_blk.data() + eoff + 12, slot);
      }
      seal_block(idx_blk.data(), kIndexCsumXor);
      if (!stage_block(msm_bitmap_root, idx_blk, error)) return false;
      if (msm_nr_allocated > 0) msm_nr_allocated--;
      put_le64(sb.data() + 200, msm_nr_allocated);
    }
    return true;
  }

  // Decrement a data block's refcount in the data space map (dsm).
  bool dsm_dec_refcount(std::uint64_t data_block, std::string& error) {
    if (data_block >= dsm_nr_blocks) {
      error = "thin mapping references out-of-range data block " + std::to_string(data_block);
      return false;
    }
    const std::uint64_t bi = data_block / kEntriesPerBitmapBlock;
    const std::uint32_t slot = static_cast<std::uint32_t>(data_block % kEntriesPerBitmapBlock);

    // Locate bitmap index `bi` inside dsm_bitmap_root B-tree (val_sz = 16).
    std::uint64_t cur_blk = dsm_bitmap_root;
    std::set<std::uint64_t> visited;
    for (int depth = 0; depth < 8; ++depth) {
      if (!visited.insert(cur_blk).second) {
        error = "cycle detected in data space map B-tree";
        return false;
      }
      std::vector<std::uint8_t> node;
      if (!read_block(cur_blk, node, error)) return false;
      std::uint32_t flags = 0, nr = 0, max_ent = 0, vsz = 0;
      if (!validate_btree_node(node.data(), cur_blk, 16u, flags, nr, max_ent, vsz, error)) {
        return false;
      }
      const std::size_t val_base = 32u + static_cast<std::size_t>(max_ent) * 8u;
      if (flags == kBtreeLeafNode) {
        std::size_t found = nr;
        for (std::size_t i = 0; i < nr; ++i) {
          if (le64(node.data() + 32u + i * 8u) == bi) {
            found = i;
            break;
          }
        }
        if (found >= nr) {
          error = "data space map missing bitmap index " + std::to_string(bi);
          return false;
        }
        const std::size_t voff = val_base + found * 16u;
        const std::uint64_t bm_blocknr = le64(node.data() + voff);
        const std::uint32_t nr_free = le32(node.data() + voff + 8);
        const std::uint32_t none_free_before = le32(node.data() + voff + 12);
        if (bm_blocknr >= msm_nr_blocks) {
          error = "data space map bitmap blocknr out of range";
          return false;
        }

        std::vector<std::uint8_t> bm_blk;
        if (!read_block(bm_blocknr, bm_blk, error)) return false;
        if (le32(bm_blk.data()) != dm_bm_checksum(bm_blk.data(), kBitmapCsumXor) ||
            le64(bm_blk.data() + 8) != bm_blocknr) {
          error = "data space map bitmap checksum/blocknr mismatch";
          return false;
        }

        const std::uint8_t rc = get_bitmap_entry(bm_blk.data(), slot);
        if (rc == 0u) {
          error = "data block " + std::to_string(data_block) + " already has zero refcount";
          return false;
        }
        if (rc >= 3u) {
          error = "data block " + std::to_string(data_block) +
                  " has overflow refcount (>= 3); refusing offline thin LV deletion";
          return false;
        }
        set_bitmap_entry(bm_blk.data(), slot, static_cast<std::uint8_t>(rc - 1u));
        seal_block(bm_blk.data(), kBitmapCsumXor);
        if (!stage_block(bm_blocknr, bm_blk, error)) return false;

        if (rc == 1u) {
          put_le32(node.data() + voff + 8, nr_free + 1u);
          if (slot < none_free_before) {
            put_le32(node.data() + voff + 12, slot);
          }
          seal_block(node.data(), kBtreeCsumXor);
          if (!stage_block(cur_blk, node, error)) return false;
          if (dsm_nr_allocated > 0) dsm_nr_allocated--;
          put_le64(sb.data() + 72, dsm_nr_allocated);
        }
        return true;
      }

      std::size_t cidx = 0;
      for (std::size_t i = 0; i < nr; ++i) {
        if (le64(node.data() + 32u + i * 8u) <= bi) cidx = i;
        else break;
      }
      cur_blk = le64(node.data() + val_base + cidx * 8u);
    }
    error = "data space map B-tree exceeded maximum depth";
    return false;
  }

  bool get_meta_block_refcount(std::uint64_t blk, std::uint8_t& rc_out, std::string& error) {
    if (blk == 0ULL || blk >= msm_nr_blocks) {
      error = "invalid thin-pool metadata block " + std::to_string(blk);
      return false;
    }
    std::vector<std::uint8_t> idx_blk;
    if (!read_block(msm_bitmap_root, idx_blk, error)) return false;
    if (le32(idx_blk.data()) != dm_bm_checksum(idx_blk.data(), kIndexCsumXor) ||
        le64(idx_blk.data() + 8) != msm_bitmap_root) {
      error = "thin-pool metadata space map index checksum/blocknr mismatch";
      return false;
    }
    const std::uint64_t bi = blk / kEntriesPerBitmapBlock;
    const std::uint32_t slot = static_cast<std::uint32_t>(blk % kEntriesPerBitmapBlock);
    if (bi >= kMaxMetadataBitmaps) {
      error = "metadata block bitmap index out of range";
      return false;
    }
    const std::size_t eoff = 16u + static_cast<std::size_t>(bi) * 16u;
    const std::uint64_t bm_blocknr = le64(idx_blk.data() + eoff);
    if (bm_blocknr >= msm_nr_blocks) {
      error = "thin-pool metadata bitmap blocknr out of range";
      return false;
    }
    std::vector<std::uint8_t> bm_blk;
    if (!read_block(bm_blocknr, bm_blk, error)) return false;
    if (le32(bm_blk.data()) != dm_bm_checksum(bm_blk.data(), kBitmapCsumXor) ||
        le64(bm_blk.data() + 8) != bm_blocknr) {
      error = "thin-pool metadata bitmap checksum/blocknr mismatch";
      return false;
    }
    rc_out = get_bitmap_entry(bm_blk.data(), slot);
    return true;
  }

  // Recursively release a device's per-device mapping B-tree while honoring
  // dm-thin lazy refcounting on shared B-tree nodes.
  bool free_device_mapping_subtree(std::uint64_t node_blk,
                                   int depth,
                                   std::set<std::uint64_t>& visited,
                                   std::string& error) {
    if (depth >= 8) {
      error = "device mapping B-tree exceeded maximum depth";
      return false;
    }
    if (!visited.insert(node_blk).second) {
      error = "cycle detected in device mapping B-tree";
      return false;
    }

    std::uint8_t rc = 0;
    if (!get_meta_block_refcount(node_blk, rc, error)) return false;
    if (rc == 0u) {
      error = "metadata block " + std::to_string(node_blk) + " already has zero refcount";
      return false;
    }
    if (rc >= 3u) {
      error = "metadata block " + std::to_string(node_blk) +
              " has overflow refcount; refusing offline deletion";
      return false;
    }

    std::vector<std::uint8_t> node;
    if (!read_block(node_blk, node, error)) return false;
    std::uint32_t flags = 0, nr = 0, max_ent = 0, vsz = 0;
    if (!validate_btree_node(node.data(), node_blk, 8u, flags, nr, max_ent, vsz, error)) {
      return false;
    }

    // Shared B-tree node (rc == 2): dm-thin uses lazy refcounting, so child nodes
    // and leaf data blocks remain owned by the surviving snapshot. Drop only this
    // node's reference (2 -> 1) without recursing or decrementing data refcounts.
    if (rc == 2u) {
      return free_meta_block(node_blk, error);
    }

    const std::size_t val_base = 32u + static_cast<std::size_t>(max_ent) * 8u;
    if (flags == kBtreeLeafNode) {
      for (std::size_t i = 0; i < nr; ++i) {
        const std::uint64_t packed = le64(node.data() + val_base + i * 8u);
        const std::uint64_t data_block = packed >> 24;
        if (!dsm_dec_refcount(data_block, error)) return false;
      }
    } else {
      for (std::size_t i = 0; i < nr; ++i) {
        const std::uint64_t child_blk = le64(node.data() + val_base + i * 8u);
        if (!free_device_mapping_subtree(child_blk, depth + 1, visited, error)) return false;
      }
    }
    return free_meta_block(node_blk, error);
  }

  // Insert (key, val[val_size]) into a B-tree rooted at root_blk_io (staged in memory).
  bool btree_insert(std::uint64_t& root_blk_io,
                    std::uint64_t key,
                    const std::uint8_t* val,
                    std::uint32_t val_size,
                    std::string& error) {
    struct PathStep {
      std::uint64_t blk = 0;
      std::size_t child_idx = 0;
    };
    std::vector<PathStep> path;
    std::set<std::uint64_t> visited;
    std::uint64_t cur_blk = root_blk_io;

    for (int depth = 0; depth < 8; ++depth) {
      if (!visited.insert(cur_blk).second) {
        error = "cycle detected in thin-pool B-tree";
        return false;
      }
      std::vector<std::uint8_t> node;
      if (!read_block(cur_blk, node, error)) return false;
      std::uint32_t flags = 0, nr = 0, max_ent = 0, vsz = 0;
      if (!validate_btree_node(node.data(), cur_blk, val_size, flags, nr, max_ent, vsz, error)) {
        return false;
      }
      const std::size_t val_base = 32u + static_cast<std::size_t>(max_ent) * 8u;

      if (flags == kBtreeLeafNode) {
        std::size_t pos = 0;
        while (pos < nr && le64(node.data() + 32u + pos * 8u) < key) ++pos;
        if (pos < nr && le64(node.data() + 32u + pos * 8u) == key) {
          error = "thin device_id " + std::to_string(key) + " already exists in thin-pool metadata";
          return false;
        }

        if (nr < max_ent) {
          for (std::size_t i = nr; i > pos; --i) {
            std::memcpy(node.data() + 32u + i * 8u, node.data() + 32u + (i - 1u) * 8u, 8u);
            std::memcpy(node.data() + val_base + i * vsz,
                        node.data() + val_base + (i - 1u) * vsz, vsz);
          }
          put_le64(node.data() + 32u + pos * 8u, key);
          std::memcpy(node.data() + val_base + pos * vsz, val, vsz);
          put_le32(node.data() + 16, nr + 1u);
          seal_block(node.data(), kBtreeCsumXor);
          return stage_block(cur_blk, node, error);
        }

        // Leaf is full: split root leaf (depth 0) into 1 internal root + 2 half-filled leaves.
        if (!path.empty()) {
          error = "thin-pool B-tree leaf full at depth > 0";
          return false;
        }
        std::uint64_t left_blk = 0;
        std::uint64_t right_blk = 0;
        if (!alloc_meta_block(left_blk, error)) return false;
        if (!alloc_meta_block(right_blk, error)) return false;

        std::vector<std::uint64_t> all_keys(nr + 1u);
        std::vector<std::uint8_t> all_vals((nr + 1u) * vsz);
        for (std::size_t i = 0; i < pos; ++i) {
          all_keys[i] = le64(node.data() + 32u + i * 8u);
          std::memcpy(&all_vals[i * vsz], node.data() + val_base + i * vsz, vsz);
        }
        all_keys[pos] = key;
        std::memcpy(&all_vals[pos * vsz], val, vsz);
        for (std::size_t i = pos; i < nr; ++i) {
          all_keys[i + 1u] = le64(node.data() + 32u + i * 8u);
          std::memcpy(&all_vals[(i + 1u) * vsz], node.data() + val_base + i * vsz, vsz);
        }

        const std::size_t mid = (nr + 1u) / 2u;
        const std::size_t right_cnt = (nr + 1u) - mid;

        std::vector<std::uint8_t> left(kThinBlockSize, 0);
        init_btree_node(left.data(), kBtreeLeafNode, left_blk,
                        static_cast<std::uint32_t>(mid), vsz);
        for (std::size_t i = 0; i < mid; ++i) {
          put_le64(left.data() + 32u + i * 8u, all_keys[i]);
          std::memcpy(left.data() + val_base + i * vsz, &all_vals[i * vsz], vsz);
        }
        seal_block(left.data(), kBtreeCsumXor);

        std::vector<std::uint8_t> right(kThinBlockSize, 0);
        init_btree_node(right.data(), kBtreeLeafNode, right_blk,
                        static_cast<std::uint32_t>(right_cnt), vsz);
        for (std::size_t i = 0; i < right_cnt; ++i) {
          put_le64(right.data() + 32u + i * 8u, all_keys[mid + i]);
          std::memcpy(right.data() + val_base + i * vsz, &all_vals[(mid + i) * vsz], vsz);
        }
        seal_block(right.data(), kBtreeCsumXor);

        std::vector<std::uint8_t> new_root(kThinBlockSize, 0);
        init_btree_node(new_root.data(), kBtreeInternalNode, cur_blk, 2u, 8u);
        const std::size_t root_val_base = 32u + static_cast<std::size_t>(btree_max_entries(8u)) * 8u;
        put_le64(new_root.data() + 32u, all_keys[0]);
        put_le64(new_root.data() + 40u, all_keys[mid]);
        put_le64(new_root.data() + root_val_base, left_blk);
        put_le64(new_root.data() + root_val_base + 8u, right_blk);
        seal_block(new_root.data(), kBtreeCsumXor);

        if (!stage_block(left_blk, left, error)) return false;
        if (!stage_block(right_blk, right, error)) return false;
        return stage_block(cur_blk, new_root, error);
      }

      std::size_t cidx = 0;
      for (std::size_t i = 0; i < nr; ++i) {
        if (le64(node.data() + 32u + i * 8u) <= key) cidx = i;
        else break;
      }
      const std::uint64_t child_blk = le64(node.data() + val_base + cidx * 8u);
      path.push_back({cur_blk, cidx});
      cur_blk = child_blk;
    }

    error = "thin-pool B-tree exceeded maximum depth";
    return false;
  }

  // Delete `key` from a B-tree rooted at root_blk_io, optionally returning its value bytes.
  bool btree_delete(std::uint64_t& root_blk_io,
                    std::uint64_t key,
                    std::uint32_t val_size,
                    std::vector<std::uint8_t>* removed_val_out,
                    std::string& error) {
    struct PathStep {
      std::uint64_t blk = 0;
      std::size_t child_idx = 0;
    };
    std::vector<PathStep> path;
    std::set<std::uint64_t> visited;
    std::uint64_t cur_blk = root_blk_io;

    for (int depth = 0; depth < 8; ++depth) {
      if (!visited.insert(cur_blk).second) {
        error = "cycle detected in thin-pool B-tree";
        return false;
      }
      std::vector<std::uint8_t> node;
      if (!read_block(cur_blk, node, error)) return false;
      std::uint32_t flags = 0, nr = 0, max_ent = 0, vsz = 0;
      if (!validate_btree_node(node.data(), cur_blk, val_size, flags, nr, max_ent, vsz, error)) {
        return false;
      }
      const std::size_t val_base = 32u + static_cast<std::size_t>(max_ent) * 8u;

      if (flags == kBtreeLeafNode) {
        std::size_t pos = nr;
        for (std::size_t i = 0; i < nr; ++i) {
          if (le64(node.data() + 32u + i * 8u) == key) {
            pos = i;
            break;
          }
        }
        if (pos >= nr) {
          error = "thin device_id " + std::to_string(key) + " not found in thin-pool metadata";
          return false;
        }
        if (removed_val_out) {
          removed_val_out->assign(node.data() + val_base + pos * vsz,
                                  node.data() + val_base + (pos + 1u) * vsz);
        }
        for (std::size_t i = pos; i + 1u < nr; ++i) {
          std::memcpy(node.data() + 32u + i * 8u, node.data() + 32u + (i + 1u) * 8u, 8u);
          std::memcpy(node.data() + val_base + i * vsz,
                      node.data() + val_base + (i + 1u) * vsz, vsz);
        }
        std::memset(node.data() + 32u + (nr - 1u) * 8u, 0, 8u);
        std::memset(node.data() + val_base + (nr - 1u) * vsz, 0, vsz);
        const std::uint32_t new_nr = nr - 1u;
        put_le32(node.data() + 16, new_nr);
        seal_block(node.data(), kBtreeCsumXor);

        if (new_nr > 0u || path.empty()) {
          if (!stage_block(cur_blk, node, error)) return false;
          // If first key of a non-root leaf changed, update parent's separator key.
          if (pos == 0u && new_nr > 0u && !path.empty()) {
            const std::uint64_t new_min = le64(node.data() + 32u);
            const auto& parent_step = path.back();
            std::vector<std::uint8_t> pnode;
            if (!read_block(parent_step.blk, pnode, error)) return false;
            put_le64(pnode.data() + 32u + parent_step.child_idx * 8u, new_min);
            seal_block(pnode.data(), kBtreeCsumXor);
            if (!stage_block(parent_step.blk, pnode, error)) return false;
          }
          return true;
        }

        // Child leaf became empty: free it and remove its entry from parent internal nodes.
        if (!free_meta_block(cur_blk, error)) return false;
        while (!path.empty()) {
          const auto step = path.back();
          path.pop_back();
          std::vector<std::uint8_t> pnode;
          if (!read_block(step.blk, pnode, error)) return false;
          std::uint32_t pf = 0, pnr = 0, pmax = 0, pvsz = 0;
          if (!validate_btree_node(pnode.data(), step.blk, val_size, pf, pnr, pmax, pvsz, error)) {
            return false;
          }
          const std::size_t pval_base = 32u + static_cast<std::size_t>(pmax) * 8u;
          for (std::size_t i = step.child_idx; i + 1u < pnr; ++i) {
            std::memcpy(pnode.data() + 32u + i * 8u, pnode.data() + 32u + (i + 1u) * 8u, 8u);
            std::memcpy(pnode.data() + pval_base + i * 8u,
                        pnode.data() + pval_base + (i + 1u) * 8u, 8u);
          }
          std::memset(pnode.data() + 32u + (pnr - 1u) * 8u, 0, 8u);
          std::memset(pnode.data() + pval_base + (pnr - 1u) * 8u, 0, 8u);
          const std::uint32_t new_pnr = pnr - 1u;
          if (new_pnr == 0u && !path.empty()) {
            if (!free_meta_block(step.blk, error)) return false;
            continue;
          }
          if (new_pnr == 0u && path.empty()) {
            // Root became completely empty: reinitialize as an empty leaf node.
            init_btree_node(pnode.data(), kBtreeLeafNode, step.blk, 0u, val_size);
            seal_block(pnode.data(), kBtreeCsumXor);
            return stage_block(step.blk, pnode, error);
          }
          if (new_pnr == 1u && path.empty()) {
            // Collapse single-child internal root directly to that remaining child.
            const std::uint64_t sole_child = le64(pnode.data() + pval_base);
            if (!free_meta_block(step.blk, error)) return false;
            root_blk_io = sole_child;
            return true;
          }
          put_le32(pnode.data() + 16, new_pnr);
          seal_block(pnode.data(), kBtreeCsumXor);
          return stage_block(step.blk, pnode, error);
        }
        return true;
      }

      std::size_t cidx = 0;
      for (std::size_t i = 0; i < nr; ++i) {
        if (le64(node.data() + 32u + i * 8u) <= key) cidx = i;
        else break;
      }
      const std::uint64_t child_blk = le64(node.data() + val_base + cidx * 8u);
      path.push_back({cur_blk, cidx});
      cur_blk = child_blk;
    }

    error = "thin-pool B-tree exceeded maximum depth";
    return false;
  }

  bool btree_max_key(std::uint64_t root_blk,
                     std::uint32_t expected_leaf_vsz,
                     std::uint64_t& max_key_out,
                     std::string& error) {
    max_key_out = 0;
    std::set<std::uint64_t> visited;
    std::uint64_t cur = root_blk;
    for (int depth = 0; depth < 8; ++depth) {
      if (!visited.insert(cur).second) {
        error = "cycle detected in thin-pool B-tree";
        return false;
      }
      std::vector<std::uint8_t> node;
      if (!read_block(cur, node, error)) return false;
      std::uint32_t flags = 0, nr = 0, max_ent = 0, vsz = 0;
      if (!validate_btree_node(node.data(), cur, expected_leaf_vsz, flags, nr, max_ent, vsz, error)) {
        return false;
      }
      if (nr == 0u) return true;
      if (flags == kBtreeLeafNode) {
        max_key_out = le64(node.data() + 32u + static_cast<std::size_t>(nr - 1u) * 8u);
        return true;
      }
      const std::size_t val_base = 32u + static_cast<std::size_t>(max_ent) * 8u;
      cur = le64(node.data() + val_base + static_cast<std::size_t>(nr - 1u) * 8u);
    }
    error = "thin-pool B-tree exceeded maximum depth";
    return false;
  }
};

}  // namespace thin_detail

inline bool init_thin_pool_metadata(const LoadedVG& loaded,
                                    const LogicalVolume& tmeta_lv,
                                    std::uint64_t nr_data_blocks,
                                    std::uint32_t data_block_size_sectors,
                                    std::string& error) {
  using namespace thin_detail;

  std::uint64_t tmeta_bytes = 0;
  if (!lv_byte_size_checked(loaded.vg, tmeta_lv, tmeta_bytes, error)) return false;
  const std::uint64_t nr_meta_blocks = tmeta_bytes / kThinBlockSize;
  if (nr_meta_blocks < 16ULL) {
    error = "thin-pool metadata volume too small (need at least 64 KiB)";
    return false;
  }
  if (nr_data_blocks == 0ULL) {
    error = "thin-pool data volume fits zero chunks";
    return false;
  }

  const std::uint64_t n_dsm_bitmaps =
      (nr_data_blocks + kEntriesPerBitmapBlock - 1ULL) / kEntriesPerBitmapBlock;
  const std::uint64_t n_msm_bitmaps =
      (nr_meta_blocks + kEntriesPerBitmapBlock - 1ULL) / kEntriesPerBitmapBlock;

  if (n_dsm_bitmaps > btree_max_entries(16u)) {
    error = "thin-pool data volume exceeds single-leaf data space map capacity";
    return false;
  }
  if (n_msm_bitmaps > kMaxMetadataBitmaps) {
    error = "thin-pool metadata volume exceeds dm-thin maximum metadata size";
    return false;
  }

  const std::uint64_t first_dsm_bm = 7ULL;
  const std::uint64_t first_msm_bm = first_dsm_bm + n_dsm_bitmaps;
  const std::uint64_t total_alloc = first_msm_bm + n_msm_bitmaps;
  if (total_alloc >= nr_meta_blocks) {
    error = "thin-pool metadata volume too small for initial space maps";
    return false;
  }

  std::vector<std::uint8_t> blk(kThinBlockSize, 0);

  // Write non-superblock blocks 1..total_alloc-1 first, then superblock block 0 last.
  init_btree_node(blk.data(), kBtreeLeafNode, 1ULL, 0u, 8u);
  seal_block(blk.data(), kBtreeCsumXor);
  if (!write_meta_block_raw(loaded, tmeta_lv, tmeta_bytes, 1, blk.data(), error)) return false;

  init_btree_node(blk.data(), kBtreeLeafNode, 2ULL, 0u, 24u);
  seal_block(blk.data(), kBtreeCsumXor);
  if (!write_meta_block_raw(loaded, tmeta_lv, tmeta_bytes, 2, blk.data(), error)) return false;

  init_btree_node(blk.data(), kBtreeLeafNode, 3ULL, 0u, 4u);
  seal_block(blk.data(), kBtreeCsumXor);
  if (!write_meta_block_raw(loaded, tmeta_lv, tmeta_bytes, 3, blk.data(), error)) return false;

  init_btree_node(blk.data(), kBtreeLeafNode, 4ULL, 0u, 4u);
  seal_block(blk.data(), kBtreeCsumXor);
  if (!write_meta_block_raw(loaded, tmeta_lv, tmeta_bytes, 4, blk.data(), error)) return false;

  init_btree_node(blk.data(), kBtreeLeafNode, 5ULL,
                  static_cast<std::uint32_t>(n_dsm_bitmaps), 16u);
  const std::size_t dsm_val_base = 32u + static_cast<std::size_t>(btree_max_entries(16u)) * 8u;
  for (std::uint64_t i = 0; i < n_dsm_bitmaps; ++i) {
    put_le64(blk.data() + 32u + static_cast<std::size_t>(i) * 8u, i);
    const std::size_t voff = dsm_val_base + static_cast<std::size_t>(i) * 16u;
    put_le64(blk.data() + voff, first_dsm_bm + i);
    put_le32(blk.data() + voff + 8, kEntriesPerBitmapBlock);
    put_le32(blk.data() + voff + 12, 0u);
  }
  seal_block(blk.data(), kBtreeCsumXor);
  if (!write_meta_block_raw(loaded, tmeta_lv, tmeta_bytes, 5, blk.data(), error)) return false;

  std::memset(blk.data(), 0, kThinBlockSize);
  put_le32(blk.data() + 4, 0u);
  put_le64(blk.data() + 8, 6ULL);
  for (std::uint64_t j = 0; j < n_msm_bitmaps; ++j) {
    const std::uint64_t range_start = j * kEntriesPerBitmapBlock;
    const std::uint64_t range_end = range_start + kEntriesPerBitmapBlock;
    std::uint32_t used_in_bm = 0u;
    if (total_alloc > range_start) {
      used_in_bm = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(total_alloc, range_end) - range_start);
    }
    const std::size_t eoff = 16u + static_cast<std::size_t>(j) * 16u;
    put_le64(blk.data() + eoff, first_msm_bm + j);
    put_le32(blk.data() + eoff + 8, kEntriesPerBitmapBlock - used_in_bm);
    put_le32(blk.data() + eoff + 12, used_in_bm);
  }
  seal_block(blk.data(), kIndexCsumXor);
  if (!write_meta_block_raw(loaded, tmeta_lv, tmeta_bytes, 6, blk.data(), error)) return false;

  for (std::uint64_t i = 0; i < n_dsm_bitmaps; ++i) {
    std::memset(blk.data(), 0, kThinBlockSize);
    put_le32(blk.data() + 4, 0u);
    put_le64(blk.data() + 8, first_dsm_bm + i);
    seal_block(blk.data(), kBitmapCsumXor);
    if (!write_meta_block_raw(loaded, tmeta_lv, tmeta_bytes, first_dsm_bm + i, blk.data(), error)) {
      return false;
    }
  }

  for (std::uint64_t j = 0; j < n_msm_bitmaps; ++j) {
    std::memset(blk.data(), 0, kThinBlockSize);
    put_le32(blk.data() + 4, 0u);
    put_le64(blk.data() + 8, first_msm_bm + j);
    const std::uint64_t range_start = j * kEntriesPerBitmapBlock;
    const std::uint64_t range_end = range_start + kEntriesPerBitmapBlock;
    if (total_alloc > range_start) {
      const std::uint32_t used_in_bm = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(total_alloc, range_end) - range_start);
      for (std::uint32_t slot = 0; slot < used_in_bm; ++slot) {
        set_bitmap_entry(blk.data(), slot, 1u);
      }
    }
    seal_block(blk.data(), kBitmapCsumXor);
    if (!write_meta_block_raw(loaded, tmeta_lv, tmeta_bytes, first_msm_bm + j, blk.data(), error)) {
      return false;
    }
  }

  // Block 0: thin_disk_superblock written last.
  std::memset(blk.data(), 0, kThinBlockSize);
  put_le32(blk.data() + 4, 0u);
  put_le64(blk.data() + 8, 0ULL);
  put_le64(blk.data() + 32, kThinSuperblockMagic);
  put_le32(blk.data() + 40, kThinSuperblockVersion);
  put_le32(blk.data() + 44, 0u);
  put_le64(blk.data() + 48, 0ULL);
  put_le64(blk.data() + 56, 0ULL);
  put_le64(blk.data() + 64, nr_data_blocks);
  put_le64(blk.data() + 72, 0ULL);
  put_le64(blk.data() + 80, 5ULL);
  put_le64(blk.data() + 88, 3ULL);
  put_le64(blk.data() + 192, nr_meta_blocks);
  put_le64(blk.data() + 200, total_alloc);
  put_le64(blk.data() + 208, 6ULL);
  put_le64(blk.data() + 216, 4ULL);
  put_le64(blk.data() + 320, 1ULL);
  put_le64(blk.data() + 328, 2ULL);
  put_le32(blk.data() + 336, data_block_size_sectors);
  put_le32(blk.data() + 340, 8u);
  put_le64(blk.data() + 344, nr_meta_blocks);
  seal_block(blk.data(), kSuperblockCsumXor);
  return write_meta_block_raw(loaded, tmeta_lv, tmeta_bytes, 0, blk.data(), error);
}

inline bool thin_pool_get_max_device_id(const LoadedVG& loaded,
                                        const LogicalVolume& tmeta_lv,
                                        std::optional<std::uint64_t> expected_vg_trans_id,
                                        std::uint64_t& max_id_out,
                                        std::string& error) {
  thin_detail::ThinMetaEditor ed{loaded, tmeta_lv};
  if (!ed.load_superblock(expected_vg_trans_id, true, error)) return false;
  const std::uint64_t det_root = le64(ed.sb.data() + 328);
  return ed.btree_max_key(det_root, 24u, max_id_out, error);
}

inline bool thin_pool_register_device(const LoadedVG& loaded,
                                      const LogicalVolume& tmeta_lv,
                                      std::uint64_t device_id,
                                      std::uint64_t expected_vg_trans_id,
                                      std::uint64_t& out_lv_trans_id,
                                      std::uint64_t& out_pool_trans_id,
                                      std::string& error) {
  using namespace thin_detail;

  ThinMetaEditor ed{loaded, tmeta_lv};
  if (!ed.load_superblock(expected_vg_trans_id, true, error)) return false;

  const std::uint64_t old_trans = le64(ed.sb.data() + 48);
  const std::uint32_t sb_time = le32(ed.sb.data() + 44);
  std::uint64_t data_map_root = le64(ed.sb.data() + 320);
  std::uint64_t dev_det_root = le64(ed.sb.data() + 328);

  std::uint64_t empty_leaf_blk = 0;
  if (!ed.alloc_meta_block(empty_leaf_blk, error)) return false;

  std::vector<std::uint8_t> leaf(kThinBlockSize, 0);
  init_btree_node(leaf.data(), kBtreeLeafNode, empty_leaf_blk, 0u, 8u);
  seal_block(leaf.data(), kBtreeCsumXor);
  if (!ed.stage_block(empty_leaf_blk, leaf, error)) return false;

  std::uint8_t leaf_ptr_val[8]{};
  put_le64(leaf_ptr_val, empty_leaf_blk);
  if (!ed.btree_insert(data_map_root, device_id, leaf_ptr_val, 8u, error)) return false;

  std::uint8_t details_val[24]{};
  put_le64(details_val + 0, 0ULL);       // mapped_blocks
  put_le64(details_val + 8, old_trans);  // transaction_id
  put_le32(details_val + 16, sb_time);   // creation_time
  put_le32(details_val + 20, 0u);        // snapshotted_time
  if (!ed.btree_insert(dev_det_root, device_id, details_val, 24u, error)) return false;

  put_le64(ed.sb.data() + 48, old_trans + 1ULL);
  put_le64(ed.sb.data() + 320, data_map_root);
  put_le64(ed.sb.data() + 328, dev_det_root);
  seal_block(ed.sb.data(), kSuperblockCsumXor);
  if (!ed.stage_block(0ULL, ed.sb, error)) return false;

  if (!ed.flush_staged(error)) return false;

  out_lv_trans_id = old_trans;
  out_pool_trans_id = old_trans + 1ULL;
  return true;
}

inline bool thin_pool_delete_device(const LoadedVG& loaded,
                                    const LogicalVolume& tmeta_lv,
                                    std::uint64_t device_id,
                                    std::uint64_t expected_vg_trans_id,
                                    std::uint64_t& out_pool_trans_id,
                                    std::string& error) {
  using namespace thin_detail;

  ThinMetaEditor ed{loaded, tmeta_lv};
  if (!ed.load_superblock(expected_vg_trans_id, true, error)) return false;

  const std::uint64_t old_trans = le64(ed.sb.data() + 48);
  std::uint64_t data_map_root = le64(ed.sb.data() + 320);
  std::uint64_t dev_det_root = le64(ed.sb.data() + 328);

  std::vector<std::uint8_t> dev_tree_val;
  if (!ed.btree_delete(data_map_root, device_id, 8u, &dev_tree_val, error)) return false;
  if (dev_tree_val.size() != 8u) {
    error = "corrupt device mapping root entry for device_id " + std::to_string(device_id);
    return false;
  }
  const std::uint64_t dev_tree_root = le64(dev_tree_val.data());
  std::set<std::uint64_t> visited;
  if (!ed.free_device_mapping_subtree(dev_tree_root, 0, visited, error)) return false;

  if (!ed.btree_delete(dev_det_root, device_id, 24u, nullptr, error)) return false;

  put_le64(ed.sb.data() + 48, old_trans + 1ULL);
  put_le64(ed.sb.data() + 320, data_map_root);
  put_le64(ed.sb.data() + 328, dev_det_root);
  seal_block(ed.sb.data(), kSuperblockCsumXor);
  if (!ed.stage_block(0ULL, ed.sb, error)) return false;

  if (!ed.flush_staged(error)) return false;

  out_pool_trans_id = old_trans + 1ULL;
  return true;
}

}  // namespace lvmfmt
