#include "testing.h"
#include "service/mutation.h"
#include "service/protocol.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {
std::byte b(std::uint8_t value) { return static_cast<std::byte>(value); }
void put32(std::vector<std::byte>& frame, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) {
    frame.push_back(b(static_cast<std::uint8_t>(value >> (i * 8U))));
  }
}
void put64(std::vector<std::byte>& frame, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) {
    frame.push_back(b(static_cast<std::uint8_t>(value >> (i * 8U))));
  }
}
}  // namespace

TEST(service_rejects_malformed_request_frames) {
  using namespace winlvm::service;

  // Empty & truncated frames
  CHECK(!parse_frame({}).ok);
  std::vector<std::byte> short_frame(kRequestHeaderSize - 1, b(0));
  CHECK(!parse_frame(short_frame).ok);
  CHECK(parse_frame(short_frame).error == ErrorCode::MalformedFrame);

  // Wrong protocol version
  std::vector<std::byte> bad_ver;
  put32(bad_ver, 1U);
  put32(bad_ver, static_cast<std::uint32_t>(Operation::GetTopology));
  put32(bad_ver, 0U);
  put64(bad_ver, 1ULL);
  CHECK(!parse_frame(bad_ver).ok);

  // Unknown operation
  std::vector<std::byte> bad_op;
  put32(bad_op, kProtocolVersion);
  put32(bad_op, 99U);
  put32(bad_op, 0U);
  put64(bad_op, 1ULL);
  CHECK(!parse_frame(bad_op).ok);

  // Oversized payload declaration
  std::vector<std::byte> oversized;
  put32(oversized, kProtocolVersion);
  put32(oversized, static_cast<std::uint32_t>(Operation::GetTopology));
  put32(oversized, 0xffffffffU);
  put64(oversized, 0ULL);
  CHECK(!parse_frame(oversized).ok);

  // Length mismatch (declared payload larger than trailing bytes)
  std::vector<std::byte> mismatch;
  put32(mismatch, kProtocolVersion);
  put32(mismatch, static_cast<std::uint32_t>(Operation::Rescan));
  put32(mismatch, 4U);
  put64(mismatch, 7ULL);
  mismatch.push_back(b('{'));
  mismatch.push_back(b('}'));
  CHECK(!parse_frame(mismatch).ok);

  // Oversized encode rejected
  const std::string huge(kMaxFramePayload + 1U, 'x');
  CHECK(encode_request_frame(Operation::GetTopology, 1, huge).empty());
}

TEST(service_rejects_malformed_response_frames) {
  using namespace winlvm::service;

  CHECK(!parse_response_frame({}).ok);
  std::vector<std::byte> short_resp(kResponseHeaderSize - 1, b(0));
  CHECK(!parse_response_frame(short_resp).ok);

  // Bad error code
  std::vector<std::byte> bad_err;
  put32(bad_err, kProtocolVersion);
  put32(bad_err, 999U);
  put32(bad_err, 0U);
  put32(bad_err, 0U);
  CHECK(!parse_response_frame(bad_err).ok);

  // Length mismatch
  std::vector<std::byte> bad_len;
  put32(bad_len, kProtocolVersion);
  put32(bad_len, static_cast<std::uint32_t>(ErrorCode::Ok));
  put32(bad_len, 10U);
  put32(bad_len, 0U);
  bad_len.push_back(b('{'));
  CHECK(!parse_response_frame(bad_len).ok);

  // Valid round-trip
  const auto resp_frame = encode_response_frame(ErrorCode::Busy, "{\"message\":\"locked\"}", 3U);
  const auto parsed = parse_response_frame(resp_frame);
  CHECK(parsed.ok);
  CHECK(parsed.error == ErrorCode::Busy);
  CHECK_EQ(parsed.flags, 3U);
  CHECK_EQ(parsed.payload, std::string("{\"message\":\"locked\"}"));
}

