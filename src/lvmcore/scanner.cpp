#include "winlvm/scanner.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#endif

#include "winlvm/crc32.h"
#include "winlvm/metadata_parser.h"

namespace winlvm {

namespace {

constexpr std::uint64_t kMaxMetadataTextBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::array<std::uint8_t, 16> kFmtMagic = {
    0x20, 0x4c, 0x56, 0x4d, 0x32, 0x20, 0x78, 0x5b,
    0x35, 0x41, 0x25, 0x72, 0x30, 0x4e, 0x2a, 0x3e};

struct DiskLocn {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

enum class SourceKind { image, device };

struct SourceSpec {
  SourceKind kind = SourceKind::image;
  std::string path;
};

std::string source_kind_name(SourceKind kind) {
  return kind == SourceKind::device ? "device" : "image";
}

#ifdef _WIN32
std::wstring widen_path(const std::string& path) {
  if (path.empty()) {
    return {};
  }
  const int required =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
  if (required > 0) {
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), wide.data(), required);
    return wide;
  }

  const int ansi_required =
      MultiByteToWideChar(CP_ACP, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(ansi_required), L'\0');
  MultiByteToWideChar(CP_ACP, 0, path.data(), static_cast<int>(path.size()), wide.data(), ansi_required);
  return wide;
}
#endif

class SourceReader {
 public:
  explicit SourceReader(SourceSpec source) : source_(std::move(source)) {}
  SourceReader(const SourceReader&) = delete;
  SourceReader& operator=(const SourceReader&) = delete;
  ~SourceReader() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
#endif
  }

  Status open() {
    if (source_.kind == SourceKind::device) {
      return open_device();
    }

    stream_.open(source_.path, std::ios::binary);
    if (!stream_) {
      return Status::failure("cannot open image read-only: " + source_.path);
    }
    std::error_code ec;
    size_ = std::filesystem::file_size(source_.path, ec);
    if (ec) {
      return Status::failure("cannot stat image: " + source_.path);
    }
    return Status::success();
  }

  bool read(std::uint64_t offset, std::uint64_t length, std::vector<std::uint8_t>& out) {
    if (length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      return false;
    }
    if (offset > size_ || length > size_ - offset) {
      return false;
    }
    out.assign(static_cast<std::size_t>(length), 0);
    if (source_.kind == SourceKind::device) {
      return read_device(offset, out);
    }
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return stream_.good() || (stream_.eof() && static_cast<std::uint64_t>(stream_.gcount()) == length);
  }

  std::uint64_t size() const { return size_; }
  const std::string& path() const { return source_.path; }
  SourceKind kind() const { return source_.kind; }

 private:
  Status open_device() {
#ifdef _WIN32
    const std::wstring path = widen_path(source_.path);
    handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      return Status::failure("cannot open device read-only: " + source_.path);
    }

    GET_LENGTH_INFORMATION length_info{};
    DWORD bytes_returned = 0;
    if (DeviceIoControl(handle_, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length_info, sizeof(length_info),
                        &bytes_returned, nullptr)) {
      size_ = static_cast<std::uint64_t>(length_info.Length.QuadPart);
      return Status::success();
    }

