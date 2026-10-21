#include "lvmfmt/commit.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

#include "lvmfmt/bytes.h"
#include "lvmfmt/crc32.h"
#include "lvmfmt/label.h"
#include "lvmfmt/metadata.h"

namespace lvmfmt {

namespace {

constexpr char kMdaMagic[16] = {' ', 'L', 'V', 'M', '2', ' ', 'x', '[',
                                '5', 'A', '%', 'r', '0', 'N', '*', '>'};
constexpr std::uint64_t kMdaStartDefault = 4096ULL;
constexpr std::uint64_t kMdaAlign = 512ULL;
constexpr std::uint64_t kMinPeStartSectors = 16ULL;

std::string generate_random_uuid() {
  static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<std::size_t> dis(0, sizeof(chars) - 2);

  std::string s;
  s.reserve(32);
  for (int i = 0; i < 32; ++i) {
    s.push_back(chars[dis(gen)]);
  }
  return s;
}

void write_uint32_le(std::uint8_t* p, std::uint32_t val) {
  p[0] = static_cast<std::uint8_t>(val & 0xffu);
  p[1] = static_cast<std::uint8_t>((val >> 8) & 0xffu);
  p[2] = static_cast<std::uint8_t>((val >> 16) & 0xffu);
  p[3] = static_cast<std::uint8_t>((val >> 24) & 0xffu);
}

void write_uint64_le(std::uint8_t* p, std::uint64_t val) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<std::uint8_t>((val >> (i * 8)) & 0xffu);
  }
}