TEST(service_topology_frame_roundtrip) {
  using namespace winlvm::service;

  Topology src;
  src.driver_available = true;
  src.service_connected = true;
  src.caller_elevated = true;
  src.driver_vg_records = 2;

  src.disks.push_back(DiskInfo{
      "\\\\.\\PhysicalDrive1", "\\Device\\Harddisk1\\DR1", "disk",
      64ULL * 1024 * 1024 * 1024, 512, 4096, true, false,
      "MMmapV-do0d-x2Tu-6PrQ-PRL3-hgD6-BJR3Od", "vgdata"});
  src.disks.push_back(DiskInfo{
      "\\\\.\\WinLuksVolume0", "\\Device\\WinLuksVolume0", "winluks",
      16ULL * 1024 * 1024 * 1024, 4096, 4096, true, false,
      "AbCdEf-1234-5678-90ab-cdef-GHIJ-KLMNOP", "vgcrypt"});

  src.pvs.push_back(PvInfo{
      "\\\\.\\PhysicalDrive1", "MMmapV-do0d-x2Tu-6PrQ-PRL3-hgD6-BJR3Od",
      "vgdata", "pv0", 64ULL * 1024 * 1024 * 1024, 12ULL * 1024 * 1024 * 1024,
      2, true, false});

  src.vgs.push_back(VgInfo{
      "vgdata", "VgUuid-1111-2222-3333-4444-5555-666666",
      4ULL * 1024 * 1024, 64ULL * 1024 * 1024 * 1024, 12ULL * 1024 * 1024 * 1024,
      1, 2, 17, false, false, "", {"prod", "fast-tier"}});

  src.lvs.push_back(LvInfo{
      "vgdata", "lv_media", "LvUuid-aaaa-bbbb-cccc-dddd-eeee-ffffff",
      "striped", 32ULL * 1024 * 1024 * 1024, 2, true, false, false, true,
      "2 stripes (pv0, pv1)", "NTFS", "Media \"HD\" \\ 1", "E:\\",
      "\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\", {"backup"}});
  src.lvs.push_back(LvInfo{
      "vgdata", "lv_snap", "LvUuid-9999-8888-7777-6666-5555-444444",
      "snapshot", 4ULL * 1024 * 1024 * 1024, 1, false, true, false, true,
      "origin: lv_media", "", "", "", "", {}});

  const std::string json = encode_topology(src);
  const auto frame = encode_response_frame(ErrorCode::Ok, json);
  const auto parsed = parse_response_frame(frame);
  CHECK(parsed.ok);
  CHECK(parsed.error == ErrorCode::Ok);

  Topology dst;
  std::string err;
  CHECK_MSG(decode_topology(parsed.payload, dst, err), err);
  CHECK_EQ(dst.driver_available, src.driver_available);
  CHECK_EQ(dst.service_connected, src.service_connected);
  CHECK_EQ(dst.caller_elevated, src.caller_elevated);
  CHECK_EQ(dst.driver_vg_records, src.driver_vg_records);
  CHECK_EQ(dst.disks.size(), 2U);
  CHECK_EQ(dst.disks[1].kind, std::string("winluks"));
  CHECK_EQ(dst.disks[1].logical_sector, 4096U);
  CHECK_EQ(dst.pvs.size(), 1U);
  CHECK_EQ(dst.pvs[0].mda_count, 2U);
  CHECK_EQ(dst.vgs.size(), 1U);
  CHECK_EQ(dst.vgs[0].seqno, 17ULL);
  CHECK_EQ(dst.vgs[0].tags.size(), 2U);
  CHECK_EQ(dst.vgs[0].tags[1], std::string("fast-tier"));
  CHECK_EQ(dst.lvs.size(), 2U);
  CHECK_EQ(dst.lvs[0].segment_type, std::string("striped"));
  CHECK_EQ(dst.lvs[0].volume_label, std::string("Media \"HD\" \\ 1"));
  CHECK_EQ(dst.lvs[0].drive_letter, std::string("E:\\"));
  CHECK_EQ(dst.lvs[0].volume_guid_path,
           std::string("\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\"));
  CHECK(dst.lvs[1].read_only);
  CHECK_EQ(dst.lvs[1].backing_info, std::string("origin: lv_media"));
}

TEST(service_action_payload_codecs_and_validation) {
  using namespace winlvm::service;
  std::string err;

  // Drive letter normalization & validation
  char letter = '?';
  CHECK(normalize_drive_letter("e:", letter) && letter == 'E');
  CHECK(normalize_drive_letter("Z:\\", letter) && letter == 'Z');
  CHECK(normalize_drive_letter("d", letter) && letter == 'D');
  CHECK(normalize_drive_letter("", letter) && letter == '\0');
  CHECK(normalize_drive_letter("None", letter) && letter == '\0');
  CHECK(!normalize_drive_letter("C:", letter));
  CHECK(!normalize_drive_letter("A:\\", letter));
  CHECK(!normalize_drive_letter("EF:", letter));
  CHECK(!normalize_drive_letter("9:", letter));

  // SetDriveLetter round-trip
  const SetDriveLetterRequest dl_req{"vg0", "lv0", "", "g:\\"};
  SetDriveLetterRequest dl_out;
  CHECK_MSG(decode_set_drive_letter(encode_set_drive_letter(dl_req), dl_out, err), err);
  CHECK_EQ(dl_out.drive_letter, std::string("G"));

  // Reject system drive letter C:
  CHECK(!decode_set_drive_letter(
      encode_set_drive_letter({"vg0", "lv0", "", "C:"}), dl_out, err));

  // SetLvReadOnly round-trip & missing field rejection
  const SetLvReadOnlyRequest ro_req{"vg0", "lv0", "", true};
  SetLvReadOnlyRequest ro_out;
  CHECK_MSG(decode_set_lv_read_only(encode_set_lv_read_only(ro_req), ro_out, err), err);
  CHECK(ro_out.read_only);
  CHECK(!decode_set_lv_read_only("{\"vg_name\":\"vg0\",\"lv_name\":\"lv0\"}", ro_out, err));

  // CreatePv validation (blocks shell metacharacters and leading option dashes)
  CreatePvRequest pv_out;
  CHECK_MSG(decode_create_pv(encode_create_pv({"\\\\.\\PhysicalDrive2", true}), pv_out, err), err);
  CHECK(pv_out.force);
  CHECK(!decode_create_pv(encode_create_pv({"\\\\.\\PhysicalDrive2;rm -rf", false}), pv_out, err));
  CHECK(!decode_create_pv(encode_create_pv({"--force", false}), pv_out, err));
  CHECK(!decode_create_pv(encode_create_pv({"-f", false}), pv_out, err));

  // CreateVg validation
  CreateVgRequest vg_out;
  CHECK_MSG(decode_create_vg(
      encode_create_vg({"vg_main", {"\\\\.\\PhysicalDrive1"}, 4ULL * 1024 * 1024, false}),
      vg_out, err), err);
  CHECK(!decode_create_vg(encode_create_vg({"-badvg", {"\\\\.\\PhysicalDrive1"}, 0, false}),
                          vg_out, err));
  CHECK(!decode_create_vg(encode_create_vg({"vg_main", {}, 0, false}), vg_out, err));
  CHECK(!decode_create_vg(encode_create_vg({"vg_main", {"\\\\.\\PhysicalDrive1"}, 12345, false}),
                          vg_out, err));

  // CreateLv: linear, striped, thin-pool, thin, snapshot + invalid cases
  CreateLvRequest lv_out;
  CHECK_MSG(decode_create_lv(
      encode_create_lv({"vg0", "lv_lin", "linear", 64ULL << 20, 0, 1, 0, "", "", false, true}),
      lv_out, err), err);
  CHECK_MSG(decode_create_lv(
      encode_create_lv({"vg0", "lv_str", "striped", 128ULL << 20, 0, 2, 65536, "", "", false, true}),
      lv_out, err), err);
  CHECK_MSG(decode_create_lv(
      encode_create_lv({"vg0", "pool0", "thin-pool", 512ULL << 20, 0, 1, 0, "", "", false, false}),
      lv_out, err), err);
  CHECK_MSG(decode_create_lv(
      encode_create_lv({"vg0", "thin0", "thin", 0, 1024ULL << 20, 1, 0, "pool0", "", false, true}),
      lv_out, err), err);
  CHECK_MSG(decode_create_lv(
      encode_create_lv({"vg0", "snap0", "snapshot", 32ULL << 20, 0, 1, 0, "", "lv_lin", false, true}),
      lv_out, err), err);
  // Reject striped with stripes < 2
  CHECK(!decode_create_lv(
      encode_create_lv({"vg0", "bad", "striped", 64ULL << 20, 0, 1, 0, "", "", false, true}),
      lv_out, err));
  // Reject thin without pool_name
  CHECK(!decode_create_lv(
      encode_create_lv({"vg0", "bad", "thin", 0, 64ULL << 20, 1, 0, "", "", false, true}),
      lv_out, err));
  // Reject snapshot without origin_name
  CHECK(!decode_create_lv(
      encode_create_lv({"vg0", "bad", "snapshot", 64ULL << 20, 0, 1, 0, "", "", false, true}),
      lv_out, err));
  // Reject unsupported segment type
  CHECK(!decode_create_lv(
      encode_create_lv({"vg0", "bad", "raid5", 64ULL << 20, 0, 1, 0, "", "", false, true}),
      lv_out, err));

  // ResizeLv & RenameLv
  ResizeLvRequest rsz_out;
  CHECK_MSG(decode_resize_lv(encode_resize_lv({"vg0", "lv0", "", 256ULL << 20}), rsz_out, err), err);
  CHECK(!decode_resize_lv(encode_resize_lv({"vg0", "lv0", "", 0}), rsz_out, err));

  RenameLvRequest ren_out;
  CHECK_MSG(decode_rename_lv(encode_rename_lv({"vg0", "old_lv", "new_lv"}), ren_out, err), err);
  CHECK(!decode_rename_lv(encode_rename_lv({"vg0", "same", "same"}), ren_out, err));

  // ActionResponse round-trip
  ActionResponse ar_out;
  CHECK_MSG(decode_action_response(
      encode_action_response({ErrorCode::Ok, "Created", "AbCdEf-1234-5678-90ab-cdef-GHIJ-KLMNOP"}),
      ar_out, err), err);
  CHECK(ar_out.error == ErrorCode::Ok);
  CHECK_EQ(ar_out.message, std::string("Created"));
}

TEST(service_policy_and_json_hardening) {
  using namespace winlvm::service;
  std::string err;

  // GetTopology works for unprivileged interactive users even without driver
  const auto topo_frame = encode_request_frame(Operation::GetTopology, 42);
  const auto topo_parsed = parse_frame(topo_frame);
  CHECK(topo_parsed.ok);
  CHECK_EQ(topo_parsed.request.request_id, 42ULL);
  CHECK(validate_request(topo_parsed.request, topo_parsed.payload, false, false, err) ==
        ErrorCode::Ok);

  // Rescan allowed for unprivileged interactive user when driver is present,
  // fails with DriverUnavailable when driver is absent.
  const auto rescan_parsed = parse_frame(encode_request_frame(Operation::Rescan, 43));
  CHECK(rescan_parsed.ok);
  CHECK(validate_request(rescan_parsed.request, rescan_parsed.payload, false, false, err) ==
        ErrorCode::DriverUnavailable);
  CHECK(validate_request(rescan_parsed.request, rescan_parsed.payload, true, false, err) ==
        ErrorCode::Ok);

  // Mutating operations require elevation (AccessDenied for unprivileged caller)
  const std::string act_payload = encode_lv_target({"vg0", "lv0", ""});
  const auto act_parsed = parse_frame(encode_request_frame(Operation::ActivateLv, 44, act_payload));
  CHECK(act_parsed.ok);
  CHECK(validate_request(act_parsed.request, act_parsed.payload, true, false, err) ==
        ErrorCode::AccessDenied);
  CHECK(validate_request(act_parsed.request, act_parsed.payload, false, true, err) ==
        ErrorCode::DriverUnavailable);
  CHECK(validate_request(act_parsed.request, act_parsed.payload, true, true, err) ==
        ErrorCode::Ok);

  // Reject hostile JSON payloads: trailing garbage, embedded NUL, deep nesting, floats
  const Request create_req{Operation::CreatePv, 45, 0};
  CHECK(validate_request(create_req, "{\"device_path\":\"a\"} trailing", true, true, err) ==
        ErrorCode::InvalidArgument);
  CHECK(validate_request(create_req, "{\"device_path\":\"a\\u0000b\"}", true, true, err) ==
        ErrorCode::InvalidArgument);
  std::string deep(40, '{');
  deep.append(40, '}');
  CHECK(validate_request(create_req, deep, true, true, err) == ErrorCode::InvalidArgument);
  CHECK(validate_request(Request{Operation::ResizeLv, 46, 0},
                         "{\"vg_name\":\"vg0\",\"lv_name\":\"lv0\",\"new_size_bytes\":1.5}",
                         true, true, err) == ErrorCode::InvalidArgument);

  CHECK_EQ(error_name(ErrorCode::Ok), std::string_view("ok"));
  CHECK_EQ(error_name(ErrorCode::MalformedFrame), std::string_view("malformed-frame"));
  CHECK_EQ(error_name(ErrorCode::AccessDenied), std::string_view("access-denied"));
  CHECK_EQ(error_name(ErrorCode::DriverUnavailable), std::string_view("driver-unavailable"));
  CHECK_EQ(operation_name(Operation::GetTopology), std::string_view("get-topology"));
  CHECK_EQ(operation_name(Operation::SetDriveLetter), std::string_view("set-drive-letter"));
}

TEST(mutation_lock_geometry_and_executor) {
  using namespace winlvm::service;

  MutationError first_error = MutationError::Unsupported;
  {
    const auto first = MutationLock::acquire("vg/lv", first_error);
    CHECK(first.owns_lock());
    CHECK(first_error == MutationError::Ok);

    MutationError second_error = MutationError::Unsupported;
    const auto second = MutationLock::acquire("vg/lv", second_error);
    CHECK(!second.owns_lock());
    CHECK(second_error == MutationError::Busy);
  }
  // Lock is released once the RAII token leaves scope.
  MutationError third_error = MutationError::Unsupported;
  const auto third = MutationLock::acquire("vg/lv", third_error);
  CHECK(third.owns_lock());

  // Geometry validation
  CHECK(validate_geometry(MutationKind::Shrink, {4096, 2048, 4096, 512}) ==
        MutationError::InvalidGeometry);
  CHECK(validate_geometry(MutationKind::Extend, {4096, 4096, 0, 512}) ==
        MutationError::InvalidGeometry);
  CHECK(validate_geometry(MutationKind::Extend, {4096, 4100, 0, 512}) ==
        MutationError::InvalidGeometry);
  CHECK(validate_geometry(MutationKind::Extend, {4096, 8192, 2048, 512}) ==
        MutationError::Ok);

  // Null executor refused cleanly
  std::string err;
  CHECK(execute_mutation(MutationKind::Extend, "vg/other", {4096, 8192, 0, 512}, {}, err) ==
        MutationError::Unsupported);

  // Busy LV refused while lock is held
  CHECK(execute_mutation(MutationKind::Extend, "vg/lv", {4096, 8192, 0, 512},
                         [](std::string&) { return true; }, err) == MutationError::Busy);

  // Successful execution
  bool ran = false;
  CHECK(execute_mutation(MutationKind::Extend, "vg/free", {4096, 8192, 0, 512},
                         [&](std::string&) {
                           ran = true;
                           return true;
                         },
                         err) == MutationError::Ok);
  CHECK(ran);

  // Failed execution propagates diagnostic
  CHECK(execute_mutation(MutationKind::Extend, "vg/free", {4096, 8192, 0, 512},
                         [](std::string& e) {
                           e = "insufficient extents";
                           return false;
                         },
                         err) == MutationError::Failed);
  CHECK_EQ(err, std::string("insufficient extents"));
  CHECK_EQ(mutation_error_name(MutationError::Failed), std::string_view("failed"));
}
