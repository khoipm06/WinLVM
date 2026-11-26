#include "win_common.h"

#include "env_path.h"

#include <cfgmgr32.h>
#include <newdev.h>
#include <objbase.h>
#include <setupapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <winsvc.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace winlvm::installer {
namespace {

template <typename Fn>
Fn load_proc(HMODULE mod, const char* name) {
  if (mod == nullptr) return nullptr;
  FARPROC raw = GetProcAddress(mod, name);
  if (raw == nullptr) return nullptr;
  Fn fn = nullptr;
  static_assert(sizeof(fn) == sizeof(raw));
  std::memcpy(&fn, &raw, sizeof(fn));
  return fn;
}

using FnDiInstallDriverW = BOOL(WINAPI*)(HWND, LPCWSTR, DWORD, PBOOL);
using FnDiUninstallDriverW = BOOL(WINAPI*)(HWND, LPCWSTR, DWORD, PBOOL);
using FnUpdateDriverW = BOOL(WINAPI*)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);

struct NewDevApi {
  HMODULE mod = nullptr;
  FnDiInstallDriverW di_install_driver = nullptr;
  FnDiUninstallDriverW di_uninstall_driver = nullptr;
  FnUpdateDriverW update_driver = nullptr;

  NewDevApi() {
    mod = LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (mod != nullptr) {
      di_install_driver =
          load_proc<FnDiInstallDriverW>(mod, "DiInstallDriverW");
      di_uninstall_driver =
          load_proc<FnDiUninstallDriverW>(mod, "DiUninstallDriverW");
      update_driver = load_proc<FnUpdateDriverW>(
          mod, "UpdateDriverForPlugAndPlayDevicesW");
    }
  }

  ~NewDevApi() {
    if (mod != nullptr) FreeLibrary(mod);
  }
};

struct LocalCodeIntegrityInfo {
  ULONG length;
  ULONG options;
};

using FnNtQuerySystemInformation = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
constexpr ULONG kSystemCodeIntegrityInformation = 103u;
constexpr ULONG kCodeIntegrityOptionTestSign = 0x02u;

bool multi_sz_contains_ci(const wchar_t* buf,
                          size_t wchar_count,
                          const wchar_t* target) {
  if (buf == nullptr || wchar_count == 0u || target == nullptr) return false;
  size_t pos = 0u;
  while (pos < wchar_count && buf[pos] != L'\0') {
    const wchar_t* entry = buf + pos;
    const size_t len = wcsnlen(entry, wchar_count - pos);
    if (_wcsicmp(entry, target) == 0) return true;
    pos += len + 1u;
  }
  return false;
}

bool devnode_matches_hwid(HDEVINFO dev_info,
                          SP_DEVINFO_DATA& dev_data,
                          const wchar_t* target_hwid) {
  DWORD reg_type = 0u;
  DWORD needed = 0u;
  SetupDiGetDeviceRegistryPropertyW(dev_info, &dev_data, SPDRP_HARDWAREID,
                                    &reg_type, nullptr, 0u, &needed);
  if (needed == 0u || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return false;
  }
  std::vector<wchar_t> buf((needed / sizeof(wchar_t)) + 2u, L'\0');
  if (!SetupDiGetDeviceRegistryPropertyW(
          dev_info, &dev_data, SPDRP_HARDWAREID, &reg_type,
          reinterpret_cast<PBYTE>(buf.data()),
          static_cast<DWORD>(buf.size() * sizeof(wchar_t)), nullptr)) {
    return false;
  }
  if (reg_type != REG_MULTI_SZ && reg_type != REG_SZ) return false;
  return multi_sz_contains_ci(buf.data(), buf.size(), target_hwid);
}

void broadcast_environment_change() {
  DWORD_PTR result = 0u;
  SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0u,
                      reinterpret_cast<LPARAM>(L"Environment"),
                      SMTO_ABORTIFHUNG, 2000u, &result);
}

std::wstring ascii_lower_w(const std::wstring& s) {
  std::wstring out = s;
  for (wchar_t& ch : out) {
    if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
  }
  return out;
}

bool read_inf_text(const std::wstring& path, std::wstring& text_out) {
  std::ifstream in(std::filesystem::path(path), std::ios::binary);
  if (!in.is_open()) return false;
  std::string bytes((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
  if (bytes.size() >= 2u && static_cast<unsigned char>(bytes[0]) == 0xffu &&
      static_cast<unsigned char>(bytes[1]) == 0xfeu) {
    const size_t wlen = (bytes.size() - 2u) / 2u;
    text_out.resize(wlen);
    if (wlen > 0u) {
      std::memcpy(text_out.data(), bytes.data() + 2u, wlen * sizeof(wchar_t));
    }
    return true;
  }
  text_out = utf8_to_wide(bytes);
  return true;
}

bool inf_is_winlvm_package(const std::wstring& inf_path) {
  std::wstring content;
  if (!read_inf_text(inf_path, content)) return false;
  const std::wstring low = ascii_lower_w(content);
  const bool has_provider =
      (low.find(L"\"winlvm\"") != std::wstring::npos) ||
      (low.find(L"provider=winlvm") != std::wstring::npos);
  const bool has_hwid =
      (low.find(L"root\\winlvm") != std::wstring::npos) ||
      (low.find(L"winlvm\\volume") != std::wstring::npos);
  return has_provider && has_hwid;
}

struct ClosePidContext {
  std::vector<DWORD> pids;
};

BOOL CALLBACK close_matching_windows_cb(HWND hwnd, LPARAM lparam) {
  const auto* ctx = reinterpret_cast<const ClosePidContext*>(lparam);
  DWORD pid = 0u;
  GetWindowThreadProcessId(hwnd, &pid);
  if (std::find(ctx->pids.begin(), ctx->pids.end(), pid) != ctx->pids.end()) {
    PostMessageW(hwnd, WM_CLOSE, 0u, 0);
  }
  return TRUE;
}

bool write_reg_sz(HKEY key, const wchar_t* name, const std::wstring& val) {
  const DWORD bytes = static_cast<DWORD>((val.size() + 1u) * sizeof(wchar_t));
  return RegSetValueExW(key, name, 0u, REG_SZ,
                        reinterpret_cast<const BYTE*>(val.c_str()),
                        bytes) == ERROR_SUCCESS;
}

bool write_reg_dword(HKEY key, const wchar_t* name, DWORD val) {
  return RegSetValueExW(key, name, 0u, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&val),
                        sizeof(val)) == ERROR_SUCCESS;
}

bool get_common_start_menu_dir(std::wstring& out_dir) {
  PWSTR known = nullptr;
  if (SUCCEEDED(
          SHGetKnownFolderPath(FOLDERID_CommonPrograms, 0u, nullptr, &known)) &&
      known != nullptr) {
    out_dir = join_path(known, L"WinLVM");
    CoTaskMemFree(known);
    return true;
  }
  return false;
}

}  // namespace

