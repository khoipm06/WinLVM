#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace winlvm::installer {

inline constexpr std::array<uint8_t, 8> kArchiveMagic = {
    'W', 'L', 'V', 'M', 'I', 'N', 'S', 'T'};
inline constexpr uint32_t kArchiveVersion = 1u;
inline constexpr std::array<uint8_t, 4> kEntryMagic = {'W', 'L', 'E', '1'};

inline constexpr size_t kEntryHeaderSize = 64u;
inline constexpr size_t kFooterSize = 64u;
inline constexpr uint32_t kMaxArchiveEntries = 4096u;
inline constexpr uint64_t kMaxEntryBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr uint64_t kMaxTotalUnpackedBytes = 512ULL * 1024ULL * 1024ULL;

enum class CompressionMethod : uint16_t {
  Store = 0u,
  Deflate = 1u,
};

struct Sha256State {
  uint32_t h[8]{};
  uint64_t total_bytes = 0u;
  uint8_t block[64]{};
  size_t block_len = 0u;

  Sha256State();
  void reset();
  void update(const uint8_t* data, size_t len);
  std::array<uint8_t, 32> finish();
};

uint32_t crc32_ieee(const uint8_t* data, size_t len);
std::array<uint8_t, 32> sha256_bytes(const uint8_t* data, size_t len);
std::string sha256_hex(const std::array<uint8_t, 32>& digest);

bool validate_relative_path(const std::string& path, std::string& error);

bool inflate_raw(const uint8_t* src,
                 size_t src_len,
                 size_t expected_out_len,
                 std::vector<uint8_t>& out,
                 std::string& error);

struct ArchiveFooter {
  uint32_t version = 0u;
  uint32_t entry_count = 0u;
  uint64_t payload_offset = 0u;
  uint64_t payload_size = 0u;
  std::array<uint8_t, 32> archive_sha256{};
};

struct ArchiveEntryMeta {
  std::string path;
  CompressionMethod compression = CompressionMethod::Store;
  uint64_t raw_size = 0u;
  uint64_t stored_size = 0u;
  uint32_t raw_crc32 = 0u;
  std::array<uint8_t, 32> raw_sha256{};
  uint64_t data_offset = 0u;
};

struct ParsedArchive {
  ArchiveFooter footer{};
  std::vector<ArchiveEntryMeta> entries;
  uint64_t total_unpacked_bytes = 0u;

  const ArchiveEntryMeta* find_entry(const std::string& path) const;
};

bool parse_and_verify_archive_buffer(const uint8_t* buf,
                                     size_t buf_len,
                                     ParsedArchive& out,
                                     std::string& error);

bool unpack_entry_from_buffer(const uint8_t* buf,
                              size_t buf_len,
                              const ArchiveEntryMeta& entry,
                              std::vector<uint8_t>& out,
                              std::string& error);

bool unpack_all_from_buffer(
    const uint8_t* buf,
    size_t buf_len,
    std::vector<std::pair<std::string, std::vector<uint8_t>>>& out,
    std::string& error);

bool parse_and_verify_archive_file(const std::filesystem::path& archive_path,
                                   ParsedArchive& out,
                                   std::string& error);

bool read_entry_from_file(const std::filesystem::path& archive_path,
                          const ArchiveEntryMeta& entry,
                          std::vector<uint8_t>& out,
                          std::string& error);

bool verify_archive_file_full(const std::filesystem::path& archive_path,
                              ParsedArchive& out,
                              std::string& error);

bool pack_archive_to_buffer(
    const std::vector<uint8_t>& stub,
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files,
    std::vector<uint8_t>& out,
    std::string& error);

}  // namespace winlvm::installer
