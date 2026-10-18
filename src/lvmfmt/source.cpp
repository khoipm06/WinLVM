#include "lvmfmt/source.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "lvmfmt/bytes.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace lvmfmt {

bool ByteSource::read(std::uint64_t offset, void* buffer, std::size_t length, std::string& error) {
  if (!read(offset, buffer, length)) {
    error = "read failed at offset " + std::to_string(offset) + " for " + std::to_string(length) + " bytes";
    return false;
  }
  return true;
}

bool ByteSource::write(std::uint64_t offset, const void* buffer, std::size_t length) {
  (void)offset;
  (void)buffer;
  (void)length;
  return false;
}

bool ByteSource::write(std::uint64_t offset, const void* buffer, std::size_t length, std::string& error) {
  if (!write(offset, buffer, length)) {
    error = "write failed at offset " + std::to_string(offset) + " for " + std::to_string(length) + " bytes";
    return false;
  }
  return true;
}

bool ByteSource::read_vec(std::uint64_t offset, std::size_t length,
                          std::vector<std::uint8_t>& out) {
  out.assign(length, 0);
  if (length == 0) return true;
  return read(offset, out.data(), length);
}

namespace {

class FileSource final : public ByteSource {
 public:
  explicit FileSource(std::FILE* f, std::uint64_t size, bool writable)
      : file_(f), size_(size), writable_(writable) {}
  ~FileSource() override {
    if (file_) std::fclose(file_);
  }

  FileSource(const FileSource&) = delete;
  FileSource& operator=(const FileSource&) = delete;

  bool read(std::uint64_t offset, void* buffer, std::size_t length) override {
    if (length == 0) return true;
    if (!range_fits(offset, length, size_)) return false;
#if defined(_WIN32)
    if (_fseeki64(file_, static_cast<long long>(offset), SEEK_SET) != 0) return false;
#else
    if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
#endif
    return std::fread(buffer, 1, length, file_) == length;
  }

  bool write(std::uint64_t offset, const void* buffer, std::size_t length) override {
    if (length == 0) return true;
    if (!writable_) return false;
    auto end = checked_add(offset, length);
    if (!end) return false;
#if defined(_WIN32)
    if (_fseeki64(file_, static_cast<long long>(offset), SEEK_SET) != 0) return false;
#else
    if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
#endif
    if (std::fwrite(buffer, 1, length, file_) != length) return false;
    std::fflush(file_);
    if (*end > size_) {
      size_ = *end;
    }
    return true;
  }

  std::uint64_t size() const override { return size_; }

 private:
  std::FILE* file_;
  std::uint64_t size_;
  bool writable_;
};

#if defined(_WIN32)

bool is_valid_sector_size(std::uint32_t s) {
  return s >= 512 && s <= 65536 && (s & (s - 1)) == 0;
}

void query_win32_sector_sizes(HANDLE h, std::uint32_t& out_logical, std::uint32_t& out_physical) {
  out_logical = 512;
  out_physical = 512;

  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageAccessAlignmentProperty;
  query.QueryType = PropertyStandardQuery;
  STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR align{};
  DWORD returned = 0;
  if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                      &align, sizeof(align), &returned, nullptr) &&
      returned >= sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR) &&
      is_valid_sector_size(align.BytesPerLogicalSector)) {
    out_logical = align.BytesPerLogicalSector;
    if (is_valid_sector_size(align.BytesPerPhysicalSector) &&
        align.BytesPerPhysicalSector >= out_logical) {
      out_physical = align.BytesPerPhysicalSector;
    } else {
      out_physical = out_logical;
    }
    return;
  }

  DISK_GEOMETRY_EX geo_ex{};
  if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                      &geo_ex, sizeof(geo_ex), &returned, nullptr) &&
      is_valid_sector_size(geo_ex.Geometry.BytesPerSector)) {
    out_logical = geo_ex.Geometry.BytesPerSector;
    out_physical = out_logical;
    return;
  }

  DISK_GEOMETRY geo{};
  if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0,
                      &geo, sizeof(geo), &returned, nullptr) &&
      is_valid_sector_size(geo.BytesPerSector)) {
    out_logical = geo.BytesPerSector;
    out_physical = out_logical;
    return;
  }
}