void Logger::set_file(const std::wstring& path) {
  std::lock_guard<std::mutex> lock(mu_);
  log_path_ = path;
}

void Logger::set_sink(Sink sink) {
  std::lock_guard<std::mutex> lock(mu_);
  sink_ = std::move(sink);
}

void Logger::set_stderr_echo(bool enabled) {
  std::lock_guard<std::mutex> lock(mu_);
  stderr_echo_ = enabled;
}

void Logger::info(const std::wstring& msg) { write_line(L"INFO", msg); }
void Logger::warn(const std::wstring& msg) { write_line(L"WARN", msg); }

void Logger::error(const std::wstring& msg) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = msg;
  }
  write_line(L"ERROR", msg);
}

std::wstring Logger::last_error() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_error_;
}

void Logger::write_line(const wchar_t* level, const std::wstring& msg) {
  const std::wstring line = std::wstring(L"[") + level + L"] " + msg;
  Sink sink_copy;
  std::wstring log_file_copy;
  bool echo = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    sink_copy = sink_;
    log_file_copy = log_path_;
    echo = stderr_echo_;
  }
  if (!log_file_copy.empty()) {
    const std::string utf8 = wide_to_utf8(line) + "\r\n";
    HANDLE fh = CreateFileW(log_file_copy.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh != INVALID_HANDLE_VALUE) {
      DWORD written = 0u;
      WriteFile(fh, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                nullptr);
      CloseHandle(fh);
    }
  }
  if (echo) {
    const std::string utf8 = wide_to_utf8(line) + "\n";
    std::fwrite(utf8.data(), 1u, utf8.size(), stderr);
    std::fflush(stderr);
  }
  if (sink_copy) {
    sink_copy(line);
  }
}

std::wstring utf8_to_wide(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int needed = MultiByteToWideChar(
      CP_UTF8, 0u, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring out(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0u, utf8.data(), static_cast<int>(utf8.size()),
                      out.data(), needed);
  return out;
}

std::string wide_to_utf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int needed = WideCharToMultiByte(
      CP_UTF8, 0u, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
      nullptr, nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0u, wide.data(), static_cast<int>(wide.size()),
                      out.data(), needed, nullptr, nullptr);
  return out;
}

std::wstring format_win32_error(DWORD err) {
  wchar_t* msg_buf = nullptr;
  const DWORD flags =
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
      FORMAT_MESSAGE_IGNORE_INSERTS;
  DWORD len = FormatMessageW(
      flags, nullptr, err, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
      reinterpret_cast<LPWSTR>(&msg_buf), 0u, nullptr);
  if (len == 0u) {
    len = FormatMessageW(flags, nullptr, err, 0u,
                         reinterpret_cast<LPWSTR>(&msg_buf), 0u, nullptr);
  }
  std::wstring text;
  if (len > 0u && msg_buf != nullptr) {
    text.assign(msg_buf, len);
    while (!text.empty() &&
           (text.back() == L'\r' || text.back() == L'\n' ||
            text.back() == L' ')) {
      text.pop_back();
    }
    LocalFree(msg_buf);
  }
  wchar_t code_buf[32]{};
  swprintf_s(code_buf, L"0x%08lX", static_cast<unsigned long>(err));
  if (text.empty()) return std::wstring(L"Win32 error ") + code_buf;
  return text + L" (" + code_buf + L")";
}

std::wstring get_self_exe_path() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD len =
        GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0u) return {};
    if (len < buf.size() - 1u) {
      buf.resize(len);
      return buf;
    }
    if (buf.size() >= 32768u) return {};
    buf.resize(buf.size() * 2u, L'\0');
  }
}

std::wstring get_default_install_dir() {
  PWSTR pf = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0u, nullptr, &pf)) &&
      pf != nullptr) {
    std::wstring dir = join_path(pf, L"WinLVM");
    CoTaskMemFree(pf);
    return dir;
  }
  return L"C:\\Program Files\\WinLVM";
}

