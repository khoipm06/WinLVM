#include "testing.h"
#include "service/protocol.h"
#include "service/mutation.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {
std::byte b(std::uint8_t value) { return static_cast<std::byte>(value); }
void put32(std::vector<std::byte>& frame, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) frame.push_back(b(static_cast<std::uint8_t>(value >> (i * 8))));
}
void put64(std::vector<std::byte>& frame, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) frame.push_back(b(static_cast<std::uint8_t>(value >> (i * 8))));
}
}

TEST(service_rejects_truncated_frame) {
  const auto result = winlvm::service::parse_frame({});
  CHECK(!result.ok);
  CHECK(result.error == winlvm::service::ErrorCode::MalformedFrame);
}

TEST(service_rejects_oversized_frame) {
  std::vector<std::byte> frame;
  put32(frame, 1);
  put32(frame, static_cast<std::uint32_t>(winlvm::service::Operation::Inventory));
  put32(frame, 0xffffffffU);
  put64(frame, 0);
  CHECK(!winlvm::service::parse_frame(frame).ok);
}

TEST(service_rejects_raw_write_without_driver) {
  const winlvm::service::Request request{winlvm::service::Operation::RawWrite, 0, 512};
  CHECK(winlvm::service::execute_request(request, false) == winlvm::service::ErrorCode::DriverUnavailable);
}

TEST(service_rejects_raw_write_even_with_driver_until_spike_exists) {
  const winlvm::service::Request request{winlvm::service::Operation::RawWrite, 0, 512};
  CHECK(winlvm::service::execute_request(request, true) == winlvm::service::ErrorCode::Unsupported);
}

TEST(service_rejects_update_lv_without_driver) {
  const winlvm::service::Request request{winlvm::service::Operation::UpdateLv, 0, 64};
  CHECK(winlvm::service::execute_request(request, false) == winlvm::service::ErrorCode::DriverUnavailable);
}

TEST(service_rejects_update_lv_until_backend_exists) {
  const winlvm::service::Request request{winlvm::service::Operation::UpdateLv, 0, 64};
  CHECK(winlvm::service::execute_request(request, true) == winlvm::service::ErrorCode::Unsupported);
}

TEST(service_rejects_unknown_operation) {
  std::vector<std::byte> frame;
  put32(frame, 1);
  put32(frame, 99);
  put32(frame, 0);
  put64(frame, 0);
  CHECK(!winlvm::service::parse_frame(frame).ok);
}

TEST(mutation_lock_is_exclusive_per_lv) {
  winlvm::service::MutationError first_error = winlvm::service::MutationError::Unsupported;
  const auto first = winlvm::service::MutationLock::acquire("vg/lv", first_error);
  CHECK(first.owns_lock());
  winlvm::service::MutationError second_error = winlvm::service::MutationError::Unsupported;
  const auto second = winlvm::service::MutationLock::acquire("vg/lv", second_error);
  CHECK(!second.owns_lock());
  CHECK(second_error == winlvm::service::MutationError::Busy);
}

TEST(mutation_rejects_invalid_geometry) {
  const winlvm::service::Geometry geometry{4096, 2048, 4096, 512};
  CHECK(winlvm::service::validate_geometry(winlvm::service::MutationKind::Shrink, geometry) ==
        winlvm::service::MutationError::InvalidGeometry);
}

TEST(mutation_refuses_without_backend) {
  const winlvm::service::Geometry geometry{4096, 8192, 2048, 512};
  CHECK(winlvm::service::mutate(winlvm::service::MutationKind::Extend, "vg/lv", geometry, false) ==
        winlvm::service::MutationError::Unsupported);
}

TEST(mutation_refuses_backend_path_even_when_available) {
  const winlvm::service::Geometry geometry{4096, 2048, 1024, 512};
  CHECK(winlvm::service::mutate(winlvm::service::MutationKind::Shrink, "vg/lv", geometry, true) ==
        winlvm::service::MutationError::Unsupported);
}

TEST(service_parses_and_executes_inventory_and_refresh) {
  // Inventory frame
  std::vector<std::byte> inv_frame;
  put32(inv_frame, winlvm::service::kProtocolVersion);
  put32(inv_frame, static_cast<std::uint32_t>(winlvm::service::Operation::Inventory));
  put32(inv_frame, 0);
  put64(inv_frame, 0);
  const auto inv_parsed = winlvm::service::parse_frame(inv_frame);
  CHECK(inv_parsed.ok);
  CHECK(inv_parsed.error == winlvm::service::ErrorCode::Ok);
  CHECK(inv_parsed.request.operation == winlvm::service::Operation::Inventory);
  CHECK(winlvm::service::execute_request(inv_parsed.request, false) ==
        winlvm::service::ErrorCode::DriverUnavailable);
  CHECK(winlvm::service::execute_request(inv_parsed.request, true) ==
        winlvm::service::ErrorCode::Ok);

  // Refresh frame
  std::vector<std::byte> ref_frame;
  put32(ref_frame, winlvm::service::kProtocolVersion);
  put32(ref_frame, static_cast<std::uint32_t>(winlvm::service::Operation::Refresh));
  put32(ref_frame, 0);
  put64(ref_frame, 0);
  const auto ref_parsed = winlvm::service::parse_frame(ref_frame);
  CHECK(ref_parsed.ok);
  CHECK(ref_parsed.error == winlvm::service::ErrorCode::Ok);
  CHECK(ref_parsed.request.operation == winlvm::service::Operation::Refresh);
  CHECK(winlvm::service::execute_request(ref_parsed.request, false) ==
        winlvm::service::ErrorCode::DriverUnavailable);
  CHECK(winlvm::service::execute_request(ref_parsed.request, true) ==
        winlvm::service::ErrorCode::Ok);

  CHECK_EQ(winlvm::service::error_name(winlvm::service::ErrorCode::Ok), std::string_view("ok"));
  CHECK_EQ(winlvm::service::error_name(winlvm::service::ErrorCode::MalformedFrame),
           std::string_view("malformed-frame"));
  CHECK_EQ(winlvm::service::error_name(winlvm::service::ErrorCode::Unsupported),
           std::string_view("unsupported"));
  CHECK_EQ(winlvm::service::error_name(winlvm::service::ErrorCode::DriverUnavailable),
           std::string_view("driver-unavailable"));
}
