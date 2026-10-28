#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsvc.h>
#include <sddl.h>
#include <atomic>
#include <vector>
#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#endif
#endif

#include "protocol.h"
#ifdef _WIN32
#include "../winlvm_ioctl.h"
#endif

#ifdef _WIN32
namespace {

constexpr wchar_t kPipeName[] = LR"(\\.\pipe\winlvm-svc)";
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
    BOOL ok = ReadFile(pipe, dst + total, size - total, &chunk, &ov);
    if (!ok) {
      if (GetLastError() != ERROR_IO_PENDING) return false;
      HANDLE waits[2] = {g_stop_event, io_event};
      DWORD wait_rc = WaitForMultipleObjects(2, waits, FALSE, 10000);
      if (wait_rc != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &ov);
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
    BOOL ok = WriteFile(pipe, src + total, size - total, &chunk, &ov);
    if (!ok) {
      if (GetLastError() != ERROR_IO_PENDING) return false;
      HANDLE waits[2] = {g_stop_event, io_event};
      DWORD wait_rc = WaitForMultipleObjects(2, waits, FALSE, 10000);
      if (wait_rc != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &ov);
        return false;
      }
      if (!GetOverlappedResult(pipe, &ov, &chunk, FALSE)) return false;
    }
    if (chunk == 0) return false;
    total += chunk;
  }
  return true;
}

HANDLE open_pipe() {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) {
    return INVALID_HANDLE_VALUE;
  }
  SECURITY_ATTRIBUTES attributes{sizeof attributes, descriptor, FALSE};
  HANDLE pipe = CreateNamedPipeW(
      kPipeName,
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      1,
      kPipeBuffer,
      kPipeBuffer,
      0,
      &attributes);
  LocalFree(descriptor);
  return pipe;
}

winlvm::service::ErrorCode dispatch_driver_operation(winlvm::service::Operation op) {
  HANDLE device = CreateFileW(WINLVM_USER_DEVICE_NAME,
                              GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (device == INVALID_HANDLE_VALUE) {
    return winlvm::service::ErrorCode::DriverUnavailable;
  }

  WINLVM_CONTROL_REQUEST req{};
  req.version = WINLVM_ABI_VERSION;
  req.size = sizeof(req);
  DWORD returned = 0;
  BOOL ok = FALSE;

  if (op == winlvm::service::Operation::Inventory) {
    WINLVM_INVENTORY inventory{};
    ok = DeviceIoControl(device, IOCTL_WINLVM_INVENTORY,
                         &req, sizeof(req), &inventory, sizeof(inventory), &returned, nullptr);
  } else if (op == winlvm::service::Operation::Refresh) {
    ok = DeviceIoControl(device, IOCTL_WINLVM_REFRESH,
                         &req, sizeof(req), nullptr, 0, &returned, nullptr);
  }
  CloseHandle(device);
  return ok ? winlvm::service::ErrorCode::Ok : winlvm::service::ErrorCode::DriverUnavailable;
}

void serve_client(HANDLE pipe, HANDLE io_event) {
  std::uint32_t frame_size = 0;
  if (!read_exact_overlapped(pipe, &frame_size, sizeof(frame_size), io_event) ||
      frame_size > winlvm::service::kMaxFramePayload + 20U) {
    return;
  }

  std::vector<std::byte> frame(frame_size);
  if (!read_exact_overlapped(pipe, frame.data(), frame_size, io_event)) return;

  const auto parsed = winlvm::service::parse_frame(frame);
  winlvm::service::ErrorCode code = parsed.ok
                                        ? winlvm::service::execute_request(parsed.request, true)
                                        : parsed.error;
  if (parsed.ok && code == winlvm::service::ErrorCode::Ok) {
    code = dispatch_driver_operation(parsed.request.operation);
  }

  const std::uint32_t result = static_cast<std::uint32_t>(code);
  write_exact_overlapped(pipe, &result, sizeof(result), io_event);
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

void WINAPI service_main(DWORD, LPWSTR*) {
  g_stop_requested.store(false);
  g_status_handle = RegisterServiceCtrlHandlerW(L"winlvm-svc", service_ctrl_handler);
  if (g_status_handle == nullptr) return;

  g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE pipe = open_pipe();
  if (!g_stop_event || !io_event || pipe == INVALID_HANDLE_VALUE) {
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    if (io_event) CloseHandle(io_event);
    if (g_stop_event) {
      CloseHandle(g_stop_event);
      g_stop_event = nullptr;
    }
    set_service_state(SERVICE_STOPPED, 0);
    return;
  }

  set_service_state(SERVICE_RUNNING, SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);

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
  set_service_state(SERVICE_STOPPED, 0);
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  SERVICE_TABLE_ENTRYW table[]{{const_cast<LPWSTR>(L"winlvm-svc"), service_main}, {nullptr, nullptr}};
  return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
}
#else
int main() { return 0; }
#endif
