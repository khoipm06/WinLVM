#include "ipc_client.h"

#include "backend.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <winsvc.h>

#include <vector>

namespace winlvm::service {
namespace {

constexpr wchar_t kPipeName[] = LR"(\\.\pipe\winlvm-svc)";
constexpr wchar_t kServiceName[] = L"winlvm-svc";

bool write_all(HANDLE h, const void* buf, DWORD len) {
  const auto* p = static_cast<const std::uint8_t*>(buf);
  DWORD done = 0;
  while (done < len) {
    DWORD chunk = 0;
    if (!WriteFile(h, p + done, len - done, &chunk, nullptr) || chunk == 0) return false;
    done += chunk;
  }
  return true;
}

bool read_all(HANDLE h, void* buf, DWORD len) {
  auto* p = static_cast<std::uint8_t*>(buf);
  DWORD done = 0;
  while (done < len) {
    DWORD chunk = 0;
    if (!ReadFile(h, p + done, len - done, &chunk, nullptr) || chunk == 0) return false;
    done += chunk;
  }
  return true;
}

}  // namespace

bool is_current_process_elevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }
  TOKEN_ELEVATION elev{};
  DWORD len = 0;
  bool elevated = false;
  if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &len)) {
    elevated = (elev.TokenIsElevated != 0);
  }
  CloseHandle(token);
  if (!elevated) return false;

  SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
  PSID admin_sid = nullptr;
  if (!AllocateAndInitializeSid(&nt_auth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admin_sid)) {
    return false;
  }
  BOOL is_member = FALSE;
  CheckTokenMembership(nullptr, admin_sid, &is_member);
  FreeSid(admin_sid);
  return is_member != FALSE;
}

bool try_start_winlvm_service(std::string& error_out) {
  error_out.clear();
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) {
    error_out = "cannot open Service Control Manager (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_START | SERVICE_QUERY_STATUS);
  if (!svc) {
    const DWORD err = GetLastError();
    CloseServiceHandle(scm);
    error_out = "winlvm-svc service not installed or inaccessible (error " + std::to_string(err) + ")";
    return false;
  }

  SERVICE_STATUS_PROCESS ssp{};
  DWORD bytes_needed = 0;
  if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp),
                           sizeof(ssp), &bytes_needed) &&
      ssp.dwCurrentState == SERVICE_RUNNING) {
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
  }

  if (!StartServiceW(svc, 0, nullptr)) {
    const DWORD err = GetLastError();
    if (err != ERROR_SERVICE_ALREADY_RUNNING) {
      CloseServiceHandle(svc);
      CloseServiceHandle(scm);
      error_out = "failed to start winlvm-svc (error " + std::to_string(err) + ")";
      return false;
    }
  }

  for (int i = 0; i < 25; ++i) {
    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp),
                             sizeof(ssp), &bytes_needed)) {
      if (ssp.dwCurrentState == SERVICE_RUNNING) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return true;
      }
      if (ssp.dwCurrentState == SERVICE_STOPPED) break;
    }
    Sleep(200);
  }

  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  error_out = "winlvm-svc did not enter running state";
  return false;
}

bool relaunch_current_process_elevated(const std::wstring& extra_args, std::string& error_out) {
  error_out.clear();
  wchar_t exe_path[MAX_PATH * 2]{};
  const DWORD len = GetModuleFileNameW(nullptr, exe_path, static_cast<DWORD>(std::size(exe_path)));
  if (len == 0 || len >= std::size(exe_path)) {
    error_out = "failed to resolve current executable path";
    return false;
  }
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOASYNC;
  sei.lpVerb = L"runas";
  sei.lpFile = exe_path;
  sei.lpParameters = extra_args.empty() ? nullptr : extra_args.c_str();
  sei.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&sei)) {
    const DWORD err = GetLastError();
    error_out = (err == ERROR_CANCELLED)
                    ? "elevation cancelled by user"
                    : ("elevation failed (error " + std::to_string(err) + ")");
    return false;
  }
  return true;
}

