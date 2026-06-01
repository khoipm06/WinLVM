#include "winlvm/driver_client.h"

#include <cstring>
#include <limits>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include "../driver/lvmdm_ioctl.h"
#endif

namespace winlvm {

namespace {

bool add_overflows(std::size_t a, std::size_t b) {
  return b > std::numeric_limits<std::size_t>::max() - a;
}

#ifdef _WIN32
std::string windows_error_message(const std::string& operation, DWORD error) {
  return operation + " failed, GetLastError=" + std::to_string(error);
}
#endif

}  // namespace

Result<std::vector<std::uint64_t>> parse_driver_mapping_list(
    std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
  if (bytes.size() < sizeof(LVMDM_LIST_MAPPINGS_OUTPUT)) {
    return Result<std::vector<std::uint64_t>>::failure("driver list response is too short");
  }

  LVMDM_LIST_MAPPINGS_OUTPUT header{};
  std::memcpy(&header, bytes.data(), sizeof(header));

  const std::size_t ids_size =
      static_cast<std::size_t>(header.count) * sizeof(std::uint64_t);
  if (add_overflows(sizeof(LVMDM_LIST_MAPPINGS_OUTPUT), ids_size) ||
      bytes.size() < sizeof(LVMDM_LIST_MAPPINGS_OUTPUT) + ids_size) {
    return Result<std::vector<std::uint64_t>>::failure("driver list response is truncated");
  }

  std::vector<std::uint64_t> ids;
  ids.reserve(header.count);
  const std::uint8_t* cursor = bytes.data() + sizeof(LVMDM_LIST_MAPPINGS_OUTPUT);
  for (std::uint32_t i = 0; i < header.count; ++i) {
    std::uint64_t id = 0;
    std::memcpy(&id, cursor + (static_cast<std::size_t>(i) * sizeof(id)), sizeof(id));
    ids.push_back(id);
  }
  return Result<std::vector<std::uint64_t>>::success(std::move(ids));
#else
  (void)bytes;
  return Result<std::vector<std::uint64_t>>::failure("lvmdm driver client is only available on Windows");
#endif
}

Result<DriverMappingStatus> parse_driver_mapping_query(
    std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
  if (bytes.size() < sizeof(LVMDM_QUERY_MAPPING_OUTPUT)) {
    return Result<DriverMappingStatus>::failure("driver query response is too short");
  }

  LVMDM_QUERY_MAPPING_OUTPUT output{};
  std::memcpy(&output, bytes.data(), sizeof(output));

  DriverMappingStatus status{};
  status.mapping_id = output.mapping_id;
  status.status = output.status;
  status.io_count = output.io_count;
  status.mapping_table_size = output.mapping_table_size;
  return Result<DriverMappingStatus>::success(status);
#else
  (void)bytes;
  return Result<DriverMappingStatus>::failure("lvmdm driver client is only available on Windows");
#endif
}

Result<DriverTranslatedRange> parse_driver_translate_range(
    std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
  if (bytes.size() < sizeof(LVMDM_TRANSLATE_RANGE_OUTPUT)) {
    return Result<DriverTranslatedRange>::failure("driver translate response is too short");
  }

  LVMDM_TRANSLATE_RANGE_OUTPUT output{};
  std::memcpy(&output, bytes.data(), sizeof(output));
  if (output.source_path[LVMDM_MAX_SOURCE_PATH_LENGTH] != '\0') {
    return Result<DriverTranslatedRange>::failure("driver translate response source path is unterminated");
  }

  DriverTranslatedRange range{};
  range.mapping_id = output.mapping_id;
  range.source_path = output.source_path;
  range.source_offset_bytes = output.source_offset_bytes;
  range.pv_offset_bytes = output.pv_offset_bytes;
  range.length_bytes = output.length_bytes;
  return Result<DriverTranslatedRange>::success(std::move(range));
#else
  (void)bytes;
  return Result<DriverTranslatedRange>::failure("lvmdm driver client is only available on Windows");
#endif
}

DriverClient::DriverClient(void* handle) : handle_(handle) {}

DriverClient::~DriverClient() {
  close();
}

DriverClient::DriverClient(DriverClient&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

DriverClient& DriverClient::operator=(DriverClient&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

Result<DriverClient> DriverClient::open_default() {
#ifdef _WIN32
  HANDLE handle = CreateFileW(
      L"\\\\.\\lvmdm",
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Result<DriverClient>::failure(
        windows_error_message("open \\\\.\\lvmdm", GetLastError()));
  }
  return Result<DriverClient>::success(DriverClient(handle));
#else
  return Result<DriverClient>::failure("lvmdm driver client is only available on Windows");
#endif
}

Result<std::uint64_t> DriverClient::create_mapping(
    std::span<const std::uint8_t> mapping_table) {
#ifdef _WIN32
  std::uint64_t mapping_id = 0;
  DWORD bytes_returned = 0;
  const BOOL ok = DeviceIoControl(
      static_cast<HANDLE>(handle_),
      IOCTL_LVMDM_CREATE_MAPPING,
      const_cast<std::uint8_t*>(mapping_table.data()),
      static_cast<DWORD>(mapping_table.size()),
      &mapping_id,
      sizeof(mapping_id),
      &bytes_returned,
      nullptr);
  if (!ok) {
    return Result<std::uint64_t>::failure(
        windows_error_message("IOCTL_LVMDM_CREATE_MAPPING", GetLastError()));
  }
  if (bytes_returned < sizeof(mapping_id)) {
    return Result<std::uint64_t>::failure("driver create response is too short");
  }
  return Result<std::uint64_t>::success(mapping_id);
#else
  (void)mapping_table;
  return Result<std::uint64_t>::failure("lvmdm driver client is only available on Windows");
#endif
}

Status DriverClient::delete_mapping(std::uint64_t mapping_id) {
#ifdef _WIN32
  LVMDM_DELETE_MAPPING_INPUT input{};
  input.mapping_id = mapping_id;
  DWORD bytes_returned = 0;
  const BOOL ok = DeviceIoControl(
      static_cast<HANDLE>(handle_),
      IOCTL_LVMDM_DELETE_MAPPING,
      &input,
      sizeof(input),
      nullptr,
      0,
      &bytes_returned,
      nullptr);
  if (!ok) {
    return Status::failure(
        windows_error_message("IOCTL_LVMDM_DELETE_MAPPING", GetLastError()));
  }
  return Status::success();
#else
  (void)mapping_id;
  return Status::failure("lvmdm driver client is only available on Windows");
#endif
}

Result<std::vector<std::uint64_t>> DriverClient::list_mappings() {
#ifdef _WIN32
  std::vector<std::uint8_t> buffer(sizeof(LVMDM_LIST_MAPPINGS_OUTPUT) + 16 * sizeof(std::uint64_t), 0);

  for (;;) {
    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(
        static_cast<HANDLE>(handle_),
        IOCTL_LVMDM_LIST_MAPPINGS,
        nullptr,
        0,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &bytes_returned,
        nullptr);
    if (ok) {
      return parse_driver_mapping_list(
          std::span<const std::uint8_t>(buffer.data(), bytes_returned));
    }

    const DWORD error = GetLastError();
    if (error != ERROR_INSUFFICIENT_BUFFER && error != ERROR_MORE_DATA &&
        error != ERROR_BUFFER_OVERFLOW) {
      return Result<std::vector<std::uint64_t>>::failure(
          windows_error_message("IOCTL_LVMDM_LIST_MAPPINGS", error));
    }

    auto parsed = parse_driver_mapping_list(
        std::span<const std::uint8_t>(buffer.data(), bytes_returned));
    if (parsed) {
      return parsed;
    }
    if (buffer.size() > (1U << 20)) {
      return Result<std::vector<std::uint64_t>>::failure("driver mapping list is unexpectedly large");
    }
    buffer.resize(buffer.size() * 2, 0);
  }
#else
  return Result<std::vector<std::uint64_t>>::failure("lvmdm driver client is only available on Windows");
#endif
}

Result<DriverMappingStatus> DriverClient::query_mapping(std::uint64_t mapping_id) {
#ifdef _WIN32
  LVMDM_QUERY_MAPPING_INPUT input{};
  input.mapping_id = mapping_id;
  LVMDM_QUERY_MAPPING_OUTPUT output{};
  DWORD bytes_returned = 0;
  const BOOL ok = DeviceIoControl(
      static_cast<HANDLE>(handle_),
      IOCTL_LVMDM_QUERY_MAPPING,
      &input,
      sizeof(input),
      &output,
      sizeof(output),
      &bytes_returned,
      nullptr);
  if (!ok) {
    return Result<DriverMappingStatus>::failure(
        windows_error_message("IOCTL_LVMDM_QUERY_MAPPING", GetLastError()));
  }

  const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(&output);
  return parse_driver_mapping_query(std::span<const std::uint8_t>(bytes, bytes_returned));
#else
  (void)mapping_id;
  return Result<DriverMappingStatus>::failure("lvmdm driver client is only available on Windows");
#endif
}

Result<DriverTranslatedRange> DriverClient::translate_range(
    std::uint64_t mapping_id,
    std::uint64_t lv_offset_bytes,
    std::uint64_t length_bytes) {
#ifdef _WIN32
  LVMDM_TRANSLATE_RANGE_INPUT input{};
  input.mapping_id = mapping_id;
  input.lv_offset_bytes = lv_offset_bytes;
  input.length_bytes = length_bytes;
  LVMDM_TRANSLATE_RANGE_OUTPUT output{};
  DWORD bytes_returned = 0;
  const BOOL ok = DeviceIoControl(
      static_cast<HANDLE>(handle_),
      IOCTL_LVMDM_TRANSLATE_RANGE,
      &input,
      sizeof(input),
      &output,
      sizeof(output),
      &bytes_returned,
      nullptr);
  if (!ok) {
    return Result<DriverTranslatedRange>::failure(
        windows_error_message("IOCTL_LVMDM_TRANSLATE_RANGE", GetLastError()));
  }

  const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(&output);
  return parse_driver_translate_range(std::span<const std::uint8_t>(bytes, bytes_returned));
#else
  (void)mapping_id;
  (void)lv_offset_bytes;
  (void)length_bytes;
  return Result<DriverTranslatedRange>::failure("lvmdm driver client is only available on Windows");
#endif
}

void DriverClient::close() {
#ifdef _WIN32
  if (handle_) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  handle_ = nullptr;
#endif
}

}  // namespace winlvm