std::string escape_lvm_string(std::string_view input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (char c : input) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
      out.push_back(c);
    } else if (c == '\n' || c == '\r' || c == '\0') {
      continue;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string format_uuid_canonical(const std::string& id) {
  std::string raw;
  raw.reserve(32);
  for (char c : id) {
    if (c != '-') raw.push_back(c);
  }
  if (raw.size() == 32) {
    return format_uuid(raw);
  }
  return id;
}

void write_status_array(std::ostringstream& ss,
                        const std::vector<std::string>& status,
                        const std::vector<std::string>& fallback) {
  const auto& list = status.empty() ? fallback : status;
  ss << "status = [";
  for (std::size_t i = 0; i < list.size(); ++i) {
    if (i > 0) ss << ", ";
    ss << "\"" << escape_lvm_string(list[i]) << "\"";
  }
  ss << "]\n";
}

bool validate_vg_serializable(const VolumeGroup& vg, std::string& error) {
  if (vg.name.empty()) {
    error = "volume group has an empty name";
    return false;
  }
  if (vg.extent_size_sectors == 0) {
    error = "volume group '" + vg.name + "' has zero extent size";
    return false;
  }
  if (vg.pvs.empty()) {
    error = "volume group '" + vg.name + "' has no physical volumes";
    return false;
  }

  for (const auto& lv : vg.lvs) {
    if (lv.internal) {
      error = "refusing to serialize VG '" + vg.name + "': contains internal LV '" + lv.name +
              "' from an unsupported volume layout";
      return false;
    }
    if (!lv.supported) {
      error = "refusing to serialize VG '" + vg.name + "': LV '" + lv.name + "' is unsupported (" +
              (lv.unsupported_reason.empty() ? "unknown layout" : lv.unsupported_reason) + ")";
      return false;
    }
    if (lv.segments.empty()) {
      error = "refusing to serialize VG '" + vg.name + "': LV '" + lv.name + "' has no segments";
      return false;
    }
    for (const auto& seg : lv.segments) {
      if (seg.type != "linear" && seg.type != "striped") {
        error = "refusing to serialize VG '" + vg.name + "': LV '" + lv.name +
                "' uses unsupported segment type '" + seg.type + "'";
        return false;
      }
      const std::uint64_t sc = (seg.stripe_count == 0) ? 1ULL : seg.stripe_count;
      if (seg.stripes.empty() || seg.stripes.size() != sc) {
        error = "refusing to serialize VG '" + vg.name + "': LV '" + lv.name +
                "' has inconsistent stripe list";
        return false;
      }
      if (sc > 1 && (seg.stripe_size_sectors == 0 || (seg.extent_count % sc) != 0)) {
        error = "refusing to serialize VG '" + vg.name + "': LV '" + lv.name +
                "' has invalid striped segment geometry";
        return false;
      }
      for (const auto& stripe : seg.stripes) {
        if (!vg.find_pv(stripe.target)) {
          error = "refusing to serialize VG '" + vg.name + "': LV '" + lv.name +
                  "' maps onto non-PV target '" + stripe.target + "'";
          return false;
        }
      }
    }
  }
  return true;
}

bool validate_mda_header_bytes(const std::vector<std::uint8_t>& buf,
                               const DiskArea& area,
                               std::string& error) {
  if (buf.size() != kMdaHeaderSize) {
    error = "invalid MDA header buffer size";
    return false;
  }
  const std::uint32_t stored_crc = le32(buf.data());
  const std::uint32_t computed_crc = lvm_crc32(buf.data() + 4, kMdaHeaderSize - 4);
  if (stored_crc != computed_crc) {
    error = "MDA header CRC mismatch at offset " + std::to_string(area.offset);
    return false;
  }
  if (std::memcmp(buf.data() + 4, kMdaMagic, sizeof(kMdaMagic)) != 0) {
    error = "MDA header magic mismatch at offset " + std::to_string(area.offset);
    return false;
  }
  if (le32(buf.data() + 20) != 1u) {
    error = "unsupported MDA header version at offset " + std::to_string(area.offset);
    return false;
  }
  const std::uint64_t hdr_start = le64(buf.data() + 24);
  const std::uint64_t hdr_size = le64(buf.data() + 32);
  if (hdr_start != area.offset || hdr_size != area.size) {
    error = "MDA header start/size disagrees with PV header at offset " + std::to_string(area.offset);
    return false;
  }
  return true;
}

std::uint64_t align_up_512(std::uint64_t v) {
  const std::uint64_t rem = v % kMdaAlign;
  return rem ? (v + (kMdaAlign - rem)) : v;
}

// Updates PV_EXT_USED (bit 0) in pv_header_extension (version >= 2) if present.
bool set_pv_ext_used(ByteSource& storage, const PvLabel& label, bool used, std::string& error) {
  std::vector<std::uint8_t> sec;
  if (!storage.read_vec(label.label_offset, kLabelSize, sec)) {
    error = "failed to read PV label sector";
    return false;
  }
  const std::uint32_t data_off = le32(&sec[20]);
  if (data_off < 32 || data_off + 40 > kLabelSize) return true;

  std::size_t cursor = static_cast<std::size_t>(data_off) + 40;
  // Skip data_areas and metadata_areas null-terminated lists.
  for (int list = 0; list < 2; ++list) {
    while (cursor + 16 <= kLabelSize) {
      const std::uint64_t off = le64(&sec[cursor]);
      const std::uint64_t sz = le64(&sec[cursor + 8]);
      cursor += 16;
      if (off == 0 && sz == 0) break;
    }
  }
  if (cursor + 8 > kLabelSize) return true;

  const std::uint32_t ext_ver = le32(&sec[cursor]);
  if (ext_ver < 2u) return true;

  const std::uint32_t old_flags = le32(&sec[cursor + 4]);
  const std::uint32_t new_flags = used ? (old_flags | 1u) : (old_flags & ~1u);
  if (new_flags == old_flags) return true;

  write_uint32_le(&sec[cursor + 4], new_flags);
  write_uint32_le(&sec[16], lvm_crc32(&sec[20], kLabelSize - 20));
  if (!storage.write(label.label_offset, sec.data(), sec.size(), error)) {
    error = "failed to update PV_EXT_USED flag in PV label: " + error;
    return false;
  }
  return true;
}

}  // namespace

