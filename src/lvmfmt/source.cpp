#include "lvmfmt/source.h"

#include <cstdio>

#include "lvmfmt/bytes.h"

namespace lvmfmt {

bool ByteSource::read_vec(std::uint64_t offset, std::size_t length,
                          std::vector<std::uint8_t>& out) {
  out.assign(length, 0);
  if (length == 0) return true;
  return read(offset, out.data(), length);
}

namespace {

class FileSource final : public ByteSource {
 public:
  explicit FileSource(std::FILE* f, std::uint64_t size) : file_(f), size_(size) {}
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

  std::uint64_t size() const override { return size_; }

 private:
  std::FILE* file_;
  std::uint64_t size_;
};

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
  return std::make_unique<FileSource>(f, static_cast<std::uint64_t>(end));
}

}  // namespace lvmfmt