std::wstring join_path(const std::wstring& base, const std::wstring& rel) {
  if (base.empty()) return rel;
  if (rel.empty()) return base;
  std::wstring out = base;
  while (out.size() > 3u && (out.back() == L'\\' || out.back() == L'/')) {
    out.pop_back();
  }
  if (out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
  size_t pos = 0u;
  while (pos < rel.size() && (rel[pos] == L'\\' || rel[pos] == L'/')) ++pos;
  for (; pos < rel.size(); ++pos) {
    out.push_back(rel[pos] == L'/' ? L'\\' : rel[pos]);
  }
  return out;
}

std::wstring normalize_dir_path(const std::wstring& path) {
  if (path.empty()) return {};
  std::wstring resolved = path;
  const DWORD needed = GetFullPathNameW(path.c_str(), 0u, nullptr, nullptr);
  if (needed > 1u) {
    std::wstring full(needed, L'\0');
    const DWORD written =
        GetFullPathNameW(path.c_str(), needed, full.data(), nullptr);
    if (written > 0u && written < needed) {
      full.resize(written);
      resolved = std::move(full);
    }
  }
  for (wchar_t& ch : resolved) {
    if (ch == L'/') ch = L'\\';
  }
  while (resolved.size() > 3u && resolved.back() == L'\\') {
    resolved.pop_back();
  }
  return resolved;
}

bool ensure_directory_tree(const std::wstring& dir, Logger& log) {
  if (dir.empty()) return false;
  std::wstring norm = normalize_dir_path(dir);
  for (size_t i = 0u; i < norm.size(); ++i) {
    if (norm[i] == L'\\' && i > 0u) {
      if (i == 2u && norm[1] == L':') continue;
      const std::wstring sub = norm.substr(0u, i);
      if (!CreateDirectoryW(sub.c_str(), nullptr)) {
        const DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
          log.error(L"Failed to create directory " + sub + L": " +
                    format_win32_error(err));
          return false;
        }
      }
    }
  }
  if (!CreateDirectoryW(norm.c_str(), nullptr)) {
    const DWORD err = GetLastError();
    if (err != ERROR_ALREADY_EXISTS) {
      log.error(L"Failed to create directory " + norm + L": " +
                format_win32_error(err));
      return false;
    }
  }
  return true;
}

bool write_file_atomic(const std::wstring& dest_path,
                       const uint8_t* data,
                       size_t len,
                       Logger& log) {
  const size_t slash = dest_path.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    if (!ensure_directory_tree(dest_path.substr(0u, slash), log)) return false;
  }

  const std::wstring tmp_path = dest_path + L".winlvm.tmp";
  HANDLE fh = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0u, nullptr,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (fh == INVALID_HANDLE_VALUE) {
    log.error(L"Failed to create temporary file " + tmp_path + L": " +
              format_win32_error(GetLastError()));
    return false;
  }

  size_t off = 0u;
  while (off < len) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<size_t>(len - off, 4u * 1024u * 1024u));
    DWORD written = 0u;
    if (!WriteFile(fh, data + off, chunk, &written, nullptr) ||
        written != chunk) {
      const DWORD err = GetLastError();
      CloseHandle(fh);
      DeleteFileW(tmp_path.c_str());
      log.error(L"Failed writing " + tmp_path + L": " +
                format_win32_error(err));
      return false;
    }
    off += written;
  }
  FlushFileBuffers(fh);
  CloseHandle(fh);

  if (!MoveFileExW(tmp_path.c_str(), dest_path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD err = GetLastError();
    DeleteFileW(tmp_path.c_str());
    log.error(L"Failed moving " + tmp_path + L" to " + dest_path + L": " +
              format_win32_error(err));
    return false;
  }
  return true;
}

bool is_testsigning_enabled() {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  auto fn = load_proc<FnNtQuerySystemInformation>(ntdll,
                                                  "NtQuerySystemInformation");
  if (fn == nullptr) return false;
  LocalCodeIntegrityInfo info{};
  info.length = sizeof(info);
  ULONG ret_len = 0u;
  const LONG status =
      fn(kSystemCodeIntegrityInformation, &info, sizeof(info), &ret_len);
  if (status < 0) return false;
  return (info.options & kCodeIntegrityOptionTestSign) != 0u;
}

bool import_certificate_to_machine_stores(const uint8_t* data,
                                          size_t len,
                                          Logger& log) {
  if (data == nullptr || len == 0u) {
    log.error(L"Certificate payload is empty.");
    return false;
  }

  std::vector<BYTE> der(data, data + len);
  // If the certificate is PEM/Base64 encoded, decode it to DER first.
  const bool looks_pem =
      (len >= 10u && std::memcmp(data, "-----BEGIN", 10u) == 0);
  if (looks_pem) {
    DWORD der_len = 0u;
    if (CryptStringToBinaryA(reinterpret_cast<const char*>(data),
                             static_cast<DWORD>(len), CRYPT_STRING_BASE64HEADER,
                             nullptr, &der_len, nullptr, nullptr) &&
        der_len > 0u) {
      std::vector<BYTE> decoded(der_len);
      if (CryptStringToBinaryA(reinterpret_cast<const char*>(data),
                               static_cast<DWORD>(len),
                               CRYPT_STRING_BASE64HEADER, decoded.data(),
                               &der_len, nullptr, nullptr)) {
        decoded.resize(der_len);
        der = std::move(decoded);
      }
    }
  }

  const wchar_t* stores[] = {L"Root", L"TrustedPublisher"};
  for (const wchar_t* store_name : stores) {
    HCERTSTORE store = CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W, 0u, 0u,
        CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG,
        store_name);
    if (store == nullptr) {
      log.error(std::wstring(L"Failed to open LocalMachine\\") + store_name +
                L" certificate store: " + format_win32_error(GetLastError()));
      return false;
    }
    const BOOL ok = CertAddEncodedCertificateToStore(
        store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der.data(),
        static_cast<DWORD>(der.size()), CERT_STORE_ADD_REPLACE_EXISTING,
        nullptr);
    const DWORD err = GetLastError();
    CertCloseStore(store, 0u);
    if (!ok) {
      log.error(std::wstring(L"Failed to add certificate to LocalMachine\\") +
                store_name + L": " + format_win32_error(err));
      return false;
    }
    log.info(std::wstring(L"Imported certificate into LocalMachine\\") +
             store_name + L".");
  }
  return true;
}

bool import_certificate_file_to_machine_stores(const std::wstring& cer_path,
                                               Logger& log) {
  std::ifstream in(std::filesystem::path(cer_path),
                   std::ios::binary | std::ios::ate);
  if (!in.is_open()) {
    log.error(L"Failed to read certificate file: " + cer_path);
    return false;
  }
  const auto sz = static_cast<size_t>(in.tellg());
  std::vector<uint8_t> bytes(sz);
  in.seekg(0, std::ios::beg);
  if (sz > 0u &&
      !in.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(sz))) {
    log.error(L"Failed to read certificate contents: " + cer_path);
    return false;
  }
  return import_certificate_to_machine_stores(bytes.data(), bytes.size(), log);
}

