#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsvc.h>
#include <sddl.h>
#include <vector>
#pragma comment(lib, "advapi32.lib")
#endif

#include "protocol.h"
#ifdef _WIN32
#include "../driver/winlvm.h"
#endif

#ifdef _WIN32
namespace {
constexpr wchar_t kPipeName[] = LR"(\\.\pipe\winlvm-svc)";
constexpr wchar_t kDeviceName[] = LR"(\\.\WinLvmControl)";
constexpr DWORD kPipeBuffer = 64U * 1024U;
constexpr DWORD kIoctlInventory = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);

bool read_exact(HANDLE pipe, void* buffer, DWORD size) {
  DWORD received = 0;
  return ReadFile(pipe, buffer, size, &received, nullptr) && received == size;
}
bool write_exact(HANDLE pipe, const void* buffer, DWORD size) {
  DWORD sent = 0;
  return WriteFile(pipe, buffer, size, &sent, nullptr) && sent == size;
}
HANDLE open_pipe() {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) return INVALID_HANDLE_VALUE;
  SECURITY_ATTRIBUTES attributes{sizeof attributes, descriptor, FALSE};
  HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, kPipeBuffer, kPipeBuffer, 0, &attributes);
  LocalFree(descriptor);
  return pipe;
}
void serve_client(HANDLE pipe) {
  std::uint32_t frame_size = 0;
  if (!read_exact(pipe, &frame_size, sizeof frame_size) || frame_size > winlvm::service::kMaxFramePayload + 20U) return;
  std::vector<std::byte> frame(frame_size);
  if (!read_exact(pipe, frame.data(), frame_size)) return;
  const auto parsed = winlvm::service::parse_frame(frame);
  std::uint32_t result = static_cast<std::uint32_t>(parsed.ok ? winlvm::service::execute_request(parsed.request, true) : parsed.error);
  if (parsed.ok && parsed.request.operation == winlvm::service::Operation::Inventory) {
    HANDLE device = CreateFileW(kDeviceName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (device == INVALID_HANDLE_VALUE) result = static_cast<std::uint32_t>(winlvm::service::ErrorCode::DriverUnavailable);
    else { DWORD returned = 0; WINLVM_CONTROL_REQUEST request{1, sizeof(WINLVM_CONTROL_REQUEST)}; WINLVM_INVENTORY inventory{}; if (!DeviceIoControl(device, kIoctlInventory, &request, sizeof request, &inventory, sizeof inventory, &returned, nullptr)) result = static_cast<std::uint32_t>(winlvm::service::ErrorCode::DriverUnavailable); CloseHandle(device); }
  }
  write_exact(pipe, &result, sizeof result);
}
#endif

#ifdef _WIN32
namespace {
SERVICE_STATUS_HANDLE g_status_handle;
void WINAPI service_main(DWORD, LPWSTR*) {
  SERVICE_STATUS status{SERVICE_WIN32_OWN_PROCESS, SERVICE_RUNNING, 0, 0, 0, 0, 0};
  g_status_handle = RegisterServiceCtrlHandlerW(L"winlvm-svc", [](DWORD control) {
    if (control == SERVICE_CONTROL_STOP) {
      SERVICE_STATUS stopped{SERVICE_WIN32_OWN_PROCESS, SERVICE_STOPPED, 0, 0, 0, 0, 0};
      SetServiceStatus(g_status_handle, &stopped);
    }
  });
  if (g_status_handle == nullptr) return;
  SetServiceStatus(g_status_handle, &status);
  HANDLE pipe = open_pipe();
  if (pipe == INVALID_HANDLE_VALUE) return;
  if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) serve_client(pipe);
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
  status.dwCurrentState = SERVICE_STOPPED;
  SetServiceStatus(g_status_handle, &status);
}
}
int wmain() {
  SERVICE_TABLE_ENTRYW table[]{{const_cast<LPWSTR>(L"winlvm-svc"), service_main}, {nullptr, nullptr}};
  return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
}
#else
int main() { return 0; }
#endif