std::optional<std::string> serialize_vg_to_text(const VolumeGroup& vg, std::string& error) {
  if (!validate_vg_serializable(vg, error)) {
    return std::nullopt;
  }

  const std::time_t now = std::time(nullptr);
  static const std::vector<std::string> kDefaultVgStatus = {"RESIZEABLE", "READ", "WRITE"};
  static const std::vector<std::string> kDefaultPvStatus = {"ALLOCATABLE"};
  static const std::vector<std::string> kDefaultLvStatus = {"READ", "WRITE", "VISIBLE"};

  std::ostringstream ss;
  ss << vg.name << " {\n";
  ss << "id = \"" << escape_lvm_string(format_uuid_canonical(vg.id)) << "\"\n";
  ss << "seqno = " << vg.seqno << "\n";
  ss << "format = \"lvm2\"\n";
  write_status_array(ss, vg.status, kDefaultVgStatus);
  ss << "flags = []\n";
  if (!vg.system_id.empty()) {
    ss << "system_id = \"" << escape_lvm_string(vg.system_id) << "\"\n";
  }
  ss << "extent_size = " << vg.extent_size_sectors << "\n";
  ss << "max_lv = " << vg.max_lv << "\n";
  ss << "max_pv = " << vg.max_pv << "\n";
  ss << "metadata_copies = " << vg.metadata_copies << "\n\n";

  ss << "physical_volumes {\n";
  for (const auto& pv : vg.pvs) {
    ss << "\n" << pv.key << " {\n";
    ss << "id = \"" << escape_lvm_string(format_uuid_canonical(pv.id)) << "\"\n";
    const std::string dev_hint = pv.device.empty() ? "/dev/unknown" : pv.device;
    ss << "device = \"" << escape_lvm_string(dev_hint) << "\"\n";
    write_status_array(ss, pv.status, kDefaultPvStatus);
    ss << "flags = []\n";
    ss << "dev_size = " << pv.dev_size_sectors << "\n";
    ss << "pe_start = " << pv.pe_start_sectors << "\n";
    ss << "pe_count = " << pv.pe_count << "\n";
    ss << "}\n";
  }
  ss << "}\n";

  if (!vg.lvs.empty()) {
    ss << "\nlogical_volumes {\n";
    for (const auto& lv : vg.lvs) {
      ss << "\n" << lv.name << " {\n";
      ss << "id = \"" << escape_lvm_string(format_uuid_canonical(lv.id)) << "\"\n";
      write_status_array(ss, lv.status, kDefaultLvStatus);
      ss << "flags = []\n";
      ss << "creation_time = " << now << "\n";
      ss << "creation_host = \"winlvm\"\n";
      ss << "segment_count = " << lv.segments.size() << "\n\n";

      std::size_t seg_idx = 1;
      for (const auto& seg : lv.segments) {
        const std::uint64_t sc = (seg.stripe_count == 0) ? 1ULL : seg.stripe_count;
        ss << "segment" << seg_idx++ << " {\n";
        ss << "start_extent = " << seg.start_extent << "\n";
        ss << "extent_count = " << seg.extent_count << "\n";
        ss << "type = \"striped\"\n";
        ss << "stripe_count = " << sc << "\n";
        if (sc > 1 && seg.stripe_size_sectors > 0) {
          ss << "stripe_size = " << seg.stripe_size_sectors << "\n";
        }
        ss << "stripes = [\n";
        for (std::size_t s = 0; s < seg.stripes.size(); ++s) {
          ss << "\"" << escape_lvm_string(seg.stripes[s].target) << "\", "
             << seg.stripes[s].start_extent;
          if (s + 1 < seg.stripes.size()) ss << ",\n";
          else ss << "\n";
        }
        ss << "]\n";
        ss << "}\n";
      }
      ss << "}\n";
    }
    ss << "}\n";
  }

  ss << "}\n\n";
  ss << "contents = \"Text Format Volume Group\"\n";
  ss << "version = 1\n\n";
  ss << "description = \"Created by WinLVM\"\n\n";
  ss << "creation_host = \"winlvm\"\n";
  ss << "creation_time = " << now << "\n";
  return ss.str();
}