void stop_gui_processes(Logger& log) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0u);
  if (snap == INVALID_HANDLE_VALUE) return;

  ClosePidContext ctx;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, kGuiExeName) == 0) {
        ctx.pids.push_back(pe.th32ProcessID);
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);

  if (ctx.pids.empty()) return;
  log.info(L"Stopping running WinLVM Volume Manager instances...");
  EnumWindows(close_matching_windows_cb, reinterpret_cast<LPARAM>(&ctx));

  for (DWORD pid : ctx.pids) {
    HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
    if (proc == nullptr) continue;
    if (WaitForSingleObject(proc, 3000u) != WAIT_OBJECT_0) {
      TerminateProcess(proc, 0u);
      WaitForSingleObject(proc, 3000u);
    }
    CloseHandle(proc);
  }
}

bool stop_service_if_running(const wchar_t* service_name,
                             DWORD timeout_ms,
                             Logger& log) {
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (scm == nullptr) {
    log.error(L"OpenSCManagerW failed: " + format_win32_error(GetLastError()));
    return false;
  }
  SC_HANDLE svc = OpenServiceW(scm, service_name,
                               SERVICE_STOP | SERVICE_QUERY_STATUS);
  if (svc == nullptr) {
    const DWORD err = GetLastError();
    CloseServiceHandle(scm);
    if (err == ERROR_SERVICE_DOES_NOT_EXIST) return true;
    log.error(std::wstring(L"OpenServiceW(") + service_name + L") failed: " +
              format_win32_error(err));
    return false;
  }

  SERVICE_STATUS_PROCESS ssp{};
  DWORD bytes_needed = 0u;
  if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                            reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp),
                            &bytes_needed)) {
    const DWORD err = GetLastError();
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    log.error(L"QueryServiceStatusEx failed: " + format_win32_error(err));
    return false;
  }

  if (ssp.dwCurrentState == SERVICE_STOPPED) {
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
  }

  log.info(std::wstring(L"Stopping service ") + service_name + L"...");
  SERVICE_STATUS ctrl_status{};
  if (ssp.dwCurrentState != SERVICE_STOP_PENDING) {
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &ctrl_status)) {
      const DWORD err = GetLastError();
      if (err != ERROR_SERVICE_NOT_ACTIVE) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        log.error(L"ControlService(STOP) failed: " + format_win32_error(err));
        return false;
      }
    }
  }

  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  while (GetTickCount64() < deadline) {
    Sleep(200u);
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp),
                              &bytes_needed)) {
      break;
    }
    if (ssp.dwCurrentState == SERVICE_STOPPED) {
      CloseServiceHandle(svc);
      CloseServiceHandle(scm);
      return true;
    }
  }

  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  log.error(std::wstring(L"Timed out waiting for service ") + service_name +
            L" to stop.");
  return false;
}

bool delete_service_if_exists(const wchar_t* service_name,
                              DWORD timeout_ms,
                              Logger& log) {
  if (!stop_service_if_running(service_name, timeout_ms, log)) {
    return false;
  }
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (scm == nullptr) {
    log.error(L"OpenSCManagerW failed: " + format_win32_error(GetLastError()));
    return false;
  }
  SC_HANDLE svc = OpenServiceW(scm, service_name, DELETE);
  if (svc == nullptr) {
    const DWORD err = GetLastError();
    CloseServiceHandle(scm);
    if (err == ERROR_SERVICE_DOES_NOT_EXIST) return true;
    log.error(std::wstring(L"OpenServiceW(DELETE) failed: ") +
              format_win32_error(err));
    return false;
  }
  const BOOL ok = DeleteService(svc);
  const DWORD err = GetLastError();
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  if (!ok && err != ERROR_SERVICE_MARKED_FOR_DELETE) {
    log.error(L"DeleteService failed: " + format_win32_error(err));
    return false;
  }
  log.info(std::wstring(L"Removed service ") + service_name + L".");
  return true;
}

bool install_and_start_service(const std::wstring& svc_exe_path, Logger& log) {
  SC_HANDLE scm =
      OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
  if (scm == nullptr) {
    log.error(L"OpenSCManagerW failed: " + format_win32_error(GetLastError()));
    return false;
  }

  const std::wstring bin_path = L"\"" + svc_exe_path + L"\"";
  SC_HANDLE svc = CreateServiceW(
      scm, kServiceName, kServiceDisplayName,
      SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
      SERVICE_ERROR_NORMAL, bin_path.c_str(), nullptr, nullptr, nullptr,
      nullptr, nullptr);
  if (svc == nullptr) {
    const DWORD err = GetLastError();
    if (err == ERROR_SERVICE_EXISTS) {
      svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
      if (svc == nullptr) {
        log.error(L"OpenServiceW failed on upgrade: " +
                  format_win32_error(GetLastError()));
        CloseServiceHandle(scm);
        return false;
      }
      if (!ChangeServiceConfigW(
              svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
              SERVICE_ERROR_NORMAL, bin_path.c_str(), nullptr, nullptr, nullptr,
              nullptr, nullptr, kServiceDisplayName)) {
        log.error(L"ChangeServiceConfigW failed: " +
                  format_win32_error(GetLastError()));
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return false;
      }
    } else {
      log.error(L"CreateServiceW failed: " + format_win32_error(err));
      CloseServiceHandle(scm);
      return false;
    }
  }

  std::wstring desc_text =
      L"Manages WinLVM logical volume discovery and control operations.";
  SERVICE_DESCRIPTIONW desc{};
  desc.lpDescription = desc_text.data();
  ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

  SC_ACTION actions[3]{};
  actions[0].Type = SC_ACTION_RESTART;
  actions[0].Delay = 5000u;
  actions[1].Type = SC_ACTION_RESTART;
  actions[1].Delay = 10000u;
  actions[2].Type = SC_ACTION_NONE;
  actions[2].Delay = 0u;
  SERVICE_FAILURE_ACTIONSW fail_actions{};
  fail_actions.dwResetPeriod = 86400u;
  fail_actions.cActions = 3u;
  fail_actions.lpsaActions = actions;
  ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fail_actions);

  log.info(std::wstring(L"Starting service ") + kServiceName + L"...");
  if (!StartServiceW(svc, 0u, nullptr)) {
    const DWORD err = GetLastError();
    if (err != ERROR_SERVICE_ALREADY_RUNNING) {
      log.error(L"StartServiceW failed: " + format_win32_error(err));
      CloseServiceHandle(svc);
      CloseServiceHandle(scm);
      return false;
    }
  }

  const ULONGLONG deadline = GetTickCount64() + 10000u;
  SERVICE_STATUS_PROCESS ssp{};
  DWORD bytes_needed = 0u;
  bool running = false;
  while (GetTickCount64() < deadline) {
    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp),
                             &bytes_needed)) {
      if (ssp.dwCurrentState == SERVICE_RUNNING) {
        running = true;
        break;
      }
      if (ssp.dwCurrentState == SERVICE_STOPPED) break;
    }
    Sleep(200u);
  }

  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  if (!running) {
    log.error(L"Service winlvm-svc did not reach SERVICE_RUNNING state.");
    return false;
  }
  log.info(L"Service winlvm-svc is running.");
  return true;
}