    LARGE_INTEGER file_size{};
    if (GetFileSizeEx(handle_, &file_size) && file_size.QuadPart >= 0) {
      size_ = static_cast<std::uint64_t>(file_size.QuadPart);
      return Status::success();
    }

    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
    return Status::failure("cannot determine device size: " + source_.path);
#else
    return Status::failure("--device is only supported on Windows");
#endif
  }

  bool read_device(std::uint64_t offset, std::vector<std::uint8_t>& out) {
#ifdef _WIN32
    const std::uint64_t aligned_offset = (offset / kSectorSize) * kSectorSize;
    const std::uint64_t leading = offset - aligned_offset;
    std::uint64_t aligned_length = leading + out.size();
    if (aligned_length % kSectorSize != 0) {
      aligned_length += kSectorSize - (aligned_length % kSectorSize);
    }
    if (aligned_length > static_cast<std::uint64_t>(std::numeric_limits<DWORD>::max())) {
      return false;
    }

    std::vector<std::uint8_t> aligned(static_cast<std::size_t>(aligned_length), 0);
    LARGE_INTEGER distance{};
    distance.QuadPart = static_cast<LONGLONG>(aligned_offset);
    if (!SetFilePointerEx(handle_, distance, nullptr, FILE_BEGIN)) {
      return false;
    }

    std::size_t total = 0;
    while (total < aligned.size()) {
      const DWORD request =
          static_cast<DWORD>(std::min<std::size_t>(aligned.size() - total, std::numeric_limits<DWORD>::max()));
      DWORD read = 0;
      if (!ReadFile(handle_, aligned.data() + total, request, &read, nullptr) || read == 0) {
        return false;
      }
      total += read;
    }
    std::copy_n(aligned.begin() + static_cast<std::ptrdiff_t>(leading), out.size(), out.begin());
    return true;
#else
    (void)offset;
    (void)out;
    return false;
#endif
  }

  SourceSpec source_;
  std::ifstream stream_;
  std::uint64_t size_ = 0;
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
};

std::uint16_t le16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8U));
}

std::uint32_t le32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::uint64_t le64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8U);
  }
  return value;
}

std::string ascii_field(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t length) {
  std::string out;
  out.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    const char c = static_cast<char>(bytes[offset + i]);
    if (c == '\0') {
      break;
    }
    out.push_back(c);
  }
  return out;
}

bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
  if (b > std::numeric_limits<std::uint64_t>::max() - a) {
    return false;
  }
  out = a + b;
  return true;
}

bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return false;
  }
  out = a * b;
  return true;
}

void add_diag(ScanResult& result, std::string severity, std::string code, std::string message) {
  result.diagnostics.push_back({std::move(severity), std::move(code), std::move(message)});
}

std::string normalize_lvm_uuid(std::string uuid) {
  uuid.erase(std::remove(uuid.begin(), uuid.end(), '-'), uuid.end());
  return uuid;
}

std::vector<std::uint64_t> discover_candidate_pv_bases(SourceReader& image, ScanResult& result) {
  std::set<std::uint64_t> bases;
  bases.insert(0);

  std::vector<std::uint8_t> sector;
  if (!image.read(0, kSectorSize, sector)) {
    return {bases.begin(), bases.end()};
  }

  if (sector[510] == 0x55 && sector[511] == 0xaa) {
    for (std::size_t i = 0; i < 4; ++i) {
      const std::size_t entry = 446 + i * 16;
      const std::uint8_t type = sector[entry + 4];
      const std::uint32_t start_lba = le32(sector, entry + 8);
      const std::uint32_t length_lba = le32(sector, entry + 12);
      if (type != 0 && start_lba != 0 && length_lba != 0) {
        std::uint64_t start_bytes = 0;
        if (checked_mul(start_lba, kSectorSize, start_bytes)) {
          bases.insert(start_bytes);
        }
      }
    }
  }

  std::vector<std::uint8_t> gpt;
  if (image.read(kSectorSize, kSectorSize, gpt) && std::memcmp(gpt.data(), "EFI PART", 8) == 0) {
    const std::uint64_t entries_lba = le64(gpt, 72);
    const std::uint32_t entry_count = le32(gpt, 80);
    const std::uint32_t entry_size = le32(gpt, 84);
    if (entry_size >= 128 && entry_count <= 16384) {
      std::uint64_t entries_offset = 0;
      if (checked_mul(entries_lba, kSectorSize, entries_offset)) {
        for (std::uint32_t i = 0; i < entry_count; ++i) {
          std::uint64_t entry_offset = 0;
          std::uint64_t entry_delta = 0;
          if (!checked_mul(i, entry_size, entry_delta) || !checked_add(entries_offset, entry_delta, entry_offset)) {
            add_diag(result, "error", "gpt_overflow", "overflow while reading GPT entries");
            break;
          }
          std::vector<std::uint8_t> entry;
          if (!image.read(entry_offset, entry_size, entry)) {
            break;
          }
          const bool empty_type = std::all_of(entry.begin(), entry.begin() + 16, [](std::uint8_t b) { return b == 0; });
          if (empty_type) {
            continue;
          }
          const std::uint64_t first_lba = le64(entry, 32);
          std::uint64_t first_byte = 0;
          if (checked_mul(first_lba, kSectorSize, first_byte)) {
            bases.insert(first_byte);
          }
        }
      }
    }
  }

  return {bases.begin(), bases.end()};
}

