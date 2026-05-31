#include "winlvm/exporter.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <vector>

#include "winlvm/hash.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace winlvm {

namespace {

constexpr std::uint64_t kCopyChunkBytes = 1024ULL * 1024ULL;

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

class SourceInput {
 public:
  SourceInput(std::string path, bool device) : path_(std::move(path)), device_(device) {}
  SourceInput(const SourceInput&) = delete;
  SourceInput& operator=(const SourceInput&) = delete;
  SourceInput(SourceInput&& other) noexcept
      : path_(std::move(other.path_)), device_(other.device_), file_(std::move(other.file_))
#ifdef _WIN32
        ,
        handle_(other.handle_)
#endif
  {
#ifdef _WIN32
    other.handle_ = INVALID_HANDLE_VALUE;
#endif
  }
  ~SourceInput() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
#endif
  }

  Status open() {
    if (device_) {
      return open_device();
    }
    file_.open(path_, std::ios::binary);
    if (!file_) {
      return Status::failure("cannot open source image read-only: " + path_);
    }
    return Status::success();
  }

  bool read(std::uint64_t offset, std::uint64_t length, std::vector<char>& buffer) {
    if (length > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
      return false;
    }
    buffer.assign(static_cast<std::size_t>(length), 0);
    if (device_) {
      return read_device(offset, buffer);
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    return file_.good() || (file_.eof() && static_cast<std::uint64_t>(file_.gcount()) == length);
  }

 private:
  Status open_device() {
#ifdef _WIN32
    const std::wstring path = widen_path(path_);
    handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      return Status::failure("cannot open source device read-only: " + path_);
    }
    return Status::success();
#else
    return Status::failure("--device is only supported on Windows");
#endif
  }

  bool read_device(std::uint64_t offset, std::vector<char>& buffer) {
#ifdef _WIN32
    const std::uint64_t aligned_offset = (offset / kSectorSize) * kSectorSize;
    const std::uint64_t leading = offset - aligned_offset;
    std::uint64_t aligned_length = leading + buffer.size();
    if (aligned_length % kSectorSize != 0) {
      aligned_length += kSectorSize - (aligned_length % kSectorSize);
    }
    if (aligned_length > static_cast<std::uint64_t>(std::numeric_limits<DWORD>::max())) {
      return false;
    }

    std::vector<char> aligned(static_cast<std::size_t>(aligned_length), 0);
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
    std::copy_n(aligned.begin() + static_cast<std::ptrdiff_t>(leading), buffer.size(), buffer.begin());
    return true;
#else
    (void)offset;
    (void)buffer;
    return false;
#endif
  }

  std::string path_;
  bool device_ = false;
  std::ifstream file_;
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
};

bool has_error(const ScanResult& result) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const Diagnostic& diagnostic) {
    return diagnostic.severity == "error";
  });
}

std::string first_error(const ScanResult& result) {
  const auto it = std::find_if(result.diagnostics.begin(), result.diagnostics.end(), [](const Diagnostic& diagnostic) {
    return diagnostic.severity == "error";
  });
  if (it == result.diagnostics.end()) {
    return {};
  }
  return it->code + ": " + it->message;
}

std::string lv_path(const VolumeGroup& vg, const LogicalVolume& lv) {
  return vg.name + "/" + lv.name;
}

const LogicalVolume* find_target_lv(const ScanResult& result, const std::string& target, const VolumeGroup** vg_out) {
  const LogicalVolume* found = nullptr;
  const VolumeGroup* found_vg = nullptr;
  for (const VolumeGroup& vg : result.vgs) {
    for (const LogicalVolume& lv : vg.lvs) {
      const bool matches_full = target == lv_path(vg, lv);
      const bool matches_name = target == lv.name;
      if (!matches_full && !matches_name) {
        continue;
      }
      if (found != nullptr) {
        return nullptr;
      }
      found = &lv;
      found_vg = &vg;
    }
  }
  if (vg_out) {
    *vg_out = found_vg;
  }
  return found;
}