bool ServiceClient::send_pipe_roundtrip(Operation op,
                                        std::string_view req_payload,
                                        ErrorCode& resp_error,
                                        std::string& resp_payload,
                                        std::string& transport_error) {
  transport_error.clear();
  resp_payload.clear();
  resp_error = ErrorCode::DriverUnavailable;

  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 2; ++attempt) {
    // Request explicit pipe client rights without GENERIC_WRITE (which includes
    // FILE_CREATE_PIPE_INSTANCE = 0x0004 and is denied to non-admin IU tokens).
    pipe = CreateFileW(kPipeName,
                       FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
                       0, nullptr, OPEN_EXISTING,
                       SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) break;
    const DWORD err = GetLastError();
    if (err == ERROR_PIPE_BUSY) {
      if (!WaitNamedPipeW(kPipeName, 1500)) {
        transport_error = "winlvm-svc pipe busy timeout";
        return false;
      }
      continue;
    }
    transport_error = "cannot connect to winlvm-svc pipe (error " + std::to_string(err) + ")";
    return false;
  }
  if (pipe == INVALID_HANDLE_VALUE) {
    transport_error = "cannot open winlvm-svc pipe";
    return false;
  }

  const std::uint64_t req_id = next_request_id_.fetch_add(1);
  const auto req_frame = encode_request_frame(op, req_id, req_payload);
  if (req_frame.empty()) {
    CloseHandle(pipe);
    transport_error = "request payload exceeds protocol frame limit";
    return false;
  }

  const std::uint32_t req_len = static_cast<std::uint32_t>(req_frame.size());
  if (!write_all(pipe, &req_len, sizeof(req_len)) ||
      !write_all(pipe, req_frame.data(), req_len)) {
    CloseHandle(pipe);
    transport_error = "failed to write request to winlvm-svc pipe";
    return false;
  }

  std::uint32_t resp_len = 0;
  if (!read_all(pipe, &resp_len, sizeof(resp_len)) ||
      resp_len < kResponseHeaderSize ||
      resp_len > kMaxResponseFrameBytes) {
    CloseHandle(pipe);
    transport_error = "invalid response frame length from winlvm-svc";
    return false;
  }

  std::vector<std::byte> resp_frame(resp_len);
  if (!read_all(pipe, resp_frame.data(), resp_len)) {
    CloseHandle(pipe);
    transport_error = "truncated response frame from winlvm-svc";
    return false;
  }
  CloseHandle(pipe);

  const auto parsed = parse_response_frame(resp_frame);
  if (!parsed.ok) {
    transport_error = "malformed response frame from winlvm-svc";
    return false;
  }
  resp_error = parsed.error;
  resp_payload = parsed.payload;
  return true;
}

bool ServiceClient::get_topology(Topology& out, std::string& error_out) {
  error_out.clear();
  ErrorCode resp_err = ErrorCode::Ok;
  std::string resp_payload;
  std::string transport_err;

  if (send_pipe_roundtrip(Operation::GetTopology, {}, resp_err, resp_payload, transport_err)) {
    mode_ = ConnectionMode::PipeService;
    if (resp_err != ErrorCode::Ok) {
      ActionResponse ar;
      std::string dec_err;
      if (decode_action_response(resp_payload, ar, dec_err) && !ar.message.empty()) {
        error_out = ar.message;
      } else {
        error_out = std::string(error_name(resp_err));
      }
      return false;
    }
    if (!decode_topology(resp_payload, out, error_out)) return false;
    out.service_connected = true;
    return true;
  }

  // In-band fallback: if elevated, attempt once to start winlvm-svc via SCM,
  // then fall back directly to \\.\WinLvmControl + lvmfmt/lvm.exe.
  if (is_current_process_elevated()) {
    if (!attempted_scm_start_) {
      attempted_scm_start_ = true;
      std::string scm_err;
      if (try_start_winlvm_service(scm_err) &&
          send_pipe_roundtrip(Operation::GetTopology, {}, resp_err, resp_payload, transport_err) &&
          resp_err == ErrorCode::Ok && decode_topology(resp_payload, out, error_out)) {
        mode_ = ConnectionMode::PipeService;
        out.service_connected = true;
        return true;
      }
    }
    mode_ = ConnectionMode::DirectFallback;
    out = query_live_topology(false, true);
    return true;
  }

  mode_ = ConnectionMode::Disconnected;
  out = Topology{};
  out.driver_available = false;
  out.service_connected = false;
  out.caller_elevated = false;
  error_out = "winlvm-svc is not running; elevate to start the service or manage volumes directly";
  return false;
}

ActionResponse ServiceClient::call_action(Operation op, std::string_view payload) {
  ErrorCode resp_err = ErrorCode::Ok;
  std::string resp_payload;
  std::string transport_err;

  if (send_pipe_roundtrip(op, payload, resp_err, resp_payload, transport_err)) {
    mode_ = ConnectionMode::PipeService;
    ActionResponse ar;
    std::string dec_err;
    if (decode_action_response(resp_payload, ar, dec_err)) {
      return ar;
    }
    return {resp_err, std::string(error_name(resp_err)), {}};
  }

  if (is_current_process_elevated()) {
    mode_ = ConnectionMode::DirectFallback;
    Request req{op, next_request_id_.fetch_add(1), static_cast<std::uint32_t>(payload.size())};
    ErrorCode out_err = ErrorCode::Ok;
    std::string out_payload;
    dispatch_operation(req, payload, true, false, out_err, out_payload);
    ActionResponse ar;
    std::string dec_err;
    if (decode_action_response(out_payload, ar, dec_err)) return ar;
    return {out_err, std::string(error_name(out_err)), {}};
  }

  mode_ = ConnectionMode::Disconnected;
  return {ErrorCode::AccessDenied,
          "winlvm-svc is not running; administrator elevation is required",
          {}};
}