std::vector<DiskLocn> read_disk_locn_list(const std::vector<std::uint8_t>& sector, std::size_t& cursor) {
  std::vector<DiskLocn> out;
  while (cursor + 16 <= sector.size()) {
    const std::uint64_t offset = le64(sector, cursor);
    const std::uint64_t size = le64(sector, cursor + 8);
    cursor += 16;
    if (offset == 0 && size == 0) {
      break;
    }
    out.push_back({offset, size});
  }
  return out;
}

bool read_wrapped(SourceReader& image,
                  std::uint64_t pv_base,
                  std::uint64_t mda_start,
                  std::uint64_t mda_size,
                  std::uint64_t relative_offset,
                  std::uint64_t length,
                  std::vector<std::uint8_t>& out) {
  if (length > kMaxMetadataTextBytes || mda_size == 0 || relative_offset >= mda_size) {
    return false;
  }

  out.clear();
  out.reserve(static_cast<std::size_t>(length));
  std::uint64_t remaining = length;
  std::uint64_t cursor = relative_offset;
  while (remaining > 0) {
    const std::uint64_t chunk = std::min(remaining, mda_size - cursor);
    std::uint64_t device_offset = 0;
    std::uint64_t image_offset = 0;
    if (!checked_add(mda_start, cursor, device_offset) || !checked_add(pv_base, device_offset, image_offset)) {
      return false;
    }
    std::vector<std::uint8_t> bytes;
    if (!image.read(image_offset, chunk, bytes)) {
      return false;
    }
    out.insert(out.end(), bytes.begin(), bytes.end());
    remaining -= chunk;
    cursor = 0;
  }
  return true;
}

std::string metadata_text_from_bytes(const std::vector<std::uint8_t>& bytes) {
  std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  const std::size_t nul = text.find('\0');
  if (nul != std::string::npos) {
    text.resize(nul);
  }
  return text;
}

