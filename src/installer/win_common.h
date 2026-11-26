#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace winlvm::installer {

inline constexpr wchar_t kProductName[] = L"WinLVM";
inline constexpr wchar_t kProductVersion[] = L"1.0.0";
inline constexpr wchar_t kPublisherName[] = L"WinLVM Project";
inline constexpr wchar_t kServiceName[] = L"winlvm-svc";
inline constexpr wchar_t kServiceDisplayName[] = L"WinLVM Control Service";
inline constexpr wchar_t kBusHardwareId[] = L"ROOT\\winlvm";
inline constexpr wchar_t kGuiExeName[] = L"winlvm_gui.exe";
inline constexpr wchar_t kUninstallRegSubkey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WinLVM";
inline constexpr wchar_t kRunRegSubkey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kRunValueName[] = L"WinLVM";
inline constexpr wchar_t kEnvRegSubkey[] =
    L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";

inline constexpr int kExitOk = 0;
inline constexpr int kExitRebootRequired = 3010;
inline constexpr int kExitError = 1;

class Logger {
 public:
  using Sink = std::function<void(const std::wstring&)>;

  void set_file(const std::wstring& path);
  void set_sink(Sink sink);
  void set_stderr_echo(bool enabled);

  void info(const std::wstring& msg);
  void warn(const std::wstring& msg);
  void error(const std::wstring& msg);
  std::wstring last_error() const;

 private:
  void write_line(const wchar_t* level, const std::wstring& msg);

  mutable std::mutex mu_;
  std::wstring log_path_;
  Sink sink_;
  bool stderr_echo_ = false;
  std::wstring last_error_;
};

std::wstring utf8_to_wide(const std::string& utf8);
std::string wide_to_utf8(const std::wstring& wide);
std::wstring format_win32_error(DWORD err);

std::wstring get_self_exe_path();
std::wstring get_default_install_dir();
std::wstring join_path(const std::wstring& base, const std::wstring& rel);
std::wstring normalize_dir_path(const std::wstring& path);
bool ensure_directory_tree(const std::wstring& dir, Logger& log);
bool write_file_atomic(const std::wstring& dest_path,
                       const uint8_t* data,
                       size_t len,
                       Logger& log);

bool is_testsigning_enabled();
bool import_certificate_to_machine_stores(const uint8_t* data,
                                          size_t len,
                                          Logger& log);
bool import_certificate_file_to_machine_stores(const std::wstring& cer_path,
                                               Logger& log);

void stop_gui_processes(Logger& log);
bool stop_service_if_running(const wchar_t* service_name,
                             DWORD timeout_ms,
                             Logger& log);
bool delete_service_if_exists(const wchar_t* service_name,
                              DWORD timeout_ms,
                              Logger& log);
bool install_and_start_service(const std::wstring& svc_exe_path, Logger& log);

bool stage_volume_driver(HWND hwnd,
                         const std::wstring& vol_inf_path,
                         bool& need_reboot,
                         Logger& log);
bool install_or_update_bus_driver(HWND hwnd,
                                  const std::wstring& bus_inf_path,
                                  bool non_interactive,
                                  bool& need_reboot,
                                  Logger& log);
bool remove_bus_devnodes(bool& need_reboot, Logger& log);
bool remove_oem_driver_packages(bool& need_reboot, Logger& log);

bool add_dir_to_system_path(const std::wstring& bin_dir, Logger& log);
bool remove_dir_from_system_path(const std::wstring& bin_dir, Logger& log);

bool create_start_menu_shortcut(const std::wstring& gui_exe_path, Logger& log);
void remove_start_menu_shortcut(Logger& log);

bool set_tray_autostart(const std::wstring& gui_exe_path,
                        bool enabled,
                        Logger& log);
bool register_uninstall_entry(const std::wstring& install_dir,
                              const std::wstring& uninstaller_path,
                              const std::wstring& display_icon_path,
                              uint64_t estimated_kb,
                              Logger& log);
bool read_installed_location(std::wstring& out_dir);
void remove_uninstall_entry(Logger& log);

UINT get_window_dpi(HWND hwnd);
int scale_dpi(int value, UINT dpi);
HFONT create_ui_font(UINT dpi, int point_size, int weight = FW_NORMAL);

}  // namespace winlvm::installer
