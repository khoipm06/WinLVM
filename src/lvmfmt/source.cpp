#include "lvmfmt/source.h"

#include <cstdio>
#include <cstring>

#include "lvmfmt/bytes.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
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
#if defined(_WIN32)
    if (_fseeki64(file_, static_cast<long long>(offset), SEEK_SET) != 0) return false;
#else
    if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
#endif
    if (std::fwrite(buffer, 1, length, file_) != length) return false;
    std::fflush(file_);
    if (offset + length > size_) {
      size_ = offset + length;
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
class Win32DeviceSource final : public ByteSource {
 public:
  explicit Win32DeviceSource(HANDLE handle, std::uint64_t size, bool writable)
      : handle_(handle), size_(size), writable_(writable) {}
  ~Win32DeviceSource() override {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }

  Win32DeviceSource(const Win32DeviceSource&) = delete;
  Win32DeviceSource& operator=(const Win32DeviceSource&) = delete;

  bool read(std::uint64_t offset, void* buffer, std::size_t length) override {
    if (length == 0) return true;
    constexpr std::uint64_t kSector = 512;
    std::uint64_t start_aligned = (offset / kSector) * kSector;
    std::uint64_t end_aligned = ((offset + length + kSector - 1) / kSector) * kSector;
    std::size_t aligned_len = static_cast<std::size_t>(end_aligned - start_aligned);

    LARGE_INTEGER li;
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
    if (bytes_read < (offset - start_aligned) + length) return false;
    std::memcpy(buffer, bounce.data() + (offset - start_aligned), length);
    return true;
  }

  bool write(std::uint64_t offset, const void* buffer, std::size_t length) override {
    if (length == 0) return true;
    if (!writable_) return false;
    constexpr std::uint64_t kSector = 512;
    std::uint64_t start_aligned = (offset / kSector) * kSector;
    std::uint64_t end_aligned = ((offset + length + kSector - 1) / kSector) * kSector;
    std::size_t aligned_len = static_cast<std::size_t>(end_aligned - start_aligned);

    if (offset == start_aligned && length == aligned_len) {
      LARGE_INTEGER li;
      li.QuadPart = static_cast<LONGLONG>(offset);
      if (!SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) return false;

      DWORD bytes_written = 0;
      if (!WriteFile(handle_, buffer, static_cast<DWORD>(length), &bytes_written, nullptr)) {
        return false;
      }
      return bytes_written == length;
    }

    std::vector<std::uint8_t> bounce(aligned_len, 0);
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(start_aligned);
    if (!SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) return false;

    DWORD bytes_read = 0;
    ReadFile(handle_, bounce.data(), static_cast<DWORD>(aligned_len), &bytes_read, nullptr);

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

 private:
  HANDLE handle_;
  std::uint64_t size_;
  bool writable_;
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
  if (fopen_s(&f, path.c_str(), "r+b") != 0) {
    if (fopen_s(&f, path.c_str(), "w+b") != 0) f = nullptr;
  }
#else
  f = std::fopen(path.c_str(), "r+b");
  if (!f) {
    f = std::fopen(path.c_str(), "w+b");
  }
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
    return std::make_unique<Win32DeviceSource>(h, dev_size, read_write);
  }
#endif

  if (read_write) {
    return open_file_source_rw(path, error);
  } else {
    return open_file_source(path, error);
  }
}

}  // namespace lvmfmt
