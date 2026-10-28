#ifndef WINLVM_SERVICE_PROTOCOL_H
#define WINLVM_SERVICE_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace winlvm::service {

constexpr std::uint32_t kProtocolVersion = 1;
constexpr std::size_t kMaxFramePayload = 64U * 1024U;

enum class Operation : std::uint32_t { Inventory = 1, RawWrite = 2, UpdateLv = 3, Refresh = 4 };
enum class ErrorCode : std::uint32_t {
  Ok = 0,
  MalformedFrame = 1,
  Unsupported = 2,
  DriverUnavailable = 3
};

struct Request {
  Operation operation;
  std::uint64_t offset;
  std::uint32_t length;
};

struct ParseResult {
  bool ok;
  Request request{};
  ErrorCode error{};
};

ParseResult parse_frame(std::span<const std::byte> frame);
ErrorCode execute_request(const Request& request, bool driver_available);
std::string_view error_name(ErrorCode error);

}  // namespace winlvm::service
#endif