std::string serialize_vg_to_text(const VolumeGroup& vg) {
  std::string err;
  auto res = serialize_vg_to_text(vg, err);
  return res.value_or(std::string{});
}

bool pvcreate_initialize(ByteSource& storage,
                         const PvCreateOptions& options,
                         std::string& generated_uuid,
                         std::string& error) {
  std::string uuid = options.custom_uuid.empty() ? generate_random_uuid() : options.custom_uuid;
  std::string clean_uuid;
  clean_uuid.reserve(32);
  for (char c : uuid) {
    if (c != '-') clean_uuid.push_back(c);
  }
  if (clean_uuid.size() != 32) {
    error = "PV UUID must be 32 characters (got " + std::to_string(clean_uuid.size()) + ")";
    return false;
  }
  generated_uuid = clean_uuid;

  if (options.pe_start_sectors < kMinPeStartSectors) {
    error = "pe_start_sectors must be >= " + std::to_string(kMinPeStartSectors) +
            " when metadata area is enabled";
    return false;
  }
  auto pe_start_bytes_opt = checked_mul(options.pe_start_sectors, 512ULL);
  if (!pe_start_bytes_opt || *pe_start_bytes_opt <= kMdaStartDefault) {
    error = "pe_start_sectors out of range";
    return false;
  }
  const std::uint64_t pe_start_bytes = *pe_start_bytes_opt;
  const std::uint64_t max_mda_bytes = pe_start_bytes - kMdaStartDefault;

  std::uint64_t mda_size = max_mda_bytes;
  if (options.mda_size_sectors > 0) {
    auto req_mda = checked_mul(options.mda_size_sectors, 512ULL);
    if (!req_mda) {
      error = "mda_size_sectors overflow";
      return false;
    }
    mda_size = std::min<std::uint64_t>(*req_mda, max_mda_bytes);
  }
  if (mda_size < 4096ULL) {
    error = "metadata area size (" + std::to_string(mda_size) +
            " bytes) is too small (minimum 4096 bytes)";
    return false;
  }

  const std::uint64_t backing_size = storage.size();
  std::uint64_t dev_size = options.override_device_size;
  if (dev_size == 0) {
    dev_size = backing_size;
  } else if (backing_size > 0 && dev_size > backing_size) {
    error = "requested PV size (" + std::to_string(dev_size) +
            " bytes) exceeds backing storage size (" + std::to_string(backing_size) + " bytes)";
    return false;
  }

  const std::uint64_t min_dev_size = std::max<std::uint64_t>(pe_start_bytes + 1024ULL * 1024ULL,
                                                             2ULL * 1024ULL * 1024ULL);
  if (dev_size < min_dev_size) {
    error = "device size too small for LVM physical volume (need at least " +
            std::to_string(min_dev_size) + " bytes)";
    return false;
  }
  if (!range_fits(kMdaStartDefault, mda_size, dev_size)) {
    error = "metadata area exceeds device size";
    return false;
  }

  // Zero sector 0 first to remove stale partition tables or signatures.
  std::vector<std::uint8_t> zero_sec(512, 0);
  if (!storage.write(0, zero_sec.data(), zero_sec.size(), error)) {
    error = "failed to clear sector 0: " + error;
    return false;
  }

  // Prepare Sector 1: label_header + pv_header + pv_header_extension (version 2).
  std::vector<std::uint8_t> sector1(512, 0);
  std::memcpy(&sector1[0], "LABELONE", 8);
  write_uint64_le(&sector1[8], 1);
  // 16..19: CRC32 filled after layout
  write_uint32_le(&sector1[20], 32);
  std::memcpy(&sector1[24], "LVM2 001", 8);

  // PV header at offset 32.
  std::memcpy(&sector1[32], clean_uuid.data(), 32);
  write_uint64_le(&sector1[64], dev_size);

  // Data area descriptor (72..87) + terminator (88..103).
  write_uint64_le(&sector1[72], pe_start_bytes);
  write_uint64_le(&sector1[80], 0);
  write_uint64_le(&sector1[88], 0);
  write_uint64_le(&sector1[96], 0);

  // Metadata area descriptor (104..119) + terminator (120..135).
  write_uint64_le(&sector1[104], kMdaStartDefault);
  write_uint64_le(&sector1[112], mda_size);
  write_uint64_le(&sector1[120], 0);
  write_uint64_le(&sector1[128], 0);

  // PV header extension version 2 (136..143) + empty bootloader area terminator (144..159).
  write_uint32_le(&sector1[136], 2u);
  write_uint32_le(&sector1[140], 0u);
  write_uint64_le(&sector1[144], 0);
  write_uint64_le(&sector1[152], 0);

  const std::uint32_t label_crc = lvm_crc32(&sector1[20], 512 - 20);
  write_uint32_le(&sector1[16], label_crc);

  if (!storage.write(512, sector1.data(), sector1.size(), error)) {
    error = "failed to write PV label to sector 1: " + error;
    return false;
  }

  // Prepare and write empty MDA header at offset 4096.
  std::vector<std::uint8_t> mda_header(512, 0);
  std::memcpy(&mda_header[4], kMdaMagic, sizeof(kMdaMagic));
  write_uint32_le(&mda_header[20], 1u);
  write_uint64_le(&mda_header[24], kMdaStartDefault);
  write_uint64_le(&mda_header[32], mda_size);

  const std::uint32_t mda_crc = lvm_crc32(&mda_header[4], 512 - 4);
  write_uint32_le(&mda_header[0], mda_crc);

  if (!storage.write(kMdaStartDefault, mda_header.data(), mda_header.size(), error)) {
    error = "failed to write MDA header at offset 4096: " + error;
    return false;
  }

  return true;
}