void scan_metadata_area(SourceReader& image,
                        const PhysicalVolume& pv,
                        std::uint64_t mda_offset,
                        std::uint64_t mda_size,
                        ScanResult& result) {
  MetadataCopy copy;
  copy.pv_uuid = pv.uuid;
  copy.source_path = image.path();
  copy.pv_base_offset_bytes = pv.pv_base_offset_bytes;
  copy.mda_offset_bytes = mda_offset;
  copy.mda_size_bytes = mda_size;

  std::uint64_t header_offset = 0;
  if (!checked_add(pv.pv_base_offset_bytes, mda_offset, header_offset)) {
    copy.rejected_reason = "metadata header offset overflow";
    result.metadata_copies.push_back(std::move(copy));
    return;
  }

  std::vector<std::uint8_t> header;
  if (!image.read(header_offset, kSectorSize, header)) {
    copy.rejected_reason = "cannot read metadata area header";
    result.metadata_copies.push_back(std::move(copy));
    return;
  }

  const std::uint32_t expected_crc = le32(header, 0);
  const std::uint32_t actual_crc = lvm_crc32(kLvmInitialCrc, header.data() + 4, header.size() - 4);
  copy.header_checksum_valid = expected_crc == actual_crc;
  copy.magic_valid = std::equal(kFmtMagic.begin(), kFmtMagic.end(), header.begin() + 4);
  const std::uint32_t version = le32(header, 20);
  const std::uint64_t header_start = le64(header, 24);
  const std::uint64_t header_size = le64(header, 32);

  if (!copy.header_checksum_valid || !copy.magic_valid || version != 1 || header_start != mda_offset ||
      header_size == 0) {
    copy.rejected_reason = "invalid metadata area header";
    result.metadata_copies.push_back(std::move(copy));
    return;
  }

  std::size_t cursor = 40;
  while (cursor + 24 <= header.size()) {
    MetadataRawLocation raw;
    raw.offset_bytes = le64(header, cursor);
    raw.size_bytes = le64(header, cursor + 8);
    raw.checksum = le32(header, cursor + 16);
    raw.flags = le32(header, cursor + 20);
    raw.ignored = (raw.flags & 0x00000001U) != 0;
    cursor += 24;

    if (raw.offset_bytes == 0 && raw.size_bytes == 0 && raw.checksum == 0 && raw.flags == 0) {
      break;
    }

    if (!raw.ignored) {
      std::vector<std::uint8_t> text_bytes;
      if (read_wrapped(image, pv.pv_base_offset_bytes, mda_offset, header_size, raw.offset_bytes, raw.size_bytes,
                       text_bytes)) {
        const std::uint32_t text_crc = lvm_crc32(kLvmInitialCrc, text_bytes.data(), text_bytes.size());
        raw.actual_checksum = text_crc;
        raw.checksum_valid = text_crc == raw.checksum;
        if (raw.checksum_valid && copy.text.empty()) {
          copy.text = metadata_text_from_bytes(text_bytes);
          auto parsed = parse_vg_metadata_text(copy.text);
          if (parsed) {
            copy.vg_name = parsed.value.name;
            copy.vg_uuid = parsed.value.uuid;
            copy.seqno = parsed.value.seqno;
          } else {
            copy.rejected_reason = parsed.status.message;
          }
        }
      }
    }
    copy.raw_locations.push_back(raw);
  }

  if (copy.text.empty() && copy.rejected_reason.empty()) {
    copy.rejected_reason = "no valid metadata text raw location";
  }
  result.metadata_copies.push_back(std::move(copy));
}

void scan_pv_candidate(SourceReader& image, std::uint64_t pv_base, ScanResult& result) {
  for (std::uint64_t label_sector = 0; label_sector < 4; ++label_sector) {
    std::uint64_t label_offset = 0;
    std::uint64_t sector_offset = 0;
    if (!checked_mul(label_sector, kSectorSize, sector_offset) || !checked_add(pv_base, sector_offset, label_offset)) {
      continue;
    }
    std::vector<std::uint8_t> sector;
    if (!image.read(label_offset, kSectorSize, sector)) {
      continue;
    }
    if (std::memcmp(sector.data(), "LABELONE", 8) != 0) {
      continue;
    }
    if (ascii_field(sector, 24, 8) != "LVM2 001") {
      add_diag(result, "warning", "non_lvm2_label", "found LABELONE with unsupported label type");
      continue;
    }
    const std::uint64_t sector_number = le64(sector, 8);
    const std::uint32_t expected_crc = le32(sector, 16);
    const std::uint32_t actual_crc = lvm_crc32(kLvmInitialCrc, sector.data() + 20, sector.size() - 20);
    const std::uint32_t label_contents_offset = le32(sector, 20);
    if (sector_number != label_sector || expected_crc != actual_crc || label_contents_offset >= sector.size()) {
      add_diag(result, "warning", "invalid_label", "found LVM label with invalid sector, CRC, or offset");
      continue;
    }

    PhysicalVolume pv;
    pv.source_path = image.path();
    pv.source_size_bytes = image.size();
    pv.pv_base_offset_bytes = pv_base;
    pv.label_offset_bytes = label_offset;
    pv.uuid = ascii_field(sector, label_contents_offset, 32);
    pv.device_size_bytes = le64(sector, label_contents_offset + 32);

    std::size_t cursor = label_contents_offset + 40;
    const std::vector<DiskLocn> data_areas = read_disk_locn_list(sector, cursor);
    const std::vector<DiskLocn> metadata_areas = read_disk_locn_list(sector, cursor);
    if (!data_areas.empty()) {
      pv.pe_start_bytes = data_areas.front().offset;
    }
    for (const DiskLocn& area : metadata_areas) {
      pv.metadata_area_offsets.push_back(area.offset);
    }

    const std::size_t pv_index = result.pvs.size();
    result.pvs.push_back(pv);
    for (const DiskLocn& area : metadata_areas) {
      scan_metadata_area(image, result.pvs[pv_index], area.offset, area.size, result);
    }
    return;
  }
}

