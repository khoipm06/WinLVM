// A randomly-addressable stream of bytes.
//
// Layers above this interface do not distinguish image files, physical disks,
// or stacked block volumes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lvmfmt {

class ByteSource {
 public:
  virtual ~ByteSource() = default;

  // Reads exactly `length` bytes at `offset`. Returns false on short read,
  // out-of-range access, or I/O error. Never partially succeeds.
  virtual bool read(std::uint64_t offset, void* buffer, std::size_t length) = 0;
  virtual bool read(std::uint64_t offset, void* buffer, std::size_t length, std::string& error);

  // Writes exactly `length` bytes at `offset`. Returns false on I/O error or if read-only.
  virtual bool write(std::uint64_t offset, const void* buffer, std::size_t length);
  virtual bool write(std::uint64_t offset, const void* buffer, std::size_t length, std::string& error);

  virtual std::uint64_t size() const = 0;

  // Underlying block device sector geometry (512 for regular image files).
  virtual std::uint32_t logical_sector_size() const { return 512; }
  virtual std::uint32_t physical_sector_size() const { return 512; }

  // Convenience wrapper returning an owned buffer.
  bool read_vec(std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& out);
};

// Opens a file for read-only access. Returns nullptr and sets `error` on failure.
std::unique_ptr<ByteSource> open_file_source(const std::string& path, std::string& error);

// Opens an existing file for read-write access without truncating or creating.
std::unique_ptr<ByteSource> open_file_source_rw(const std::string& path, std::string& error);

// Generic storage opener (read-only or read-write, supports image files and raw devices).
std::unique_ptr<ByteSource> open_storage_source(const std::string& path, bool read_write, std::string& error);

}  // namespace lvmfmt