bool query_volume_disk_numbers(HANDLE h, std::vector<DWORD>& out_disks) {
  std::vector<std::uint8_t> buf(sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT), 0);
  DWORD returned = 0;
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                        buf.data(), static_cast<DWORD>(buf.size()), &returned, nullptr)) {
      const auto* vde = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buf.data());
      for (DWORD i = 0; i < vde->NumberOfDiskExtents; ++i) {
        out_disks.push_back(vde->Extents[i].DiskNumber);
      }
      return !out_disks.empty();
    }
    if (GetLastError() == ERROR_MORE_DATA && returned >= sizeof(VOLUME_DISK_EXTENTS)) {
      const auto* vde = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buf.data());
      std::size_t needed = sizeof(VOLUME_DISK_EXTENTS) +
                           static_cast<std::size_t>(vde->NumberOfDiskExtents) * sizeof(DISK_EXTENT);
      if (needed > buf.size() && needed <= 1024 * 1024) {
        buf.assign(needed, 0);
        continue;
      }
    }
    break;
  }

  STORAGE_DEVICE_NUMBER sdn{};
  if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                      &sdn, sizeof(sdn), &returned, nullptr) &&
      sdn.DeviceType == FILE_DEVICE_DISK) {
    out_disks.push_back(sdn.DeviceNumber);
    return true;
  }
  return false;
}

std::vector<DWORD> get_windows_system_disk_numbers() {
  std::vector<DWORD> sys_disks;
  wchar_t sys_dir[MAX_PATH]{};
  UINT len = GetSystemDirectoryW(sys_dir, MAX_PATH);
  if (len < 2 || sys_dir[1] != L':') {
    return sys_disks;
  }
  wchar_t vol_path[8] = {L'\\', L'\\', L'.', L'\\', sys_dir[0], L':', L'\0', L'\0'};
  HANDLE h = CreateFileW(vol_path, 0,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return sys_disks;
  }
  query_volume_disk_numbers(h, sys_disks);
  CloseHandle(h);
  return sys_disks;
}

std::optional<DWORD> parse_physical_drive_number(const std::string& path) {
  std::string lower;
  lower.reserve(path.size());
  for (char c : path) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  const std::string p1 = "\\\\.\\physicaldrive";
  const std::string p2 = "\\\\?\\physicaldrive";
  std::size_t pos = 0;
  if (lower.rfind(p1, 0) == 0) {
    pos = p1.size();
  } else if (lower.rfind(p2, 0) == 0) {
    pos = p2.size();
  } else {
    return std::nullopt;
  }
  if (pos >= lower.size()) return std::nullopt;
  DWORD num = 0;
  for (std::size_t i = pos; i < lower.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(lower[i]))) return std::nullopt;
    num = num * 10u + static_cast<DWORD>(lower[i] - '0');
  }
  return num;
}

bool is_system_disk_target(HANDLE h, const std::string& path, DWORD& matched_disk) {
  const auto sys_disks = get_windows_system_disk_numbers();
  if (sys_disks.empty()) return false;

  if (auto parsed = parse_physical_drive_number(path)) {
    if (std::find(sys_disks.begin(), sys_disks.end(), *parsed) != sys_disks.end()) {
      matched_disk = *parsed;
      return true;
    }
  }

  std::vector<DWORD> target_disks;
  if (query_volume_disk_numbers(h, target_disks)) {
    for (DWORD d : target_disks) {
      if (std::find(sys_disks.begin(), sys_disks.end(), d) != sys_disks.end()) {
        matched_disk = d;
        return true;
      }
    }
  }
  return false;
}

class Win32DeviceSource final : public ByteSource {
 public:
  Win32DeviceSource(HANDLE handle, std::uint64_t size, bool writable,
                    std::uint32_t logical_sector, std::uint32_t physical_sector)
      : handle_(handle),
        size_(size),
        writable_(writable),
        logical_sector_(logical_sector),
        physical_sector_(physical_sector) {}
  ~Win32DeviceSource() override {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }

