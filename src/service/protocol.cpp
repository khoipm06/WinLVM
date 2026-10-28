#include "protocol.h"

#include <cstring>

namespace winlvm::service {
namespace {
constexpr std::size_t kHeaderSize = 20;

std::uint32_t read_u32(const std::byte* p) {
  std::uint32_t value = 0;
  std::memcpy(&value, p, sizeof value);
  return value;
}

std::uint64_t read_u64(const std::byte* p) {
  std::uint64_t value = 0;
  std::memcpy(&value, p, sizeof value);
  return value;
}
}  // namespace

ParseResult parse_frame(std::span<const std::byte> frame) {
  if (frame.size() < kHeaderSize) return {false, {}, ErrorCode::MalformedFrame};
  const auto version = read_u32(frame.data());
  const auto operation = read_u32(frame.data() + 4);
  const auto payload_size = read_u32(frame.data() + 8);
  if (version != kProtocolVersion || payload_size > kMaxFramePayload ||
      static_cast<std::size_t>(payload_size) + kHeaderSize != frame.size()) {
    return {false, {}, ErrorCode::MalformedFrame};
  }
  if (operation != static_cast<std::uint32_t>(Operation::Inventory) &&
      operation != static_cast<std::uint32_t>(Operation::RawWrite) &&
      operation != static_cast<std::uint32_t>(Operation::UpdateLv) &&
      operation != static_cast<std::uint32_t>(Operation::Refresh)) {
    return {false, {}, ErrorCode::MalformedFrame};
  }
  Request request{static_cast<Operation>(operation), read_u64(frame.data() + 12), payload_size};
  return {true, request, ErrorCode::Ok};
}

ErrorCode execute_request(const Request& request, bool driver_available) {
  if (!driver_available) return ErrorCode::DriverUnavailable;
  switch (request.operation) {
    case Operation::Inventory:
    case Operation::Refresh:
      return ErrorCode::Ok;
    case Operation::RawWrite:
    case Operation::UpdateLv:
      return ErrorCode::Unsupported;
  }
  return ErrorCode::Unsupported;
}

std::string_view error_name(ErrorCode error) {
  switch (error) {
    case ErrorCode::Ok:
      return "ok";
    case ErrorCode::MalformedFrame:
      return "malformed-frame";
    case ErrorCode::Unsupported:
      return "unsupported";
    case ErrorCode::DriverUnavailable:
      return "driver-unavailable";
  }
  return "unknown-error";
}

}  // namespace winlvm::service