Result<std::uint64_t> process_logical_volume_range(const ExportOptions& options,
                                                   const std::function<Status(std::span<const char>)>& sink) {
  if (!options.readonly) {
    return Result<std::uint64_t>::failure("LV range processing is read-only only");
  }
  if (options.target.empty()) {
    return Result<std::uint64_t>::failure("LV range processing requires an LV target such as vg/lv");
  }

  auto scanned = scan_images(options.scan_options);
  if (!scanned) {
    return Result<std::uint64_t>::failure(scanned.status.message);
  }
  if (has_error(scanned.value)) {
    return Result<std::uint64_t>::failure("refusing export because scan failed closed: " + first_error(scanned.value));
  }

  const VolumeGroup* target_vg = nullptr;
  const LogicalVolume* target_lv = find_target_lv(scanned.value, options.target, &target_vg);
  if (!target_lv || !target_vg) {
    return Result<std::uint64_t>::failure("LV target not found or ambiguous: " + options.target);
  }
  if (!target_lv->supported) {
    return Result<std::uint64_t>::failure("LV target is unsupported: " + target_lv->unsupported_reason);
  }
  if (options.offset_bytes > target_lv->size_bytes) {
    return Result<std::uint64_t>::failure("export offset is beyond the LV size");
  }
  const std::uint64_t export_length = options.length_bytes.value_or(target_lv->size_bytes - options.offset_bytes);
  if (export_length > target_lv->size_bytes - options.offset_bytes) {
    return Result<std::uint64_t>::failure("export range exceeds the LV size");
  }
  if (export_length == 0) {
    return Result<std::uint64_t>::failure("export length must be greater than zero");
  }
  const std::uint64_t export_end = options.offset_bytes + export_length;

  std::vector<MappingRange> mappings;
  for (const MappingRange& mapping : scanned.value.mappings) {
    if (mapping.vg_name == target_vg->name && mapping.lv_name == target_lv->name && mapping.lv_uuid == target_lv->uuid) {
      mappings.push_back(mapping);
    }
  }
  std::sort(mappings.begin(), mappings.end(), [](const MappingRange& lhs, const MappingRange& rhs) {
    return lhs.lv_start_bytes < rhs.lv_start_bytes;
  });

  std::uint64_t expected_offset = 0;
  for (const MappingRange& mapping : mappings) {
    if (mapping.lv_start_bytes != expected_offset) {
      return Result<std::uint64_t>::failure("LV mappings are not contiguous");
    }
    if (mapping.length_bytes > std::numeric_limits<std::uint64_t>::max() - expected_offset) {
      return Result<std::uint64_t>::failure("LV mapping length overflow");
    }
    expected_offset += mapping.length_bytes;
  }
  if (expected_offset != target_lv->size_bytes) {
    return Result<std::uint64_t>::failure("LV mappings do not cover the full LV");
  }

  std::vector<MappingRange> export_mappings;
  for (const MappingRange& mapping : mappings) {
    const std::uint64_t mapping_end = mapping.lv_start_bytes + mapping.length_bytes;
    const std::uint64_t piece_start = std::max(options.offset_bytes, mapping.lv_start_bytes);
    const std::uint64_t piece_end = std::min(export_end, mapping_end);
    if (piece_start >= piece_end) {
      continue;
    }

    MappingRange piece = mapping;
    piece.lv_start_bytes = piece_start;
    piece.length_bytes = piece_end - piece_start;
    piece.source_offset_bytes = mapping.source_offset_bytes + (piece_start - mapping.lv_start_bytes);
    piece.pv_start_bytes = mapping.pv_start_bytes + (piece_start - mapping.lv_start_bytes);
    export_mappings.push_back(piece);
  }
  if (export_mappings.empty()) {
    return Result<std::uint64_t>::failure("export range did not intersect LV mappings");
  }

  std::map<std::string, bool> source_is_device;
  for (const InputImage& input : scanned.value.inputs) {
    source_is_device[input.path] = input.source_type == "device";
  }

  std::map<std::string, SourceInput> inputs;
  for (const MappingRange& mapping : export_mappings) {
    if (!inputs.contains(mapping.source_path)) {
      SourceInput source(mapping.source_path, source_is_device[mapping.source_path]);
      Status status = source.open();
      if (!status.ok) {
        return Result<std::uint64_t>::failure(status.message);
      }
      inputs.emplace(mapping.source_path, std::move(source));
    }
  }

  std::vector<char> buffer;
  std::uint64_t exported = 0;
  for (const MappingRange& mapping : export_mappings) {
    SourceInput& source = inputs.at(mapping.source_path);
    std::uint64_t remaining = mapping.length_bytes;
    std::uint64_t source_offset = mapping.source_offset_bytes;
    while (remaining > 0) {
      const std::uint64_t chunk = std::min(remaining, kCopyChunkBytes);
      if (!source.read(source_offset, chunk, buffer)) {
        return Result<std::uint64_t>::failure("failed to read source during LV range processing");
      }
      Status sink_status = sink(std::span<const char>(buffer.data(), buffer.size()));
      if (!sink_status.ok) {
        return Result<std::uint64_t>::failure(sink_status.message);
      }
      source_offset += chunk;
      remaining -= chunk;
      exported += chunk;
    }
  }

  return Result<std::uint64_t>::success(exported);
}

}  // namespace

Result<std::uint64_t> export_logical_volume(const ExportOptions& options) {
  if (options.output_path.empty()) {
    return Result<std::uint64_t>::failure("export requires --output <path>");
  }
  if (std::filesystem::exists(options.output_path)) {
    return Result<std::uint64_t>::failure("refusing to overwrite existing output file: " + options.output_path);
  }

  std::ofstream output(options.output_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return Result<std::uint64_t>::failure("cannot create output file: " + options.output_path);
  }

  auto processed = process_logical_volume_range(options, [&](std::span<const char> bytes) {
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
      return Status::failure("failed to write output file during export");
    }
   return Status::success();
 });
 if (!processed) {
    output.close();
    std::error_code ec;
    std::filesystem::remove(options.output_path, ec);
    return processed;
 }
 output.flush();
  if (!output) {
    return Result<std::uint64_t>::failure("failed to flush output file after export");
  }
  return processed;
}

Result<HashResult> hash_logical_volume(const ExportOptions& options) {
  Sha256Hasher hasher;
  auto processed = process_logical_volume_range(options, [&](std::span<const char> bytes) {
    return hasher.update(bytes);
  });
  if (!processed) {
    return Result<HashResult>::failure(processed.status.message);
  }
  auto hex = hasher.finish_hex();
  if (!hex) {
    return Result<HashResult>::failure(hex.status.message);
  }
  return Result<HashResult>::success({"sha256", hex.value, processed.value});
}

}  // namespace winlvm
