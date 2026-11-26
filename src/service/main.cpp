#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <winsvc.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#endif
#endif

#include "backend.h"
#include "protocol.h"

#ifdef _WIN32
namespace {

// Pipe ACL security model:
// - SY (LocalSystem): full control
// - BA (Builtin Administrators): full control (read + mutating operations)
// - IU (Local Interactive Users): explicit pipe client read/write
//   (0x0012019B = FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_EA |
//   FILE_WRITE_ATTRIBUTES), intentionally omitting FILE_CREATE_PIPE_INSTANCE
//   (0x0004) so unprivileged users cannot create rogue pipe instances while
//   still permitting local desktop tray/GUI GetTopology/Inventory/Rescan
//   without UAC. Network, anonymous, guest, and service tokens are blocked.
// Every mutating operation is additionally gated in-band by checking the
// impersonated pipe client's token for BA or SY membership.
constexpr wchar_t kPipeName[] = LR"(\\.\pipe\winlvm-svc)";
constexpr wchar_t kPipeSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x12019B;;;IU)";
constexpr DWORD kPipeBuffer = 64U * 1024U;

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
HANDLE g_stop_event = nullptr;
std::atomic<bool> g_stop_requested{false};

bool read_exact_overlapped(HANDLE pipe, void* buffer, DWORD size, HANDLE io_event) {
  auto* dst = static_cast<std::uint8_t*>(buffer);
  DWORD total = 0;
  while (total < size) {
    ResetEvent(io_event);
    OVERLAPPED ov{};
    ov.hEvent = io_event;
    DWORD chunk = 0;
    const BOOL ok = ReadFile(pipe, dst + total, size - total, &chunk, &ov);
    if (!ok) {
      if (GetLastError() != ERROR_IO_PENDING) return false;
      HANDLE waits[2] = {g_stop_event, io_event};
      const DWORD wait_rc = WaitForMultipleObjects(2, waits, FALSE, 15000);
      if (wait_rc != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &ov);
        DWORD cancelled_bytes = 0;
        GetOverlappedResult(pipe, &ov, &cancelled_bytes, TRUE);
        return false;
      }
      if (!GetOverlappedResult(pipe, &ov, &chunk, FALSE)) return false;
    }
    if (chunk == 0) return false;
    total += chunk;
  }
  return true;
}

bool write_exact_overlapped(HANDLE pipe, const void* buffer, DWORD size, HANDLE io_event) {
  const auto* src = static_cast<const std::uint8_t*>(buffer);
  DWORD total = 0;
  while (total < size) {
    ResetEvent(io_event);
    OVERLAPPED ov{};
    ov.hEvent = io_event;
    DWORD chunk = 0;
    const BOOL ok = WriteFile(pipe, src + total, size - total, &chunk, &ov);
    if (!ok) {
      if (GetLastError() != ERROR_IO_PENDING) return false;
      HANDLE waits[2] = {g_stop_event, io_event};
      const DWORD wait_rc = WaitForMultipleObjects(2, waits, FALSE, 15000);
      if (wait_rc != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &ov);
        DWORD cancelled_bytes = 0;
        GetOverlappedResult(pipe, &ov, &cancelled_bytes, TRUE);
        return false;
      }
      if (!GetOverlappedResult(pipe, &ov, &chunk, FALSE)) return false;
    }
    if (chunk == 0) return false;
    total += chunk;
  }
  return true;
}

bool check_pipe_client_elevated(HANDLE pipe) {
  if (!ImpersonateNamedPipeClient(pipe)) {
    return false;
  }

  bool privileged = false;
  SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;

  PSID admin_sid = nullptr;
  if (AllocateAndInitializeSid(&nt_auth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                               DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admin_sid)) {
    BOOL is_admin = FALSE;
    if (CheckTokenMembership(nullptr, admin_sid, &is_admin) && is_admin) {
      privileged = true;
    }
    FreeSid(admin_sid);
  }

  if (!privileged) {
    PSID sys_sid = nullptr;
    if (AllocateAndInitializeSid(&nt_auth, 1, SECURITY_LOCAL_SYSTEM_RID,
                                 0, 0, 0, 0, 0, 0, 0, &sys_sid)) {
      BOOL is_sys = FALSE;
      if (CheckTokenMembership(nullptr, sys_sid, &is_sys) && is_sys) {
        privileged = true;
      }
      FreeSid(sys_sid);
    }
  }

  if (!RevertToSelf()) {
    ExitProcess(1);
  }
  return privileged;
}

HANDLE open_pipe_instance() {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kPipeSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
    return INVALID_HANDLE_VALUE;
  }
  SECURITY_ATTRIBUTES attributes{sizeof attributes, descriptor, FALSE};
  HANDLE pipe = CreateNamedPipeW(
      kPipeName,
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1,
      kPipeBuffer,
      kPipeBuffer,
      0,
      &attributes);
  LocalFree(descriptor);
  return pipe;
}