bool stage_volume_driver(HWND hwnd,
                         const std::wstring& vol_inf_path,
                         bool& need_reboot,
                         Logger& log) {
  NewDevApi api;
  if (api.di_install_driver == nullptr) {
    log.error(L"newdev.dll does not export DiInstallDriverW.");
    return false;
  }
  log.info(L"Staging volume driver package: " + vol_inf_path);
  BOOL reboot = FALSE;
  if (!api.di_install_driver(hwnd, vol_inf_path.c_str(), DIIRFLAG_FORCE_INF,
                             &reboot)) {
    const DWORD err = GetLastError();
    log.error(L"DiInstallDriverW failed for winlvm-vol.inf: " +
              format_win32_error(err));
    return false;
  }
  if (reboot) need_reboot = true;
  log.info(L"Volume driver package staged cleanly.");
  return true;
}

bool install_or_update_bus_driver(HWND hwnd,
                                  const std::wstring& bus_inf_path,
                                  bool non_interactive,
                                  bool& need_reboot,
                                  Logger& log) {
  NewDevApi api;
  if (api.update_driver == nullptr) {
    log.error(
        L"newdev.dll does not export UpdateDriverForPlugAndPlayDevicesW.");
    return false;
  }

  bool existing_devnode = false;
  HDEVINFO root_set =
      SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES);
  if (root_set != INVALID_HANDLE_VALUE) {
    SP_DEVINFO_DATA dev_data{};
    dev_data.cbSize = sizeof(dev_data);
    for (DWORD idx = 0u; SetupDiEnumDeviceInfo(root_set, idx, &dev_data);
         ++idx) {
      if (devnode_matches_hwid(root_set, dev_data, kBusHardwareId)) {
        existing_devnode = true;
        break;
      }
    }
    SetupDiDestroyDeviceInfoList(root_set);
  }

  HDEVINFO created_set = INVALID_HANDLE_VALUE;
  SP_DEVINFO_DATA created_data{};
  created_data.cbSize = sizeof(created_data);

  if (!existing_devnode) {
    log.info(L"Creating ROOT\\winlvm bus device node...");
    GUID class_guid{};
    wchar_t class_name[MAX_CLASS_NAME_LEN]{};
    if (!SetupDiGetINFClassW(bus_inf_path.c_str(), &class_guid, class_name,
                             MAX_CLASS_NAME_LEN, nullptr)) {
      log.error(L"SetupDiGetINFClassW failed: " +
                format_win32_error(GetLastError()));
      return false;
    }

    created_set = SetupDiCreateDeviceInfoList(&class_guid, hwnd);
    if (created_set == INVALID_HANDLE_VALUE) {
      log.error(L"SetupDiCreateDeviceInfoList failed: " +
                format_win32_error(GetLastError()));
      return false;
    }

    if (!SetupDiCreateDeviceInfoW(created_set, class_name, &class_guid,
                                  L"WinLVM Volume Bus", hwnd, DICD_GENERATE_ID,
                                  &created_data)) {
      log.error(L"SetupDiCreateDeviceInfoW failed: " +
                format_win32_error(GetLastError()));
      SetupDiDestroyDeviceInfoList(created_set);
      return false;
    }

    // Double-NUL-terminated REG_MULTI_SZ hardware ID list.
    const wchar_t hwid_multi_sz[] = L"ROOT\\winlvm\0";
    if (!SetupDiSetDeviceRegistryPropertyW(
            created_set, &created_data, SPDRP_HARDWAREID,
            reinterpret_cast<const BYTE*>(hwid_multi_sz),
            sizeof(hwid_multi_sz))) {
      log.error(L"SetupDiSetDeviceRegistryPropertyW(HARDWAREID) failed: " +
                format_win32_error(GetLastError()));
      SetupDiDestroyDeviceInfoList(created_set);
      return false;
    }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, created_set,
                                   &created_data)) {
      log.error(L"SetupDiCallClassInstaller(DIF_REGISTERDEVICE) failed: " +
                format_win32_error(GetLastError()));
      SetupDiDestroyDeviceInfoList(created_set);
      return false;
    }
  } else {
    log.info(L"Existing ROOT\\winlvm bus device node detected; updating...");
  }

  DWORD install_flags = INSTALLFLAG_FORCE;
  if (non_interactive) {
    install_flags |= INSTALLFLAG_NONINTERACTIVE;
  }
  BOOL reboot = FALSE;
  const BOOL ok = api.update_driver(hwnd, kBusHardwareId, bus_inf_path.c_str(),
                                    install_flags, &reboot);
  const DWORD err = GetLastError();

  if (!ok) {
    log.error(L"UpdateDriverForPlugAndPlayDevicesW failed: " +
              format_win32_error(err));
    if (created_set != INVALID_HANDLE_VALUE) {
      SetupDiCallClassInstaller(DIF_REMOVE, created_set, &created_data);
      SetupDiDestroyDeviceInfoList(created_set);
    }
    return false;
  }

  if (created_set != INVALID_HANDLE_VALUE) {
    SetupDiDestroyDeviceInfoList(created_set);
  }
  if (reboot) need_reboot = true;
  log.info(L"ROOT\\winlvm bus driver installed and started.");
  return true;
}