void select_metadata_and_build_topology(ScanResult& result) {
  std::map<std::string, int> pv_uuid_counts;
  for (const PhysicalVolume& pv : result.pvs) {
    ++pv_uuid_counts[normalize_lvm_uuid(pv.uuid)];
  }
  bool duplicate_pv = false;
  for (const auto& [uuid, count] : pv_uuid_counts) {
    if (!uuid.empty() && count > 1) {
      duplicate_pv = true;
      add_diag(result, "error", "duplicate_pv_uuid", "duplicate PV UUID detected: " + uuid);
    }
  }
  if (duplicate_pv) {
    return;
  }

  std::vector<std::size_t> valid_indexes;
  for (std::size_t i = 0; i < result.metadata_copies.size(); ++i) {
    const MetadataCopy& copy = result.metadata_copies[i];
    if (copy.rejected_reason.empty() && !copy.text.empty() && !copy.vg_uuid.empty()) {
      valid_indexes.push_back(i);
    }
  }
  if (valid_indexes.empty()) {
    if (!result.pvs.empty()) {
      add_diag(result, "error", "no_valid_metadata", "no valid LVM metadata copies were found");
    }
    return;
  }

  std::sort(valid_indexes.begin(), valid_indexes.end(), [&](std::size_t a, std::size_t b) {
    const MetadataCopy& lhs = result.metadata_copies[a];
    const MetadataCopy& rhs = result.metadata_copies[b];
    if (lhs.seqno != rhs.seqno) {
      return lhs.seqno > rhs.seqno;
    }
    return lhs.vg_uuid < rhs.vg_uuid;
  });

  const MetadataCopy& chosen = result.metadata_copies[valid_indexes.front()];
  for (std::size_t index : valid_indexes) {
    MetadataCopy& copy = result.metadata_copies[index];
    if (copy.seqno == chosen.seqno && copy.vg_uuid == chosen.vg_uuid && copy.text != chosen.text) {
      add_diag(result, "error", "ambiguous_metadata",
               "metadata copies with the newest VG seqno disagree; refusing topology selection");
      return;
    }
  }

  result.metadata_copies[valid_indexes.front()].selected = true;
  auto parsed = parse_vg_metadata_text(chosen.text);
  if (!parsed) {
    add_diag(result, "error", "metadata_parse_failed", parsed.status.message);
    return;
  }

  VolumeGroup vg = std::move(parsed.value);
  std::map<std::string, PhysicalVolume> scanned_by_uuid;
  for (const PhysicalVolume& scanned : result.pvs) {
    scanned_by_uuid[normalize_lvm_uuid(scanned.uuid)] = scanned;
  }
  for (PhysicalVolume& metadata_pv : vg.pvs) {
    auto scanned_it = scanned_by_uuid.find(normalize_lvm_uuid(metadata_pv.uuid));
    if (scanned_it == scanned_by_uuid.end()) {
      add_diag(result, "error", "missing_pv", "metadata references missing PV UUID: " + metadata_pv.uuid);
      return;
    }
    const PhysicalVolume& scanned = scanned_it->second;
    metadata_pv.uuid = scanned.uuid;
    metadata_pv.source_path = scanned.source_path;
    metadata_pv.source_size_bytes = scanned.source_size_bytes;
    metadata_pv.pv_base_offset_bytes = scanned.pv_base_offset_bytes;
    metadata_pv.label_offset_bytes = scanned.label_offset_bytes;
    metadata_pv.metadata_area_offsets = scanned.metadata_area_offsets;
    if (metadata_pv.pe_start_bytes == 0) {
      metadata_pv.pe_start_bytes = scanned.pe_start_bytes;
    }
    for (PhysicalVolume& scanned_pv : result.pvs) {
      if (normalize_lvm_uuid(scanned_pv.uuid) == normalize_lvm_uuid(metadata_pv.uuid)) {
        scanned_pv.name = metadata_pv.name;
        scanned_pv.device_size_bytes = metadata_pv.device_size_bytes;
        scanned_pv.pe_start_bytes = metadata_pv.pe_start_bytes;
        scanned_pv.pe_count = metadata_pv.pe_count;
      }
    }
  }

  std::map<std::string, PhysicalVolume> pv_by_name;
  for (const PhysicalVolume& pv : vg.pvs) {
    pv_by_name[pv.name] = pv;
  }

  for (LogicalVolume& lv : vg.lvs) {
    if (!lv.visible) {
      lv.supported = false;
      if (lv.unsupported_reason.empty()) {
        lv.unsupported_reason = "hidden/internal LV";
      }
    }
    std::uint64_t expected_start = 0;
    for (Segment& segment : lv.segments) {
      if (segment.lv_start_bytes != expected_start) {
        lv.supported = false;
        lv.unsupported_reason = "LV segments are not contiguous";
      }
      expected_start = segment.lv_start_bytes + segment.length_bytes;

      auto pv_it = pv_by_name.find(segment.pv_name);
      if (pv_it == pv_by_name.end()) {
        lv.supported = false;
        lv.unsupported_reason = "segment references missing PV " + segment.pv_name;
        continue;
      }
      const PhysicalVolume& pv = pv_it->second;
      segment.pv_uuid = pv.uuid;
      std::uint64_t pe_offset = 0;
      if (!checked_mul(segment.pv_start_extent, vg.extent_size_bytes, pe_offset) ||
          !checked_add(pv.pe_start_bytes, pe_offset, segment.pv_start_bytes)) {
        lv.supported = false;
        lv.unsupported_reason = "segment PV offset overflow";
        continue;
      }

      std::uint64_t source_offset = 0;
      if (!checked_add(pv.pv_base_offset_bytes, segment.pv_start_bytes, source_offset)) {
        lv.supported = false;
        lv.unsupported_reason = "segment source offset overflow";
        continue;
      }
      if (source_offset > pv.source_size_bytes || segment.length_bytes > pv.source_size_bytes - source_offset) {
        lv.supported = false;
        lv.unsupported_reason = "segment exceeds backing image size";
        continue;
      }

      if (lv.supported) {
        result.mappings.push_back({vg.name, lv.name, lv.uuid, segment.lv_start_bytes, segment.length_bytes,
                                   pv.uuid, segment.pv_start_bytes, pv.source_path, source_offset});
      }
    }
    if (!lv.supported) {
      add_diag(result, "error", "unsupported_lv", vg.name + "/" + lv.name + ": " + lv.unsupported_reason);
    }
  }

  result.vgs.push_back(std::move(vg));
}

}  // namespace

Result<ScanResult> scan_images(const ScanOptions& options) {
  ScanResult result;
  if (options.image_paths.empty() && options.device_paths.empty()) {
    return Result<ScanResult>::failure("at least one --image or --device source is required for scanning");
  }

  std::vector<SourceSpec> sources;
  for (const std::string& path : options.image_paths) {
    sources.push_back({SourceKind::image, path});
  }
  for (const std::string& path : options.device_paths) {
    sources.push_back({SourceKind::device, path});
  }

  for (const SourceSpec& source : sources) {
    SourceReader image(source);
    Status open_status = image.open();
    if (!open_status.ok) {
      return Result<ScanResult>::failure(open_status.message);
    }

    InputImage input;
    input.source_type = source_kind_name(source.kind);
    input.path = source.path;
    input.size_bytes = image.size();
    input.candidate_pv_base_offsets = discover_candidate_pv_bases(image, result);
    for (const std::uint64_t base : input.candidate_pv_base_offsets) {
      scan_pv_candidate(image, base, result);
    }
    result.inputs.push_back(std::move(input));
  }

  select_metadata_and_build_topology(result);
  return Result<ScanResult>::success(std::move(result));
}

}  // namespace winlvm
