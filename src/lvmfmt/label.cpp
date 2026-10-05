#include "lvmfmt/label.h"

#include <cstring>

#include "lvmfmt/bytes.h"
#include "lvmfmt/crc32.h"
#include "lvmfmt/metadata.h"

namespace lvmfmt {
namespace {

constexpr char kLabelMagic[8] = {'L', 'A', 'B', 'E', 'L', 'O', 'N', 'E'};
constexpr char kLabelType[8] = {'L', 'V', 'M', '2', ' ', '0', '0', '1'};

// " LVM2 x[5A%r0N*>" — see FMTT_MAGIC in the LVM2 sources.
constexpr char kMdaMagic[16] = {' ', 'L', 'V', 'M', '2', ' ', 'x', '[',
                                '5', 'A', '%', 'r', '0', 'N', '*', '>'};

// LVM places the label within the first few 512-byte sectors. Scanning a
// little further than the canonical sector 1 costs nothing and keeps us honest
// about devices with a 4096-byte logical sector size.
constexpr std::uint64_t kLabelScanBytes = 8192;

// Guard against a corrupt header describing an absurd number of areas.
constexpr std::size_t kMaxAreas = 64;

// A single VG's text metadata. LVM's default MDA is 1 MiB; refuse anything
// wildly larger rather than allocating whatever the disk asks for.
constexpr std::uint64_t kMaxMetadataText = 64ull * 1024 * 1024;

bool parse_area_list(const std::vector<std::uint8_t>& buf, std::size_t& cursor,
                     std::vector<DiskArea>& out, std::string& error) {
  while (true) {
    if (cursor + 16 > buf.size()) {
      error = "physical volume header truncated in disk-area list";
      return false;
    }
    const std::uint64_t offset = le64(buf.data() + cursor);
    const std::uint64_t size = le64(buf.data() + cursor + 8);
    cursor += 16;
    if (offset == 0 && size == 0) return true;  // terminator
    if (out.size() >= kMaxAreas) {
      error = "physical volume header declares too many disk areas";
      return false;
    }
    out.push_back(DiskArea{offset, size});
  }
}

}  // namespace

std::string format_uuid(const std::string& raw) {
  if (raw.size() != 32) return raw;
  // LVM's canonical grouping: 6-4-4-4-4-4-6.
  static constexpr int kGroups[] = {6, 4, 4, 4, 4, 4, 6};
  std::string out;
  std::size_t pos = 0;
  for (std::size_t g = 0; g < sizeof(kGroups) / sizeof(kGroups[0]); ++g) {
    if (g) out.push_back('-');
    out.append(raw, pos, static_cast<std::size_t>(kGroups[g]));
    pos += static_cast<std::size_t>(kGroups[g]);
  }
  return out;
}

std::optional<PvLabel> read_pv_label(ByteSource& source, std::string& error) {
  const std::uint64_t scan = std::min<std::uint64_t>(kLabelScanBytes, source.size());
  if (scan < kLabelSize) {
    error = "device too small to contain a PV label";
    return std::nullopt;
  }

  std::vector<std::uint8_t> head;
  if (!source.read_vec(0, static_cast<std::size_t>(scan), head)) {
    error = "failed to read device head";
    return std::nullopt;
  }

  for (std::uint64_t off = 0; off + kLabelSize <= scan; off += kLabelSize) {
    const std::uint8_t* p = head.data() + off;
    if (std::memcmp(p, kLabelMagic, sizeof(kLabelMagic)) != 0) continue;

    const std::uint32_t stored_crc = le32(p + 16);
    const std::uint32_t computed = lvm_crc32(p + 20, kLabelSize - 20);
    if (stored_crc != computed) {
      error = "PV label CRC mismatch";
      return std::nullopt;
    }
    if (std::memcmp(p + 24, kLabelType, sizeof(kLabelType)) != 0) {
      error = "unsupported PV label type";
      return std::nullopt;
    }

    const std::uint64_t claimed_sector = le64(p + 8);
    const std::uint32_t data_offset = le32(p + 20);
    if (data_offset < 32 || data_offset >= kLabelSize) {
      error = "PV label data offset out of range";
      return std::nullopt;
    }

    // The PV header follows within the same label sector; the areas it lists
    // may extend past it, so re-read a generous window.
    std::vector<std::uint8_t> pvh;
    const std::uint64_t pvh_off = off + data_offset;
    const std::size_t pvh_len =
        static_cast<std::size_t>(std::min<std::uint64_t>(kLabelSize, source.size() - pvh_off));
    if (pvh_off + pvh_len <= scan) {
      pvh.assign(head.data() + pvh_off, head.data() + pvh_off + pvh_len);
    } else if (!source.read_vec(pvh_off, pvh_len, pvh)) {
      error = "failed to read physical volume header";
      return std::nullopt;
    }
    if (pvh.size() < 40) {
      error = "failed to read physical volume header";
      return std::nullopt;
    }

    PvLabel label;
    label.label_offset = off;
    label.label_sector = claimed_sector;
    label.pv_uuid.assign(reinterpret_cast<const char*>(pvh.data()), 32);
    label.device_size = le64(pvh.data() + 32);

    std::size_t cursor = 40;
    if (!parse_area_list(pvh, cursor, label.data_areas, error)) return std::nullopt;
    if (!parse_area_list(pvh, cursor, label.metadata_areas, error)) return std::nullopt;

    return label;
  }

  error = "no LVM2 PV label found";
  return std::nullopt;
}

std::optional<MdaHeader> read_mda_header(ByteSource& source, const DiskArea& area,
                                         std::string& error) {
  if (area.size < kMdaHeaderSize || !range_fits(area.offset, area.size, source.size())) {
    error = "metadata area lies outside the device";
    return std::nullopt;
  }

  std::vector<std::uint8_t> buf;
  if (!source.read_vec(area.offset, kMdaHeaderSize, buf)) {
    error = "failed to read metadata area header";
    return std::nullopt;
  }

  const std::uint32_t stored = le32(buf.data());
  const std::uint32_t computed = lvm_crc32(buf.data() + 4, kMdaHeaderSize - 4);
  if (stored != computed) {
    error = "metadata area header CRC mismatch";
    return std::nullopt;
  }
  if (std::memcmp(buf.data() + 4, kMdaMagic, sizeof(kMdaMagic)) != 0) {
    error = "metadata area header magic mismatch";
    return std::nullopt;
  }
  if (le32(buf.data() + 20) != 1) {
    error = "unsupported metadata area header version";
    return std::nullopt;
  }

  MdaHeader mda;
  mda.start = le64(buf.data() + 24);
  mda.size = le64(buf.data() + 32);
  // raw_locn slot 0 == committed metadata.
  mda.commit_offset = le64(buf.data() + 40);
  mda.commit_size = le64(buf.data() + 48);
  mda.commit_checksum = le32(buf.data() + 56);

  if (mda.start != area.offset || mda.size != area.size) {
    error = "metadata area header disagrees with the PV header";
    return std::nullopt;
  }
  if (mda.commit_size == 0) {
    error = "metadata area contains no committed metadata";
    return std::nullopt;
  }
  if (mda.commit_size > kMaxMetadataText ||
      !range_fits(mda.commit_offset, mda.commit_size, mda.size + mda.commit_size)) {
    error = "committed metadata descriptor is out of range";
    return std::nullopt;
  }
  if (mda.commit_offset < kMdaHeaderSize || mda.commit_offset >= mda.size) {
    error = "committed metadata offset is inside the header or past the area";
    return std::nullopt;
  }
  return mda;
}

std::optional<std::string> read_metadata_text(ByteSource& source, const MdaHeader& mda,
                                              std::string& error) {
  // The text lives in a ring buffer occupying [start + 512, start + size). A
  // record that runs off the end wraps back to the start of that region.
  const std::uint64_t ring_begin = kMdaHeaderSize;
  const std::uint64_t ring_size = mda.size - ring_begin;
  if (mda.commit_size > ring_size) {
    error = "committed metadata is larger than the metadata area";
    return std::nullopt;
  }

  const std::uint64_t rel = mda.commit_offset - ring_begin;
  const std::uint64_t first = std::min<std::uint64_t>(mda.commit_size, ring_size - rel);
  const std::uint64_t wrapped = mda.commit_size - first;

  std::string text;
  text.resize(static_cast<std::size_t>(mda.commit_size));

  if (!source.read(mda.start + mda.commit_offset, text.data(), static_cast<std::size_t>(first))) {
    error = "failed to read metadata text";
    return std::nullopt;
  }
  if (wrapped > 0 &&
      !source.read(mda.start + ring_begin, text.data() + first, static_cast<std::size_t>(wrapped))) {
    error = "failed to read wrapped metadata text";
    return std::nullopt;
  }

  const std::uint32_t computed = lvm_crc32(text.data(), text.size());
  if (computed != mda.commit_checksum) {
    error = "metadata text CRC mismatch";
    return std::nullopt;
  }
  return text;
}

std::optional<std::string> read_vg_metadata(ByteSource& source, const PvLabel& label,
                                            std::string& error) {
  if (label.metadata_areas.empty()) {
    error = "physical volume carries no metadata area";
    return std::nullopt;
  }
  std::string last_error;
  std::optional<std::string> best_text;
  std::uint64_t best_seqno = 0;
  bool found_any = false;

  for (const auto& area : label.metadata_areas) {
    std::string local;
    auto mda = read_mda_header(source, area, local);
    if (!mda) {
      last_error = local;
      continue;
    }
    auto text = read_metadata_text(source, *mda, local);
    if (!text) {
      last_error = local;
      continue;
    }
    std::string parse_err;
    auto vg = parse_vg_metadata(*text, parse_err);
    if (vg) {
      if (!found_any || vg->seqno > best_seqno) {
        best_seqno = vg->seqno;
        best_text = std::move(text);
        found_any = true;
      }
    } else if (!found_any) {
      best_text = std::move(text);
      found_any = true;
    }
  }
  if (found_any) return best_text;
  error = last_error.empty() ? "no readable metadata area" : last_error;
  return std::nullopt;
}

}  // namespace lvmfmt