bool remove_bus_devnodes(bool& need_reboot, Logger& log) {
  HDEVINFO root_set =
      SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES);
  if (root_set == INVALID_HANDLE_VALUE) {
    log.error(L"SetupDiGetClassDevsW(ROOT) failed: " +
              format_win32_error(GetLastError()));
    return false;
  }

  bool all_ok = true;
  int removed_count = 0;
  SP_DEVINFO_DATA dev_data{};
  dev_data.cbSize = sizeof(dev_data);
  for (DWORD idx = 0u; SetupDiEnumDeviceInfo(root_set, idx, &dev_data); ++idx) {
    if (!devnode_matches_hwid(root_set, dev_data, kBusHardwareId)) continue;
    log.info(L"Removing ROOT\\winlvm bus device node...");
    if (!SetupDiCallClassInstaller(DIF_REMOVE, root_set, &dev_data)) {
      log.error(L"SetupDiCallClassInstaller(DIF_REMOVE) failed: " +
                format_win32_error(GetLastError()));
      all_ok = false;
      continue;
    }
    SP_DEVINSTALL_PARAMS_W params{};
    params.cbSize = sizeof(params);
    if (SetupDiGetDeviceInstallParamsW(root_set, &dev_data, &params)) {
      if ((params.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0u) {
        need_reboot = true;
      }
    }
    ++removed_count;
  }
  SetupDiDestroyDeviceInfoList(root_set);
  if (removed_count == 0) {
    log.info(L"No ROOT\\winlvm bus device nodes present.");
  }

  HDEVINFO vol_set =
      SetupDiGetClassDevsW(nullptr, L"WINLVM", nullptr, DIGCF_ALLCLASSES);
  if (vol_set != INVALID_HANDLE_VALUE) {
    int vol_removed = 0;
    SP_DEVINFO_DATA vol_data{};
    vol_data.cbSize = sizeof(vol_data);
    for (DWORD idx = 0u; SetupDiEnumDeviceInfo(vol_set, idx, &vol_data); ++idx) {
      if (!devnode_matches_hwid(vol_set, vol_data, L"WinLvm\\Volume") &&
          !devnode_matches_hwid(vol_set, vol_data, L"WinLvmVolume")) {
        continue;
      }
      log.info(L"Removing residual WinLvm\\Volume child device node...");
      if (!SetupDiCallClassInstaller(DIF_REMOVE, vol_set, &vol_data)) {
        log.error(L"SetupDiCallClassInstaller(DIF_REMOVE) for volume failed: " +
                  format_win32_error(GetLastError()));
        all_ok = false;
        continue;
      }
      SP_DEVINSTALL_PARAMS_W params{};
      params.cbSize = sizeof(params);
      if (SetupDiGetDeviceInstallParamsW(vol_set, &vol_data, &params)) {
        if ((params.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0u) {
          need_reboot = true;
        }
      }
      ++vol_removed;
    }
    SetupDiDestroyDeviceInfoList(vol_set);
    if (vol_removed > 0) {
      log.info(L"Removed " + std::to_wstring(vol_removed) +
               L" residual WinLvm\\Volume child device node(s).");
    }
  } else {
    const DWORD err = GetLastError();
    if (err != ERROR_INVALID_DATA && err != ERROR_NO_MORE_ITEMS) {
      log.warn(L"SetupDiGetClassDevsW(WINLVM) returned: " +
               format_win32_error(err));
    }
  }

  return all_ok;
}

bool remove_oem_driver_packages(bool& need_reboot, Logger& log) {
  wchar_t win_dir[MAX_PATH]{};
  const UINT wlen = GetWindowsDirectoryW(win_dir, MAX_PATH);
  if (wlen == 0u || wlen >= MAX_PATH) {
    log.error(L"GetWindowsDirectoryW failed.");
    return false;
  }
  const std::wstring inf_dir = join_path(win_dir, L"INF");
  const std::wstring pattern = join_path(inf_dir, L"oem*.inf");

  WIN32_FIND_DATAW fd{};
  HANDLE find_h = FindFirstFileW(pattern.c_str(), &fd);
  if (find_h == INVALID_HANDLE_VALUE) return true;

  std::vector<std::wstring> matches;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) continue;
    const std::wstring full_path = join_path(inf_dir, fd.cFileName);
    if (inf_is_winlvm_package(full_path)) {
      matches.emplace_back(fd.cFileName);
    }
  } while (FindNextFileW(find_h, &fd));
  FindClose(find_h);

  NewDevApi api;
  bool all_ok = true;
  for (const auto& oem_name : matches) {
    const std::wstring full_path = join_path(inf_dir, oem_name);
    log.info(L"Removing WinLVM driver store package: " + oem_name);
    bool removed = false;
    if (api.di_uninstall_driver != nullptr) {
      BOOL reboot = FALSE;
      if (api.di_uninstall_driver(nullptr, full_path.c_str(), 0u, &reboot)) {
        if (reboot) need_reboot = true;
        removed = true;
      }
    }
    if (!removed) {
      if (SetupUninstallOEMInfW(oem_name.c_str(), SUOI_FORCEDELETE, nullptr)) {
        removed = true;
      } else {
        log.warn(L"SetupUninstallOEMInfW failed for " + oem_name + L": " +
                 format_win32_error(GetLastError()));
        all_ok = false;
      }
    }
  }
  return all_ok;
}