bool pvremove_wipe(ByteSource& storage, std::string& error) {
  std::vector<std::uint8_t> zeros(8192, 0);
  if (!storage.write(0, zeros.data(), zeros.size(), error)) {
    error = "failed to wipe PV label and MDA header: " + error;
    return false;
  }
  return true;
}

bool pv_clear_vg_metadata(ByteSource& storage, std::string& error) {
  auto label = read_pv_label(storage, error);
  if (!label) return false;

  for (const auto& area : label->metadata_areas) {
    if (area.size < kMdaHeaderSize || !range_fits(area.offset, area.size, storage.size())) {
      error = "metadata area lies outside device bounds";
      return false;
    }
    std::vector<std::uint8_t> mdah;
    if (!storage.read_vec(area.offset, kMdaHeaderSize, mdah)) {
      error = "failed to read MDA header at offset " + std::to_string(area.offset);
      return false;
    }
    if (!validate_mda_header_bytes(mdah, area, error)) return false;
    if ((le32(&mdah[60]) & kRawLocnIgnored) != 0) continue;

    // Zero Slot 0 (40..63) and Slot 1 (64..87) while keeping mda_header intact.
    std::memset(&mdah[40], 0, 48);
    write_uint32_le(&mdah[0], lvm_crc32(&mdah[4], kMdaHeaderSize - 4));
    if (!storage.write(area.offset, mdah.data(), mdah.size(), error)) {
      error = "failed to clear MDA header at offset " + std::to_string(area.offset) + ": " + error;
      return false;
    }
  }
  return set_pv_ext_used(storage, *label, false, error);
}

bool pv_update_device_size(ByteSource& storage,
                           std::uint64_t new_size_bytes,
                           std::string& error) {
  auto label = read_pv_label(storage, error);
  if (!label) return false;

  if (storage.size() > 0 && new_size_bytes > storage.size()) {
    error = "requested PV size exceeds backing storage size";
    return false;
  }

  std::vector<std::uint8_t> sec;
  if (!storage.read_vec(label->label_offset, kLabelSize, sec)) {
    error = "failed to read PV label sector";
    return false;
  }
  const std::uint32_t data_off = le32(&sec[20]);
  if (data_off < 32 || data_off + 40 > kLabelSize) {
    error = "invalid PV header offset in label sector";
    return false;
  }

  write_uint64_le(&sec[data_off + 32], new_size_bytes);
  write_uint32_le(&sec[16], lvm_crc32(&sec[20], kLabelSize - 20));

  if (!storage.write(label->label_offset, sec.data(), sec.size(), error)) {
    error = "failed to write updated PV label: " + error;
    return false;
  }
  return true;
}