ActionResponse ServiceClient::rescan() { return call_action(Operation::Rescan, {}); }
ActionResponse ServiceClient::activate_lv(const LvTargetRequest& req) {
  return call_action(Operation::ActivateLv, encode_lv_target(req));
}
ActionResponse ServiceClient::deactivate_lv(const LvTargetRequest& req) {
  return call_action(Operation::DeactivateLv, encode_lv_target(req));
}
ActionResponse ServiceClient::set_drive_letter(const SetDriveLetterRequest& req) {
  return call_action(Operation::SetDriveLetter, encode_set_drive_letter(req));
}
ActionResponse ServiceClient::set_lv_read_only(const SetLvReadOnlyRequest& req) {
  return call_action(Operation::SetLvReadOnly, encode_set_lv_read_only(req));
}
ActionResponse ServiceClient::create_pv(const CreatePvRequest& req) {
  return call_action(Operation::CreatePv, encode_create_pv(req));
}
ActionResponse ServiceClient::create_vg(const CreateVgRequest& req) {
  return call_action(Operation::CreateVg, encode_create_vg(req));
}
ActionResponse ServiceClient::create_lv(const CreateLvRequest& req) {
  return call_action(Operation::CreateLv, encode_create_lv(req));
}
ActionResponse ServiceClient::resize_lv(const ResizeLvRequest& req) {
  return call_action(Operation::ResizeLv, encode_resize_lv(req));
}
ActionResponse ServiceClient::rename_lv(const RenameLvRequest& req) {
  return call_action(Operation::RenameLv, encode_rename_lv(req));
}
ActionResponse ServiceClient::remove_lv(const LvTargetRequest& req) {
  return call_action(Operation::RemoveLv, encode_lv_target(req));
}

}  // namespace winlvm::service

#else

namespace winlvm::service {

bool is_current_process_elevated() { return false; }
bool try_start_winlvm_service(std::string& error_out) {
  error_out = "Windows service control is unavailable on this platform";
  return false;
}
bool relaunch_current_process_elevated(const std::wstring&, std::string& error_out) {
  error_out = "UAC elevation is unavailable on this platform";
  return false;
}

bool ServiceClient::send_pipe_roundtrip(Operation, std::string_view, ErrorCode& resp_error,
                                        std::string&, std::string& transport_error) {
  resp_error = ErrorCode::DriverUnavailable;
  transport_error = "Windows named pipes are unavailable on this platform";
  return false;
}

bool ServiceClient::get_topology(Topology& out, std::string& error_out) {
  out = Topology{};
  error_out = "Windows service client is unavailable on this platform";
  return false;
}

ActionResponse ServiceClient::call_action(Operation, std::string_view) {
  return {ErrorCode::DriverUnavailable, "Windows service client is unavailable on this platform", {}};
}

ActionResponse ServiceClient::rescan() { return call_action(Operation::Rescan, {}); }
ActionResponse ServiceClient::activate_lv(const LvTargetRequest& req) {
  return call_action(Operation::ActivateLv, encode_lv_target(req));
}
ActionResponse ServiceClient::deactivate_lv(const LvTargetRequest& req) {
  return call_action(Operation::DeactivateLv, encode_lv_target(req));
}
ActionResponse ServiceClient::set_drive_letter(const SetDriveLetterRequest& req) {
  return call_action(Operation::SetDriveLetter, encode_set_drive_letter(req));
}
ActionResponse ServiceClient::set_lv_read_only(const SetLvReadOnlyRequest& req) {
  return call_action(Operation::SetLvReadOnly, encode_set_lv_read_only(req));
}
ActionResponse ServiceClient::create_pv(const CreatePvRequest& req) {
  return call_action(Operation::CreatePv, encode_create_pv(req));
}
ActionResponse ServiceClient::create_vg(const CreateVgRequest& req) {
  return call_action(Operation::CreateVg, encode_create_vg(req));
}
ActionResponse ServiceClient::create_lv(const CreateLvRequest& req) {
  return call_action(Operation::CreateLv, encode_create_lv(req));
}
ActionResponse ServiceClient::resize_lv(const ResizeLvRequest& req) {
  return call_action(Operation::ResizeLv, encode_resize_lv(req));
}
ActionResponse ServiceClient::rename_lv(const RenameLvRequest& req) {
  return call_action(Operation::RenameLv, encode_rename_lv(req));
}
ActionResponse ServiceClient::remove_lv(const LvTargetRequest& req) {
  return call_action(Operation::RemoveLv, encode_lv_target(req));
}

}  // namespace winlvm::service

#endif