  Win32DeviceSource(const Win32DeviceSource&) = delete;
  Win32DeviceSource& operator=(const Win32DeviceSource&) = delete;

  bool read(std::uint64_t offset, void* buffer, std::size_t length) override {
    if (length == 0) return true;
    if (size_ > 0 && !range_fits(offset, length, size_)) return false;

    const std::uint64_t sec = logical_sector_;
    auto end = checked_add(offset, length);
    if (!end) return false;
    auto end_padded = checked_add(*end, sec - 1);
    if (!end_padded) return false;

    const std::uint64_t start_aligned = (offset / sec) * sec;
    const std::uint64_t end_aligned = (*end_padded / sec) * sec;
    const std::uint64_t aligned_len_u64 = end_aligned - start_aligned;
    if (aligned_len_u64 > 0x7fffffffULL) return false;
    const std::size_t aligned_len = static_cast<std::size_t>(aligned_len_u64);

    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(start_aligned);
    if (!SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) return false;

    if (offset == start_aligned && length == aligned_len) {
      DWORD bytes_read = 0;
      if (!ReadFile(handle_, buffer, static_cast<DWORD>(length), &bytes_read, nullptr)) {
        return false;
      }
      return bytes_read == length;
    }

    std::vector<std::uint8_t> bounce(aligned_len);
    DWORD bytes_read = 0;
    if (!ReadFile(handle_, bounce.data(), static_cast<DWORD>(aligned_len), &bytes_read, nullptr)) {
      return false;
    }
    const std::uint64_t skip = offset - start_aligned;
    if (bytes_read < skip + length) return false;
    std::memcpy(buffer, bounce.data() + skip, length);
    return true;
  }

  bool write(std::uint64_t offset, const void* buffer, std::size_t length) override {
    if (length == 0) return true;
    if (!writable_) return false;
    if (size_ > 0 && !range_fits(offset, length, size_)) return false;

    const std::uint64_t sec = logical_sector_;
    auto end = checked_add(offset, length);
    if (!end) return false;
    auto end_padded = checked_add(*end, sec - 1);
    if (!end_padded) return false;

    const std::uint64_t start_aligned = (offset / sec) * sec;
    const std::uint64_t end_aligned = (*end_padded / sec) * sec;
    const std::uint64_t aligned_len_u64 = end_aligned - start_aligned;
    if (aligned_len_u64 > 0x7fffffffULL) return false;
    const std::size_t aligned_len = static_cast<std::size_t>(aligned_len_u64);

    if (offset == start_aligned && length == aligned_len) {
      LARGE_INTEGER li{};
      li.QuadPart = static_cast<LONGLONG>(offset);
      if (!SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) return false;

      DWORD bytes_written = 0;
      if (!WriteFile(handle_, buffer, static_cast<DWORD>(length), &bytes_written, nullptr)) {
        return false;
      }
      return bytes_written == length;
    }

    // Read-modify-write whole logical sectors so unaligned metadata updates
    // preserve neighboring bytes (required for 4Kn disks and 4Kn WinLUKS).
    std::vector<std::uint8_t> bounce(aligned_len, 0);
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(start_aligned);
    if (!SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) return false;

    DWORD bytes_read = 0;
    if (!ReadFile(handle_, bounce.data(), static_cast<DWORD>(aligned_len), &bytes_read, nullptr) ||
        bytes_read != aligned_len) {
      return false;
    }

    std::memcpy(bounce.data() + (offset - start_aligned), buffer, length);

    li.QuadPart = static_cast<LONGLONG>(start_aligned);
    if (!SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) return false;

    DWORD bytes_written = 0;
    if (!WriteFile(handle_, bounce.data(), static_cast<DWORD>(aligned_len), &bytes_written, nullptr)) {
      return false;
    }
    return bytes_written == aligned_len;
  }

  std::uint64_t size() const override { return size_; }
  std::uint32_t logical_sector_size() const override { return logical_sector_; }
  std::uint32_t physical_sector_size() const override { return physical_sector_; }