static bool query_reg_string_strict(HKEY key,
                                    const wchar_t* value_name,
                                    std::wstring& out,
                                    bool& exists,
                                    Logger* log) {
  out.clear();
  exists = false;
  for (int attempt = 0; attempt < 8; ++attempt) {
    DWORD type = 0u;
    DWORD bytes = 0u;
    LONG rc = RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &bytes);
    if (rc == ERROR_FILE_NOT_FOUND) {
      exists = false;
      out.clear();
      return true;
    }
    if (rc != ERROR_SUCCESS) {
      if (log != nullptr) {
        log->error(std::wstring(L"Failed to query registry value ") +
                   value_name + L": " +
                   format_win32_error(static_cast<DWORD>(rc)));
      }
      return false;
    }
    if (type != REG_EXPAND_SZ && type != REG_SZ) {
      if (log != nullptr) {
        log->error(std::wstring(L"Unexpected registry value type for ") +
                   value_name + L": " + std::to_wstring(type));
      }
      return false;
    }
    if (bytes == 0u) {
      exists = true;
      out.clear();
      return true;
    }

    std::vector<wchar_t> buf((bytes / sizeof(wchar_t)) + 2u, L'\0');
    DWORD read_bytes =
        static_cast<DWORD>((buf.size() - 1u) * sizeof(wchar_t));
    rc = RegQueryValueExW(key, value_name, nullptr, &type,
                          reinterpret_cast<LPBYTE>(buf.data()), &read_bytes);
    if (rc == ERROR_MORE_DATA) {
      continue;
    }
    if (rc != ERROR_SUCCESS) {
      if (log != nullptr) {
        log->error(std::wstring(L"Failed to read registry value ") +
                   value_name + L": " +
                   format_win32_error(static_cast<DWORD>(rc)));
      }
      return false;
    }
    if (type != REG_EXPAND_SZ && type != REG_SZ) {
      if (log != nullptr) {
        log->error(std::wstring(L"Unexpected registry value type for ") +
                   value_name + L": " + std::to_wstring(type));
      }
      return false;
    }
    exists = true;
    out = buf.data();
    return true;
  }

  if (log != nullptr) {
    log->error(std::wstring(L"Registry value ") + value_name +
               L" changed concurrently during read; aborting update.");
  }
  return false;
}

bool add_dir_to_system_path(const std::wstring& bin_dir, Logger& log) {
  HKEY key = nullptr;
  LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kEnvRegSubkey, 0u,
                          KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
  if (rc != ERROR_SUCCESS) {
    log.error(L"Failed to open Environment registry key: " +
              format_win32_error(static_cast<DWORD>(rc)));
    return false;
  }

  std::wstring current;
  bool exists = false;
  if (!query_reg_string_strict(key, L"Path", current, exists, &log)) {
    RegCloseKey(key);
    return false;
  }

  std::wstring updated;
  if (!path_list_add(current, bin_dir, updated)) {
    RegCloseKey(key);
    log.info(L"System PATH already contains " + bin_dir + L".");
    return true;
  }

  const DWORD out_bytes =
      static_cast<DWORD>((updated.size() + 1u) * sizeof(wchar_t));
  rc = RegSetValueExW(key, L"Path", 0u, REG_EXPAND_SZ,
                      reinterpret_cast<const BYTE*>(updated.c_str()),
                      out_bytes);
  RegCloseKey(key);
  if (rc != ERROR_SUCCESS) {
    log.error(L"Failed to update System PATH: " +
              format_win32_error(static_cast<DWORD>(rc)));
    return false;
  }
  broadcast_environment_change();
  log.info(L"Added " + bin_dir + L" to System PATH.");
  return true;
}

bool remove_dir_from_system_path(const std::wstring& bin_dir, Logger& log) {
  HKEY key = nullptr;
  LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kEnvRegSubkey, 0u,
                          KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
  if (rc != ERROR_SUCCESS) {
    log.warn(L"Failed to open Environment registry key: " +
             format_win32_error(static_cast<DWORD>(rc)));
    return false;
  }

  std::wstring current;
  bool exists = false;
  if (!query_reg_string_strict(key, L"Path", current, exists, &log)) {
    RegCloseKey(key);
    return false;
  }
  if (!exists) {
    RegCloseKey(key);
    return true;
  }

  std::wstring updated;
  if (!path_list_remove(current, bin_dir, updated)) {
    RegCloseKey(key);
    return true;
  }

  const DWORD out_bytes =
      static_cast<DWORD>((updated.size() + 1u) * sizeof(wchar_t));
  rc = RegSetValueExW(key, L"Path", 0u, REG_EXPAND_SZ,
                      reinterpret_cast<const BYTE*>(updated.c_str()),
                      out_bytes);
  RegCloseKey(key);
  if (rc != ERROR_SUCCESS) {
    log.warn(L"Failed updating System PATH during uninstall: " +
             format_win32_error(static_cast<DWORD>(rc)));
    return false;
  }
  broadcast_environment_change();
  log.info(L"Removed " + bin_dir + L" from System PATH.");
  return true;
}

