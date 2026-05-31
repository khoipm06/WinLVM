#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "winlvm/exporter.h"
#include "winlvm/result.h"

namespace winlvm {

class Sha256Hasher {
 public:
  Sha256Hasher();
  Sha256Hasher(const Sha256Hasher&) = delete;
  Sha256Hasher& operator=(const Sha256Hasher&) = delete;
  Sha256Hasher(Sha256Hasher&& other) noexcept;
  Sha256Hasher& operator=(Sha256Hasher&& other) noexcept;
  ~Sha256Hasher();

  Status update(std::span<const char> bytes);
  Result<std::string> finish_hex();

 private:
  void destroy();

  void* algorithm_ = nullptr;
  void* hash_ = nullptr;
  std::vector<std::uint8_t> object_;
  bool finished_ = false;
};

struct HashResult {
  std::string algorithm;
  std::string hex;
  std::uint64_t bytes_hashed = 0;
};

Result<HashResult> hash_logical_volume(const ExportOptions& options);

}  // namespace winlvm