void serve_client(HANDLE pipe, HANDLE io_event) {
  std::uint32_t frame_size = 0;
  if (!read_exact_overlapped(pipe, &frame_size, sizeof(frame_size), io_event) ||
      frame_size < winlvm::service::kRequestHeaderSize ||
      frame_size > winlvm::service::kMaxRequestFrameBytes) {
    const auto err_frame = winlvm::service::encode_response_frame(
        winlvm::service::ErrorCode::MalformedFrame,
        winlvm::service::encode_action_response(
            {winlvm::service::ErrorCode::MalformedFrame, "Invalid request frame length", {}}));
    const std::uint32_t resp_len = static_cast<std::uint32_t>(err_frame.size());
    write_exact_overlapped(pipe, &resp_len, sizeof(resp_len), io_event);
    write_exact_overlapped(pipe, err_frame.data(), resp_len, io_event);
    FlushFileBuffers(pipe);
    return;
  }

  std::vector<std::byte> frame(frame_size);
  if (!read_exact_overlapped(pipe, frame.data(), frame_size, io_event)) return;

  const auto parsed = winlvm::service::parse_frame(frame);
  winlvm::service::ErrorCode resp_code = winlvm::service::ErrorCode::MalformedFrame;
  std::string resp_payload;

  if (!parsed.ok) {
    resp_code = parsed.error;
    resp_payload = winlvm::service::encode_action_response(
        {parsed.error, "Malformed protocol frame", {}});
  } else {
    const bool client_elevated = check_pipe_client_elevated(pipe);
    winlvm::service::dispatch_operation(parsed.request, parsed.payload, client_elevated, true,
                                        resp_code, resp_payload);
  }

  auto resp_frame = winlvm::service::encode_response_frame(resp_code, resp_payload);
  if (resp_frame.empty()) {
    resp_frame = winlvm::service::encode_response_frame(
        winlvm::service::ErrorCode::OperationFailed,
        winlvm::service::encode_action_response(
            {winlvm::service::ErrorCode::OperationFailed, "Response exceeded protocol limit", {}}));
  }

  const std::uint32_t resp_len = static_cast<std::uint32_t>(resp_frame.size());
  write_exact_overlapped(pipe, &resp_len, sizeof(resp_len), io_event);
  write_exact_overlapped(pipe, resp_frame.data(), resp_len, io_event);
  FlushFileBuffers(pipe);
}

void set_service_state(DWORD state, DWORD controls = 0) {
  if (!g_status_handle) return;
  SERVICE_STATUS status{};
  status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  status.dwCurrentState = state;
  status.dwControlsAccepted = controls;
  SetServiceStatus(g_status_handle, &status);
}

void WINAPI service_ctrl_handler(DWORD control) {
  if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
    g_stop_requested.store(true);
    set_service_state(SERVICE_STOP_PENDING, 0);
    if (g_stop_event) SetEvent(g_stop_event);
  }
}

BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
  if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT ||
      ctrl_type == CTRL_CLOSE_EVENT || ctrl_type == CTRL_SHUTDOWN_EVENT) {
    g_stop_requested.store(true);
    if (g_stop_event) SetEvent(g_stop_event);
    return TRUE;
  }
  return FALSE;
}

int run_broker_loop(bool as_service) {
  g_stop_requested.store(false);
  g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE pipe = open_pipe_instance();

  if (!g_stop_event || !io_event || pipe == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    if (io_event) CloseHandle(io_event);
    if (g_stop_event) {
      CloseHandle(g_stop_event);
      g_stop_event = nullptr;
    }
    if (as_service) set_service_state(SERVICE_STOPPED, 0);
    return static_cast<int>(err ? err : 1);
  }

  if (as_service) {
    set_service_state(SERVICE_RUNNING, SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
  }

  while (!g_stop_requested.load()) {
    ResetEvent(io_event);
    OVERLAPPED ov{};
    ov.hEvent = io_event;

    bool connected = false;
    if (ConnectNamedPipe(pipe, &ov)) {
      connected = true;
    } else {
      const DWORD err = GetLastError();
      if (err == ERROR_PIPE_CONNECTED) {
        connected = true;
      } else if (err == ERROR_IO_PENDING) {
        HANDLE waits[2] = {g_stop_event, io_event};
        const DWORD wait_rc = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wait_rc == WAIT_OBJECT_0) {
          CancelIoEx(pipe, &ov);
          DWORD cancelled_bytes = 0;
          GetOverlappedResult(pipe, &ov, &cancelled_bytes, TRUE);
          break;
        }
        if (wait_rc == WAIT_OBJECT_0 + 1) {
          DWORD transferred = 0;
          if (GetOverlappedResult(pipe, &ov, &transferred, FALSE)) {
            connected = true;
          }
        }
      } else {
        break;
      }
    }

    if (connected) {
      serve_client(pipe, io_event);
      DisconnectNamedPipe(pipe);
    }
  }

  CloseHandle(pipe);
  CloseHandle(io_event);
  CloseHandle(g_stop_event);
  g_stop_event = nullptr;
  if (as_service) set_service_state(SERVICE_STOPPED, 0);
  return 0;
}

void WINAPI service_main(DWORD, LPWSTR*) {
  g_status_handle = RegisterServiceCtrlHandlerW(L"winlvm-svc", service_ctrl_handler);
  if (g_status_handle == nullptr) return;
  run_broker_loop(true);
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--console") == 0) {
      SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
      return run_broker_loop(false);
    }
  }
  SERVICE_TABLE_ENTRYW table[]{{const_cast<LPWSTR>(L"winlvm-svc"), service_main},
                               {nullptr, nullptr}};
  return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
}
#else
int main() { return 0; }
#endif