 private:
  HANDLE handle_;
  std::uint64_t size_;
  bool writable_;
  std::uint32_t logical_sector_;
  std::uint32_t physical_sector_;
};
#endif

}  // namespace

std::unique_ptr<ByteSource> open_file_source(const std::string& path, std::string& error) {
  std::FILE* f = nullptr;
#if defined(_WIN32)
  if (fopen_s(&f, path.c_str(), "rb") != 0) f = nullptr;
#else
  f = std::fopen(path.c_str(), "rb");
#endif
  if (!f) {
    error = "cannot open " + path;
    return nullptr;
  }

#if defined(_WIN32)
  if (_fseeki64(f, 0, SEEK_END) != 0) {
    std::fclose(f);
    error = "cannot seek " + path;
    return nullptr;
  }
  const long long end = _ftelli64(f);
#else
  if (fseeko(f, 0, SEEK_END) != 0) {
    std::fclose(f);
    error = "cannot seek " + path;
    return nullptr;
  }
  const off_t end = ftello(f);
#endif
  if (end < 0) {
    std::fclose(f);
    error = "cannot size " + path;
    return nullptr;
  }
  return std::make_unique<FileSource>(f, static_cast<std::uint64_t>(end), false);
}

std::unique_ptr<ByteSource> open_file_source_rw(const std::string& path, std::string& error) {
  std::FILE* f = nullptr;
#if defined(_WIN32)
  if (fopen_s(&f, path.c_str(), "r+b") != 0) f = nullptr;
#else
  f = std::fopen(path.c_str(), "r+b");
#endif
  if (!f) {
    error = "cannot open " + path + " for read-write";
    return nullptr;
  }

#if defined(_WIN32)
  if (_fseeki64(f, 0, SEEK_END) != 0) {
    std::fclose(f);
    error = "cannot seek " + path;
    return nullptr;
  }
  const long long end = _ftelli64(f);
#else
  if (fseeko(f, 0, SEEK_END) != 0) {
    std::fclose(f);
    error = "cannot seek " + path;
    return nullptr;
  }
  const off_t end = ftello(f);
#endif
  if (end < 0) {
    std::fclose(f);
    error = "cannot size " + path;
    return nullptr;
  }
  return std::make_unique<FileSource>(f, static_cast<std::uint64_t>(end), true);
}

std::unique_ptr<ByteSource> open_storage_source(const std::string& path, bool read_write, std::string& error) {
#if defined(_WIN32)
  if (path.rfind("\\\\.\\", 0) == 0 || path.rfind("\\\\?\\", 0) == 0) {
    DWORD access = GENERIC_READ;
    if (read_write) access |= GENERIC_WRITE;
    HANDLE h = CreateFileA(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      error = "failed to open device " + path + " (error " + std::to_string(GetLastError()) + ")";
      return nullptr;
    }

    if (read_write) {
      DWORD matched_disk = 0;
      if (is_system_disk_target(h, path, matched_disk)) {
        CloseHandle(h);
        error = "refusing writable access to active Windows boot/system disk (PhysicalDrive" +
                std::to_string(matched_disk) + ")";
        return nullptr;
      }
      DWORD dummy = 0;
      DeviceIoControl(h, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &dummy, nullptr);
    }

    GET_LENGTH_INFORMATION gli{};
    DWORD returned = 0;
    std::uint64_t dev_size = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &gli, sizeof(gli), &returned, nullptr)) {
      dev_size = static_cast<std::uint64_t>(gli.Length.QuadPart);
    } else {
      DISK_GEOMETRY_EX geo{};
      if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof(geo), &returned, nullptr)) {
        dev_size = static_cast<std::uint64_t>(geo.DiskSize.QuadPart);
      }
    }

    std::uint32_t logical_sector = 512;
    std::uint32_t physical_sector = 512;
    query_win32_sector_sizes(h, logical_sector, physical_sector);

    return std::make_unique<Win32DeviceSource>(h, dev_size, read_write, logical_sector, physical_sector);
  }
#endif

  if (read_write) {
    return open_file_source_rw(path, error);
  }
  return open_file_source(path, error);
}

}  // namespace lvmfmt
