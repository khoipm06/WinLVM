#include <atomic>
#include <iostream>
#include <memory>
#include <string>

#include "winlvm/activation.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

std::atomic_bool g_stop_requested{false};

#ifdef _WIN32
SERVICE_STATUS_HANDLE g_service_status_handle = nullptr;
SERVICE_STATUS g_service_status{};

void report_service_status(DWORD current_state, DWORD win32_exit_code, DWORD wait_hint) {
  static DWORD checkpoint = 1;
  g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_service_status.dwCurrentState = current_state;
  g_service_status.dwWin32ExitCode = win32_exit_code;
  g_service_status.dwWaitHint = wait_hint;
  g_service_status.dwControlsAccepted = current_state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
  g_service_status.dwCheckPoint = (current_state == SERVICE_RUNNING || current_state == SERVICE_STOPPED) ? 0 : checkpoint++;
  SetServiceStatus(g_service_status_handle, &g_service_status);
}

DWORD WINAPI service_control_handler(DWORD control, DWORD, LPVOID, LPVOID) {
  switch (control) {
    case SERVICE_CONTROL_STOP:
      report_service_status(SERVICE_STOP_PENDING, NO_ERROR, 1000);
      g_stop_requested = true;
      return NO_ERROR;
    default:
      return NO_ERROR;
  }
}

void WINAPI service_main(DWORD, LPWSTR*) {
  g_service_status_handle = RegisterServiceCtrlHandlerExW(L"lvm2d", service_control_handler, nullptr);
  if (!g_service_status_handle) {
    return;
  }

  report_service_status(SERVICE_START_PENDING, NO_ERROR, 1000);
  auto manager = winlvm::create_activation_manager();
  if (!manager) {
    report_service_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
    return;
  }

  report_service_status(SERVICE_RUNNING, NO_ERROR, 0);
  while (!g_stop_requested) {
    Sleep(500);
  }
  report_service_status(SERVICE_STOPPED, NO_ERROR, 0);
}
#endif

int run_console_mode() {
  auto manager = winlvm::create_activation_manager();
  if (!manager) {
    std::cerr << "failed to create activation manager\n";
    return 1;
  }
  std::cout << "lvm2d activation service skeleton running in console mode\n";
  std::cout << "press Ctrl+C to stop\n";
#ifdef _WIN32
  while (!g_stop_requested) {
    Sleep(1000);
  }
#else
  return 0;
#endif
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--console") {
    return run_console_mode();
  }

#ifdef _WIN32
  SERVICE_TABLE_ENTRYW service_table[] = {
      {const_cast<LPWSTR>(L"lvm2d"), service_main},
      {nullptr, nullptr},
  };
  if (StartServiceCtrlDispatcherW(service_table)) {
    return 0;
  }
  return run_console_mode();
#else
  return run_console_mode();
#endif
}
