#ifndef WINLVM_SERVICE_PROTOCOL_H
#define WINLVM_SERVICE_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace winlvm::service {

inline constexpr std::uint32_t kProtocolVersion = 2;
inline constexpr std::size_t kRequestHeaderSize = 20;
inline constexpr std::size_t kResponseHeaderSize = 16;
inline constexpr std::size_t kMaxFramePayload = 1024U * 1024U;
inline constexpr std::size_t kMaxRequestFrameBytes = kRequestHeaderSize + kMaxFramePayload;
inline constexpr std::size_t kMaxResponseFrameBytes = kResponseHeaderSize + kMaxFramePayload;

enum class Operation : std::uint32_t {
  Inventory = 1,
  Refresh = 4,
  GetTopology = 5,
  Rescan = 6,
  ActivateLv = 7,
  DeactivateLv = 8,
  SetDriveLetter = 9,
  SetLvReadOnly = 10,
  CreatePv = 11,
  CreateVg = 12,
  CreateLv = 13,
  ResizeLv = 14,
  RenameLv = 15,
  RemoveLv = 16,
};

enum class ErrorCode : std::uint32_t {
  Ok = 0,
  MalformedFrame = 1,
  Unsupported = 2,
  DriverUnavailable = 3,
  InvalidArgument = 4,
  AccessDenied = 5,
  NotFound = 6,
  Busy = 7,
  OperationFailed = 8,
};

struct Request {
  Operation operation = Operation::GetTopology;
  std::uint64_t request_id = 0;
  std::uint32_t length = 0;
};

struct ParseResult {
  bool ok = false;
  Request request{};
  std::string payload;
  ErrorCode error = ErrorCode::MalformedFrame;
};

struct ResponseParseResult {
  bool ok = false;
  ErrorCode error = ErrorCode::MalformedFrame;
  std::uint32_t flags = 0;
  std::string payload;
};

struct DiskInfo {
  std::string device_path;
  std::string nt_target;
  std::string kind;  // "disk", "partition", "winluks"
  std::uint64_t size_bytes = 0;
  std::uint32_t logical_sector = 512;
  std::uint32_t physical_sector = 512;
  bool is_pv = false;
  bool is_system_disk = false;
  std::string pv_uuid;
  std::string vg_name;
};

struct PvInfo {
  std::string device_path;
  std::string pv_uuid;
  std::string vg_name;
  std::string pv_key;
  std::uint64_t size_bytes = 0;
  std::uint64_t free_bytes = 0;
  std::uint32_t mda_count = 0;
  bool allocatable = true;
  bool missing = false;
};

struct VgInfo {
  std::string name;
  std::string vg_uuid;
  std::uint64_t extent_size_bytes = 0;
  std::uint64_t size_bytes = 0;
  std::uint64_t free_bytes = 0;
  std::uint32_t pv_count = 0;
  std::uint32_t lv_count = 0;
  std::uint64_t seqno = 0;
  bool read_only = false;
  bool exported = false;
  std::string system_id;
  std::vector<std::string> tags;
};

struct LvInfo {
  std::string vg_name;
  std::string lv_name;
  std::string lv_uuid;
  std::string segment_type;  // "linear", "striped", "thin-pool", "thin", "snapshot", "raid1", "mirror"
  std::uint64_t size_bytes = 0;
  std::uint32_t stripe_count = 1;
  bool active = false;
  bool read_only = false;
  bool internal = false;
  bool supported = true;
  std::string backing_info;
  std::string filesystem;
  std::string volume_label;
  std::string drive_letter;      // e.g. "E:\\" or ""
  std::string volume_guid_path;  // e.g. "\\\\?\\Volume{...}\\" or ""
  std::vector<std::string> tags;
};

struct Topology {
  bool driver_available = false;
  bool service_connected = false;
  bool caller_elevated = false;
  std::uint32_t driver_vg_records = 0;
  std::vector<DiskInfo> disks;
  std::vector<PvInfo> pvs;
  std::vector<VgInfo> vgs;
  std::vector<LvInfo> lvs;
};

struct ActionResponse {
  ErrorCode error = ErrorCode::Ok;
  std::string message;
  std::string created_uuid;
};

struct LvTargetRequest {
  std::string vg_name;
  std::string lv_name;
  std::string lv_uuid;
};

struct SetDriveLetterRequest {
  std::string vg_name;
  std::string lv_name;
  std::string lv_uuid;
  std::string drive_letter;  // "" to remove, or single letter "D".."Z"
};

struct SetLvReadOnlyRequest {
  std::string vg_name;
  std::string lv_name;
  std::string lv_uuid;
  bool read_only = false;
};

struct CreatePvRequest {
  std::string device_path;
  bool force = false;
};