bool create_start_menu_shortcut(const std::wstring& gui_exe_path, Logger& log) {
  std::wstring folder;
  if (!get_common_start_menu_dir(folder)) {
    log.warn(L"Could not resolve Common Programs directory.");
    return false;
  }
  if (!ensure_directory_tree(folder, log)) return false;

  const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool need_uninit = SUCCEEDED(co_hr);

  IShellLinkW* link = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW,
                                reinterpret_cast<void**>(&link));
  if (FAILED(hr) || link == nullptr) {
    if (need_uninit) CoUninitialize();
    log.warn(L"CoCreateInstance(CLSID_ShellLink) failed.");
    return false;
  }

  link->SetPath(gui_exe_path.c_str());
  const size_t slash = gui_exe_path.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    const std::wstring work_dir = gui_exe_path.substr(0u, slash);
    link->SetWorkingDirectory(work_dir.c_str());
  }
  link->SetDescription(L"WinLVM Volume Manager");

  IPersistFile* pf = nullptr;
  hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf));
  bool ok = false;
  if (SUCCEEDED(hr) && pf != nullptr) {
    const std::wstring lnk_path =
        join_path(folder, L"WinLVM Volume Manager.lnk");
    ok = SUCCEEDED(pf->Save(lnk_path.c_str(), TRUE));
    pf->Release();
    if (ok) log.info(L"Created Start Menu shortcut: " + lnk_path);
  }
  link->Release();
  if (need_uninit) CoUninitialize();
  if (!ok) log.warn(L"Failed to save Start Menu shortcut.");
  return ok;
}

void remove_start_menu_shortcut(Logger& log) {
  std::wstring folder;
  if (!get_common_start_menu_dir(folder)) return;
  const std::wstring lnk_path =
      join_path(folder, L"WinLVM Volume Manager.lnk");
  if (DeleteFileW(lnk_path.c_str())) {
    log.info(L"Removed Start Menu shortcut.");
  }
  RemoveDirectoryW(folder.c_str());
}

bool set_tray_autostart(const std::wstring& gui_exe_path,
                        bool enabled,
                        Logger& log) {
  HKEY key = nullptr;
  const LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRunRegSubkey, 0u,
                                KEY_SET_VALUE, &key);
  if (rc != ERROR_SUCCESS) {
    log.warn(L"Failed to open Run registry key: " +
             format_win32_error(static_cast<DWORD>(rc)));
    return false;
  }
  bool ok = true;
  if (enabled) {
    const std::wstring cmd = L"\"" + gui_exe_path + L"\" --tray";
    ok = write_reg_sz(key, kRunValueName, cmd);
    if (ok) log.info(L"Enabled WinLVM tray autostart.");
  } else {
    const LONG del_rc = RegDeleteValueW(key, kRunValueName);
    ok = (del_rc == ERROR_SUCCESS || del_rc == ERROR_FILE_NOT_FOUND);
  }
  RegCloseKey(key);
  return ok;
}

bool register_uninstall_entry(const std::wstring& install_dir,
                              const std::wstring& uninstaller_path,
                              const std::wstring& display_icon_path,
                              uint64_t estimated_kb,
                              Logger& log) {
  HKEY key = nullptr;
  const LONG rc = RegCreateKeyExW(
      HKEY_LOCAL_MACHINE, kUninstallRegSubkey, 0u, nullptr,
      REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (rc != ERROR_SUCCESS) {
    log.error(L"Failed to create Uninstall registry key: " +
              format_win32_error(static_cast<DWORD>(rc)));
    return false;
  }

  bool ok = true;
  ok &= write_reg_sz(key, L"DisplayName", kProductName);
  ok &= write_reg_sz(key, L"DisplayVersion", kProductVersion);
  ok &= write_reg_sz(key, L"Publisher", kPublisherName);
  ok &= write_reg_sz(key, L"InstallLocation", install_dir);
  ok &= write_reg_sz(key, L"DisplayIcon", display_icon_path);
  ok &= write_reg_sz(key, L"UninstallString", L"\"" + uninstaller_path + L"\"");
  ok &= write_reg_sz(key, L"QuietUninstallString",
                     L"\"" + uninstaller_path + L"\" /S");
  ok &= write_reg_dword(key, L"NoModify", 1u);
  ok &= write_reg_dword(key, L"NoRepair", 1u);
  ok &= write_reg_dword(
      key, L"EstimatedSize",
      static_cast<DWORD>(std::min<uint64_t>(estimated_kb, 0xffffffffULL)));
  RegCloseKey(key);
  if (ok) {
    log.info(L"Registered WinLVM in Apps & Features.");
  } else {
    log.error(L"Failed writing one or more Uninstall registry values.");
  }
  return ok;
}

bool read_installed_location(std::wstring& out_dir) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallRegSubkey, 0u,
                    KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  std::wstring raw;
  bool exists = false;
  const bool ok =
      query_reg_string_strict(key, L"InstallLocation", raw, exists, nullptr);
  RegCloseKey(key);
  if (!ok || !exists || raw.empty()) return false;
  out_dir = normalize_dir_path(raw);
  return !out_dir.empty();
}

void remove_uninstall_entry(Logger& log) {
  const LONG rc = RegDeleteKeyW(HKEY_LOCAL_MACHINE, kUninstallRegSubkey);
  if (rc == ERROR_SUCCESS) {
    log.info(L"Removed Apps & Features registry entry.");
  }
}

UINT get_window_dpi(HWND hwnd) {
  using FnGetDpiForWindow = UINT(WINAPI*)(HWND);
  auto fn = load_proc<FnGetDpiForWindow>(GetModuleHandleW(L"user32.dll"),
                                         "GetDpiForWindow");
  if (fn != nullptr && hwnd != nullptr) {
    const UINT dpi = fn(hwnd);
    if (dpi > 0u) return dpi;
  }
  HDC hdc = GetDC(hwnd);
  if (hdc != nullptr) {
    const int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(hwnd, hdc);
    if (dpi > 0) return static_cast<UINT>(dpi);
  }
  return 96u;
}

int scale_dpi(int value, UINT dpi) {
  return MulDiv(value, static_cast<int>(dpi), 96);
}

HFONT create_ui_font(UINT dpi, int point_size, int weight) {
  LOGFONTW lf{};
  lf.lfHeight = -MulDiv(point_size, static_cast<int>(dpi), 72);
  lf.lfWeight = weight;
  lf.lfQuality = CLEARTYPE_QUALITY;
  wcsncpy_s(lf.lfFaceName, L"Segoe UI", _TRUNCATE);
  return CreateFontIndirectW(&lf);
}

}  // namespace winlvm::installer