bool commit_vg_metadata(const VolumeGroup& vg,
                        const std::vector<ByteSource*>& pv_storages,
                        std::string& error) {
  if (pv_storages.empty()) {
    error = "no physical volumes provided to commit metadata";
    return false;
  }

  auto text_opt = serialize_vg_to_text(vg, error);
  if (!text_opt) return false;

  // Upstream LVM2 includes the terminating NUL byte in both raw_locn.size
  // and raw_locn.checksum (format_text/export.c:1046,1070; format-text.c:689).
  std::vector<std::uint8_t> payload(text_opt->begin(), text_opt->end());
  payload.push_back(0);
  const std::uint64_t text_size = static_cast<std::uint64_t>(payload.size());
  const std::uint32_t text_crc = lvm_crc32(payload.data(), payload.size());

  struct MdaCommitState {
    ByteSource* storage = nullptr;
    std::uint64_t mda_start = 0;
    std::uint64_t mda_size = 0;
    std::vector<std::uint8_t> orig_mdah;
    bool has_old = false;
    std::uint64_t old_start = 0;
    std::uint64_t new_start = kMdaHeaderSize;
    std::uint64_t new_wrap = 0;
    bool precommitted = false;
    bool committed = false;
  };

  std::vector<MdaCommitState> states;
  std::vector<std::pair<ByteSource*, PvLabel>> pv_labels;

  // Phase 0: Discover every non-ignored MDA on every PV, validate headers,
  // enforce upstream circular-ring limits, and compute non-overlapping offsets.
  for (ByteSource* storage : pv_storages) {
    if (!storage) continue;

    auto label = read_pv_label(*storage, error);
    if (!label) {
      error = "failed to read PV label before commit: " + error;
      return false;
    }
    pv_labels.emplace_back(storage, *label);

    for (const auto& area : label->metadata_areas) {
      if (area.size < kMdaHeaderSize || !range_fits(area.offset, area.size, storage->size())) {
        error = "metadata area at offset " + std::to_string(area.offset) + " lies outside device";
        return false;
      }

      MdaCommitState st;
      st.storage = storage;
      st.mda_start = area.offset;
      st.mda_size = area.size;

      if (!storage->read_vec(area.offset, kMdaHeaderSize, st.orig_mdah)) {
        error = "failed to read MDA header at offset " + std::to_string(area.offset);
        return false;
      }
      if (!validate_mda_header_bytes(st.orig_mdah, area, error)) {
        return false;
      }

      // Skip MDAs marked RAW_LOCN_IGNORED (--metadataignore y).
      const std::uint32_t slot0_flags = le32(&st.orig_mdah[60]);
      if ((slot0_flags & kRawLocnIgnored) != 0) {
        continue;
      }

      // Upstream max_size = ((mda_size - 512) / 2) - 512 so two full copies
      // (committed + precommitted) always fit without overlap.
      if (st.mda_size <= kMdaHeaderSize + 1024ULL) {
        error = "metadata area too small (" + std::to_string(st.mda_size) + " bytes)";
        return false;
      }
      const std::uint64_t max_size = ((st.mda_size - kMdaHeaderSize) / 2ULL) - kMdaAlign;
      if (text_size > max_size) {
        error = "VG metadata (" + std::to_string(text_size) +
                " bytes) exceeds maximum MDA capacity (" + std::to_string(max_size) + " bytes)";
        return false;
      }

      const std::uint64_t old_start = le64(&st.orig_mdah[40]);
      const std::uint64_t old_size = le64(&st.orig_mdah[48]);
      std::uint64_t old_last = 0;
      std::uint64_t old_wrap = 0;

      if (old_start == 0 && old_size == 0) {
        st.has_old = false;
        st.new_start = kMdaHeaderSize;
      } else {
        if (old_start < kMdaHeaderSize || old_start >= st.mda_size ||
            old_size == 0 || old_size > st.mda_size - kMdaHeaderSize) {
          error = "existing Slot 0 descriptor is out of bounds in MDA at offset " +
                  std::to_string(area.offset);
          return false;
        }
        st.has_old = true;
        st.old_start = old_start;
        if (old_start + old_size > st.mda_size) {
          old_wrap = (old_start + old_size) - st.mda_size;
          old_last = old_wrap + kMdaHeaderSize - 1ULL;
        } else {
          old_wrap = 0;
          old_last = old_start + old_size - 1ULL;
        }

        // Upstream _next_rlocn_offset: 512-byte aligned start following old_last.
        if (st.mda_size - old_last < kMdaAlign) {
          st.new_start = kMdaHeaderSize;
        } else {
          const std::uint64_t next_start = align_up_512(old_last + 1ULL);
          st.new_start = (next_start > st.mda_size - kMdaAlign) ? kMdaHeaderSize : next_start;
        }
      }

      std::uint64_t new_last = 0;
      if (st.new_start + text_size > st.mda_size) {
        st.new_wrap = (st.new_start + text_size) - st.mda_size;
        new_last = st.new_wrap + kMdaHeaderSize - 1ULL;
      } else {
        st.new_wrap = 0;
        new_last = st.new_start + text_size - 1ULL;
      }

      if (st.has_old) {
        bool overlap = true;
        if (st.new_wrap && old_wrap) {
          overlap = true;
        } else if (!st.new_wrap && !old_wrap &&
                   st.new_start > old_last && new_last >= st.new_start) {
          overlap = false;
        } else if (!st.new_wrap && !old_wrap &&
                   st.new_start < old_start && new_last < old_start) {
          overlap = false;
        } else if (old_wrap && !st.new_wrap &&
                   old_last < st.new_start && st.new_start <= new_last && new_last < old_start) {
          overlap = false;
        } else if (st.new_wrap && !old_wrap &&
                   new_last < old_start && old_start <= old_last && old_last < st.new_start) {
          overlap = false;
        }
        if (overlap) {
          error = "new metadata would overlap committed Slot 0 in circular MDA buffer";
          return false;
        }
      }

      states.push_back(std::move(st));
    }
  }

  if (states.empty()) {
    error = "no active (non-ignored) metadata areas found on provided physical volumes";
    return false;
  }

  // Phase 1 (Precommit): Write payload (split cleanly if wrapped, zero-padded to 512B)
  // and publish raw_locns[1] at MDA header bytes 64..87 across every MDA.
  bool phase1_ok = true;
  for (auto& st : states) {
    const std::uint64_t write1_raw = text_size - st.new_wrap;
    std::uint64_t write1_len = write1_raw;
    if (st.new_wrap == 0) {
      const std::uint64_t aligned = align_up_512(write1_raw);
      if (st.new_start + aligned <= st.mda_size &&
          (!st.has_old || st.old_start < st.new_start || st.new_start + aligned <= st.old_start)) {
        write1_len = aligned;
      }
    }

    std::vector<std::uint8_t> part1(static_cast<std::size_t>(write1_len), 0);
    std::memcpy(part1.data(), payload.data(), static_cast<std::size_t>(write1_raw));
    if (!st.storage->write(st.mda_start + st.new_start, part1.data(), part1.size(), error)) {
      error = "failed to write metadata payload at offset " +
              std::to_string(st.mda_start + st.new_start) + ": " + error;
      phase1_ok = false;
      break;
    }

    if (st.new_wrap > 0) {
      std::uint64_t write2_len = st.new_wrap;
      const std::uint64_t aligned = align_up_512(st.new_wrap);
      if (kMdaHeaderSize + aligned <= st.mda_size &&
          (!st.has_old || kMdaHeaderSize + aligned <= st.old_start)) {
        write2_len = aligned;
      }
      std::vector<std::uint8_t> part2(static_cast<std::size_t>(write2_len), 0);
      std::memcpy(part2.data(), payload.data() + write1_raw, static_cast<std::size_t>(st.new_wrap));
      if (!st.storage->write(st.mda_start + kMdaHeaderSize, part2.data(), part2.size(), error)) {
        error = "failed to write wrapped metadata payload at offset " +
                std::to_string(st.mda_start + kMdaHeaderSize) + ": " + error;
        phase1_ok = false;
        break;
      }
    }

    std::vector<std::uint8_t> mdah = st.orig_mdah;
    write_uint64_le(&mdah[64], st.new_start);
    write_uint64_le(&mdah[72], text_size);
    write_uint32_le(&mdah[80], text_crc);
    write_uint32_le(&mdah[84], 0u);
    write_uint32_le(&mdah[0], lvm_crc32(&mdah[4], kMdaHeaderSize - 4));

    if (!st.storage->write(st.mda_start, mdah.data(), mdah.size(), error)) {
      error = "failed to write precommit MDA header at offset " +
              std::to_string(st.mda_start) + ": " + error;
      phase1_ok = false;
      break;
    }
    st.precommitted = true;
  }

  if (!phase1_ok) {
    for (auto& st : states) {
      if (st.precommitted) {
        std::string rb_err;
        st.storage->write(st.mda_start, st.orig_mdah.data(), st.orig_mdah.size(), rb_err);
      }
    }
    return false;
  }

  // Phase 2 (Commit): Promote raw_locn to Slot 0 (40..63), zero Slot 1 (64..87),
  // recompute header CRC32 across every MDA of every PV.
  bool phase2_ok = true;
  for (auto& st : states) {
    std::vector<std::uint8_t> mdah = st.orig_mdah;
    write_uint64_le(&mdah[40], st.new_start);
    write_uint64_le(&mdah[48], text_size);
    write_uint32_le(&mdah[56], text_crc);
    write_uint32_le(&mdah[60], 0u);
    std::memset(&mdah[64], 0, 24);
    write_uint32_le(&mdah[0], lvm_crc32(&mdah[4], kMdaHeaderSize - 4));

    if (!st.storage->write(st.mda_start, mdah.data(), mdah.size(), error)) {
      error = "failed to commit active MDA header at offset " +
              std::to_string(st.mda_start) + ": " + error;
      phase2_ok = false;
      break;
    }
    st.committed = true;
  }

  if (!phase2_ok) {
    for (auto& st : states) {
      if (st.committed || st.precommitted) {
        std::string rb_err;
        st.storage->write(st.mda_start, st.orig_mdah.data(), st.orig_mdah.size(), rb_err);
      }
    }
    return false;
  }

  // Mark PV_EXT_USED in pv_header_extension (v2) so upstream vgck reports 0 warnings.
  for (const auto& entry : pv_labels) {
    if (!set_pv_ext_used(*entry.first, entry.second, true, error)) {
      return false;
    }
  }

  return true;
}

bool backup_vg_metadata(const VolumeGroup& vg,
                        std::string_view filepath,
                        std::string& error) {
  auto text_opt = serialize_vg_to_text(vg, error);
  if (!text_opt) return false;

  std::ofstream out(std::string(filepath), std::ios::binary);
  if (!out) {
    error = "failed to open backup file for writing: " + std::string(filepath);
    return false;
  }
  out << "# Generated by WinLVM (LVM2 metadata backup)\n\n";
  out << *text_opt;
  out.close();
  if (!out) {
    error = "failed while writing backup file: " + std::string(filepath);
    return false;
  }
  return true;
}

bool restore_vg_metadata(std::string_view filepath,
                         const std::vector<ByteSource*>& pv_storages,
                         std::string& error) {
  std::ifstream in(std::string(filepath), std::ios::binary);
  if (!in) {
    error = "failed to open backup file for reading: " + std::string(filepath);
    return false;
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();

  auto vg_opt = parse_vg_metadata(text, error);
  if (!vg_opt) {
    error = "failed to parse metadata backup: " + error;
    return false;
  }

  return commit_vg_metadata(*vg_opt, pv_storages, error);
}

}  // namespace lvmfmt