struct CreateVgRequest {
  std::string vg_name;
  std::vector<std::string> pv_paths;
  std::uint64_t extent_size_bytes = 0;  // 0 = default 4 MiB
  bool force = false;
};

struct CreateLvRequest {
  std::string vg_name;
  std::string lv_name;
  std::string segment_type = "linear";  // "linear", "striped", "thin-pool", "thin", "snapshot"
  std::uint64_t size_bytes = 0;
  std::uint64_t virtual_size_bytes = 0;
  std::uint32_t stripes = 1;
  std::uint64_t stripe_size_bytes = 0;
  std::string pool_name;
  std::string origin_name;
  bool read_only = false;
  bool activate = true;
};

struct ResizeLvRequest {
  std::string vg_name;
  std::string lv_name;
  std::string lv_uuid;
  std::uint64_t new_size_bytes = 0;
};

struct RenameLvRequest {
  std::string vg_name;
  std::string old_lv_name;
  std::string new_lv_name;
};

// Framing codecs
[[nodiscard]] std::vector<std::byte> encode_request_frame(Operation operation,
                                                          std::uint64_t request_id,
                                                          std::string_view json_payload = {});
[[nodiscard]] ParseResult parse_frame(std::span<const std::byte> frame);

[[nodiscard]] std::vector<std::byte> encode_response_frame(ErrorCode error,
                                                           std::string_view json_payload = {},
                                                           std::uint32_t flags = 0);
[[nodiscard]] ResponseParseResult parse_response_frame(std::span<const std::byte> frame);

// Policy & validation helpers
[[nodiscard]] bool is_valid_operation(std::uint32_t raw_op) noexcept;
[[nodiscard]] bool is_valid_error_code(std::uint32_t raw_error) noexcept;
[[nodiscard]] bool operation_requires_elevation(Operation operation) noexcept;
[[nodiscard]] bool operation_requires_driver(Operation operation) noexcept;

[[nodiscard]] bool is_valid_lvm_name(std::string_view name);
[[nodiscard]] bool is_valid_uuid_chars(std::string_view uuid);
[[nodiscard]] bool is_valid_device_path(std::string_view path);
[[nodiscard]] bool is_valid_create_segment_type(std::string_view segment_type);
[[nodiscard]] bool normalize_drive_letter(std::string_view input, char& out_letter);

[[nodiscard]] ErrorCode validate_request(const Request& request,
                                         std::string_view payload,
                                         bool driver_available,
                                         bool caller_elevated,
                                         std::string& error_out);

[[nodiscard]] std::string_view operation_name(Operation operation) noexcept;
[[nodiscard]] std::string_view error_name(ErrorCode error) noexcept;

// Payload codecs
[[nodiscard]] std::string encode_topology(const Topology& topology);
[[nodiscard]] bool decode_topology(std::string_view json, Topology& out, std::string& error);

[[nodiscard]] std::string encode_action_response(const ActionResponse& response);
[[nodiscard]] bool decode_action_response(std::string_view json,
                                          ActionResponse& out,
                                          std::string& error);

[[nodiscard]] std::string encode_lv_target(const LvTargetRequest& req);
[[nodiscard]] bool decode_lv_target(std::string_view json,
                                    LvTargetRequest& out,
                                    std::string& error);

[[nodiscard]] std::string encode_set_drive_letter(const SetDriveLetterRequest& req);
[[nodiscard]] bool decode_set_drive_letter(std::string_view json,
                                           SetDriveLetterRequest& out,
                                           std::string& error);

[[nodiscard]] std::string encode_set_lv_read_only(const SetLvReadOnlyRequest& req);
[[nodiscard]] bool decode_set_lv_read_only(std::string_view json,
                                           SetLvReadOnlyRequest& out,
                                           std::string& error);

[[nodiscard]] std::string encode_create_pv(const CreatePvRequest& req);
[[nodiscard]] bool decode_create_pv(std::string_view json,
                                    CreatePvRequest& out,
                                    std::string& error);

[[nodiscard]] std::string encode_create_vg(const CreateVgRequest& req);
[[nodiscard]] bool decode_create_vg(std::string_view json,
                                    CreateVgRequest& out,
                                    std::string& error);

[[nodiscard]] std::string encode_create_lv(const CreateLvRequest& req);
[[nodiscard]] bool decode_create_lv(std::string_view json,
                                    CreateLvRequest& out,
                                    std::string& error);

[[nodiscard]] std::string encode_resize_lv(const ResizeLvRequest& req);
[[nodiscard]] bool decode_resize_lv(std::string_view json,
                                    ResizeLvRequest& out,
                                    std::string& error);

[[nodiscard]] std::string encode_rename_lv(const RenameLvRequest& req);
[[nodiscard]] bool decode_rename_lv(std::string_view json,
                                    RenameLvRequest& out,
                                    std::string& error);

}  // namespace winlvm::service
#endif
