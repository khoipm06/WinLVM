#pragma once

#include "winlvm/result.h"

#include <cstdint>
#include <span>
#include <vector>

namespace winlvm {

struct DriverMappingStatus {
  std::uint64_t mapping_id = 0;
  std::uint32_t status = 0;
  std::uint64_t io_count = 0;
  std::uint64_t mapping_table_size = 0;
};

Result<std::vector<std::uint64_t>> parse_driver_mapping_list(
    std::span<const std::uint8_t> bytes);
Result<DriverMappingStatus> parse_driver_mapping_query(
    std::span<const std::uint8_t> bytes);

class DriverClient {
 public:
  DriverClient() = default;
  ~DriverClient();

  DriverClient(const DriverClient&) = delete;
  DriverClient& operator=(const DriverClient&) = delete;

  DriverClient(DriverClient&& other) noexcept;
  DriverClient& operator=(DriverClient&& other) noexcept;

  static Result<DriverClient> open_default();

  Result<std::uint64_t> create_mapping(std::span<const std::uint8_t> mapping_table);
  Status delete_mapping(std::uint64_t mapping_id);
  Result<std::vector<std::uint64_t>> list_mappings();
  Result<DriverMappingStatus> query_mapping(std::uint64_t mapping_id);

 private:
  explicit DriverClient(void* handle);

  void close();

  void* handle_ = nullptr;
};

}  // namespace winlvm
