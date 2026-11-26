#include "win_common.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace {

using namespace winlvm::installer;

struct UninstallOptions {
  bool silent = false;
  bool show_help = false;
  std::wstring install_dir;
  std::wstring log_file;
};

struct UninstallOutcome {
  int exit_code = kExitError;
  bool reboot_required = false;
  std::wstring summary;
};

constexpr UINT WM_APP_PROGRESS = WM_APP + 1u;
constexpr UINT WM_APP_LOG = WM_APP + 2u;
constexpr UINT WM_APP_DONE = WM_APP + 3u;

constexpr int kIdProgress = 201;
constexpr int kIdLogEdit = 202;
constexpr int kIdUninstallBtn = 203;
constexpr int kIdCloseBtn = 204;

std::wstring resolve_default_uninstall_dir() {
  std::wstring reg_dir;
  if (read_installed_location(reg_dir) && !reg_dir.empty()) {
    return reg_dir;
  }
  const std::wstring self = get_self_exe_path();
  const size_t slash = self.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    return normalize_dir_path(self.substr(0u, slash));
  }
  return get_default_install_dir();
}

bool parse_args(UninstallOptions& opts, std::wstring& err_out) {
  opts.install_dir = resolve_default_uninstall_dir();
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) return true;

  bool ok = true;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (_wcsicmp(arg.c_str(), L"/S") == 0 ||
        _wcsicmp(arg.c_str(), L"--silent") == 0) {
      opts.silent = true;
    } else if (_wcsicmp(arg.c_str(), L"--help") == 0 ||
               _wcsicmp(arg.c_str(), L"-h") == 0 ||
               _wcsicmp(arg.c_str(), L"/?") == 0) {
      opts.show_help = true;
    } else if (_wcsicmp(arg.c_str(), L"--install-dir") == 0) {
      if (i + 1 >= argc) {
        err_out = L"Missing value for --install-dir.";
        ok = false;
        break;
      }
      opts.install_dir = normalize_dir_path(argv[++i]);
    } else if (arg.rfind(L"--install-dir=", 0u) == 0u) {
      opts.install_dir = normalize_dir_path(arg.substr(14u));
    } else if (_wcsicmp(arg.c_str(), L"--log") == 0) {
      if (i + 1 >= argc) {
        err_out = L"Missing value for --log.";
        ok = false;
        break;
      }
      opts.log_file = argv[++i];
    } else if (arg.rfind(L"--log=", 0u) == 0u) {
      opts.log_file = arg.substr(6u);
    } else {
      err_out = L"Unknown argument: " + arg;
      ok = false;
      break;
    }
  }
  LocalFree(argv);
  return ok;
}

std::wstring usage_text() {
  return L"Usage: winlvm-uninstall.exe [options]\n"
         L"  /S, --silent            Run unattended uninstallation\n"
         L"  --install-dir <dir>     Override installed directory\n"
         L"  --log <file>            Write UTF-8 uninstall log to file\n";
}

bool is_safe_removal_root(const std::wstring& dir) {
  const std::wstring norm = normalize_dir_path(dir);
  if (norm.size() < 5u || norm[1] != L':' || norm[2] != L'\\') return false;

  std::vector<std::wstring> forbidden;
  wchar_t win_dir[MAX_PATH]{};
  const UINT wlen = GetWindowsDirectoryW(win_dir, MAX_PATH);
  if (wlen > 0u && wlen < MAX_PATH) {
    const std::wstring wnorm = normalize_dir_path(win_dir);
    if (_wcsicmp(norm.c_str(), wnorm.c_str()) == 0) return false;
    const std::wstring wprefix = wnorm + L"\\";
    if (norm.size() > wprefix.size() &&
        _wcsnicmp(norm.c_str(), wprefix.c_str(), wprefix.size()) == 0) {
      return false;
    }
  }

  const KNOWNFOLDERID kProtectedFolders[] = {
      FOLDERID_ProgramFiles,
      FOLDERID_ProgramFilesX86,
      FOLDERID_ProgramData,
      FOLDERID_UserProfiles,
  };
  for (const KNOWNFOLDERID& fid : kProtectedFolders) {
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(fid, 0u, nullptr, &known)) &&
        known != nullptr) {
      forbidden.push_back(normalize_dir_path(known));
      CoTaskMemFree(known);
    }
  }

  for (const auto& f : forbidden) {
    if (!f.empty() && _wcsicmp(norm.c_str(), f.c_str()) == 0) return false;
  }
  return true;
}

bool contains_unsafe_cmd_chars(const std::wstring& s) {
  for (wchar_t ch : s) {
    if (ch == L'"' || ch == L'&' || ch == L'|' || ch == L'<' || ch == L'>' ||
        ch == L'^' || ch == L'%' || ch < 0x20) {
      return true;
    }
  }
  return false;
}

void delete_file_or_schedule(const std::wstring& path,
                             bool& need_reboot,
                             Logger& log) {
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
  SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
  if (DeleteFileW(path.c_str())) return;
  if (MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
    need_reboot = true;
    log.warn(L"Scheduled locked file for deletion on reboot: " + path);
  }
}

void remove_directory_tree_except_self(const std::wstring& dir,
                                       const std::wstring& self_exe,
                                       bool& need_reboot,
                                       Logger& log) {
  const std::wstring pattern = join_path(dir, L"*");
  WIN32_FIND_DATAW fd{};
  HANDLE fh = FindFirstFileW(pattern.c_str(), &fd);
  if (fh == INVALID_HANDLE_VALUE) return;

  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
      continue;
    }
    const std::wstring full = join_path(dir, fd.cFileName);
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
      // Never recurse through junctions/symlinks during uninstall.
      RemoveDirectoryW(full.c_str());
      continue;
    }
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
      remove_directory_tree_except_self(full, self_exe, need_reboot, log);
      if (!RemoveDirectoryW(full.c_str())) {
        MoveFileExW(full.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
      }
    } else {
      if (_wcsicmp(full.c_str(), self_exe.c_str()) == 0) continue;
      delete_file_or_schedule(full, need_reboot, log);
    }
  } while (FindNextFileW(fh, &fd));
  FindClose(fh);
}

void schedule_self_cleanup(const std::wstring& self_exe,
                           const std::wstring& install_dir,
                           bool& need_reboot,
                           Logger& log) {
  bool scheduled_cmd = false;
  wchar_t sys_dir[MAX_PATH]{};
  const UINT slen = GetSystemDirectoryW(sys_dir, MAX_PATH);
  if (slen > 0u && slen < MAX_PATH && !contains_unsafe_cmd_chars(self_exe) &&
      !contains_unsafe_cmd_chars(install_dir)) {
    // Release any current-directory lock on install_dir before spawning cleanup.
    SetCurrentDirectoryW(sys_dir);
    const std::wstring cmd_exe = join_path(sys_dir, L"cmd.exe");
    std::wstring cmdline = L"\"" + cmd_exe +
                           L"\" /c ping -n 3 127.0.0.1 >nul & del /f /q \"" +
                           self_exe + L"\" & rmdir /q \"" + install_dir + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(cmd_exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, sys_dir,
                       &si, &pi)) {
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      scheduled_cmd = true;
      log.info(L"Scheduled self-removal of uninstaller and install folder.");
    }
  }

  if (!scheduled_cmd) {
    if (MoveFileExW(self_exe.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
      MoveFileExW(install_dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
      need_reboot = true;
      log.info(L"Scheduled uninstaller cleanup on next reboot.");
    }
  }
}

UninstallOutcome run_uninstallation(const UninstallOptions& opts,
                                    Logger& log,
                                    const std::function<void(int)>& on_progress,
                                    bool& should_self_delete) {
  UninstallOutcome outcome;
  should_self_delete = false;
  auto report = [&](int pct) {
    if (on_progress) on_progress(pct);
  };

  bool need_reboot = false;
  bool had_error = false;

  // Step 1: Close / terminate running winlvm_gui.exe.
  report(10);
  stop_gui_processes(log);

  // Step 2: Stop and delete winlvm-svc in SCM (up to 10s stop timeout).
  report(25);
  if (!delete_service_if_exists(kServiceName, 10000u, log)) {
    had_error = true;
  }

  // Step 3: Remove all ROOT\winlvm devnodes via SetupAPI (unloads winlvm.sys).
  report(45);
  if (!remove_bus_devnodes(need_reboot, log)) {
    had_error = true;
  }

  // Step 4: Scan %SystemRoot%\INF\oem*.inf and remove WinLVM driver packages.
  report(65);
  if (!remove_oem_driver_packages(need_reboot, log)) {
    had_error = true;
  }

  // Step 5: Remove PATH entry, Start Menu shortcut, Run and Uninstall keys.
  report(80);
  if (!opts.install_dir.empty()) {
    remove_dir_from_system_path(join_path(opts.install_dir, L"bin"), log);
  }
  remove_start_menu_shortcut(log);
  set_tray_autostart(L"", false, log);
  remove_uninstall_entry(log);

  // Step 6: Remove installed files under <InstallDir> (deferring self-exe).
  report(92);
  const std::wstring self_exe = get_self_exe_path();
  if (is_safe_removal_root(opts.install_dir) &&
      GetFileAttributesW(opts.install_dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
    log.info(L"Removing installed files from " + opts.install_dir + L"...");
    remove_directory_tree_except_self(opts.install_dir, self_exe, need_reboot,
                                      log);

    // Check if our own binary resides inside install_dir.
    const std::wstring prefix = opts.install_dir + L"\\";
    if (self_exe.size() > prefix.size() &&
        _wcsnicmp(self_exe.c_str(), prefix.c_str(), prefix.size()) == 0) {
      should_self_delete = true;
    } else {
      RemoveDirectoryW(opts.install_dir.c_str());
    }
  } else {
    log.warn(L"Skipped file deletion for non-standard directory: " +
             opts.install_dir);
  }

  report(100);
  outcome.reboot_required = need_reboot;
  if (had_error) {
    outcome.exit_code = kExitError;
    outcome.summary =
        L"WinLVM uninstallation completed with errors. Review the log for "
        L"details.";
  } else if (need_reboot) {
    outcome.exit_code = kExitRebootRequired;
    outcome.summary =
        L"WinLVM was removed. Restart Windows to complete driver cleanup.";
  } else {
    outcome.exit_code = kExitOk;
    outcome.summary = L"WinLVM was completely removed.";
  }
  log.info(outcome.summary);
  return outcome;
}

struct UninstallWindowState {
  UninstallOptions opts;
  Logger logger;
  bool running = false;
  bool finished = false;
  bool should_self_delete = false;
  bool reboot_required = false;
  int exit_code = kExitOk;
  UINT dpi = 96u;
  HFONT title_font = nullptr;
  HFONT body_font = nullptr;

  HWND title_lbl = nullptr;
  HWND desc_lbl = nullptr;
  HWND progress_bar = nullptr;
  HWND log_edit = nullptr;
  HWND uninstall_btn = nullptr;
  HWND close_btn = nullptr;
};

void append_log(HWND edit, const std::wstring& line) {
  if (edit == nullptr) return;
  const int len = GetWindowTextLengthW(edit);
  SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(len),
               static_cast<LPARAM>(len));
  const std::wstring with_nl = line + L"\r\n";
  SendMessageW(edit, EM_REPLACESEL, FALSE,
               reinterpret_cast<LPARAM>(with_nl.c_str()));
  SendMessageW(edit, EM_SCROLLCARET, 0u, 0);
}

void layout_uninstall_window(HWND hwnd, UninstallWindowState* st) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  const UINT d = st->dpi;
  const int pad = scale_dpi(16, d);
  const int gap = scale_dpi(8, d);
  const int inner_w = w - 2 * pad;

  int y = pad;
  MoveWindow(st->title_lbl, pad, y, inner_w, scale_dpi(26, d), TRUE);
  y += scale_dpi(30, d);
  MoveWindow(st->desc_lbl, pad, y, inner_w, scale_dpi(34, d), TRUE);
  y += scale_dpi(40, d);

  const int prog_h = scale_dpi(18, d);
  MoveWindow(st->progress_bar, pad, y, inner_w, prog_h, TRUE);
  y += prog_h + gap;

  const int btn_w = scale_dpi(96, d);
  const int btn_h = scale_dpi(28, d);
  const int btn_y = h - pad - btn_h;
  const int log_h = std::max(scale_dpi(96, d), btn_y - y - scale_dpi(12, d));
  MoveWindow(st->log_edit, pad, y, inner_w, log_h, TRUE);

  MoveWindow(st->close_btn, w - pad - btn_w, btn_y, btn_w, btn_h, TRUE);
  MoveWindow(st->uninstall_btn, w - pad - 2 * btn_w - gap, btn_y, btn_w, btn_h,
             TRUE);
}

void apply_fonts(UninstallWindowState* st) {
  HFONT old_title = st->title_font;
  HFONT old_body = st->body_font;
  st->title_font = create_ui_font(st->dpi, 12, FW_SEMIBOLD);
  st->body_font = create_ui_font(st->dpi, 9, FW_NORMAL);

  SendMessageW(st->title_lbl, WM_SETFONT,
               reinterpret_cast<WPARAM>(st->title_font), TRUE);
  HWND ctrls[] = {st->desc_lbl, st->log_edit, st->uninstall_btn, st->close_btn};
  for (HWND c : ctrls) {
    if (c != nullptr) {
      SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st->body_font),
                   TRUE);
    }
  }
  if (old_title != nullptr) DeleteObject(old_title);
  if (old_body != nullptr) DeleteObject(old_body);
}

struct UninstallWorkerArgs {
  HWND hwnd;
  UninstallWindowState* state;
};

struct DonePayload {
  UninstallOutcome outcome;
  bool should_self_delete = false;
};

DWORD WINAPI uninstall_worker_thread(LPVOID param) {
  auto* args = static_cast<UninstallWorkerArgs*>(param);
  HWND hwnd = args->hwnd;
  UninstallWindowState* st = args->state;
  delete args;

  st->logger.set_sink([hwnd](const std::wstring& line) {
    auto* copy = new std::wstring(line);
    if (!PostMessageW(hwnd, WM_APP_LOG, 0u, reinterpret_cast<LPARAM>(copy))) {
      delete copy;
    }
  });

  auto* payload = new DonePayload();
  payload->outcome = run_uninstallation(
      st->opts, st->logger,
      [hwnd](int pct) {
        PostMessageW(hwnd, WM_APP_PROGRESS, static_cast<WPARAM>(pct), 0);
      },
      payload->should_self_delete);
  if (!PostMessageW(hwnd, WM_APP_DONE, 0u,
                    reinterpret_cast<LPARAM>(payload))) {
    delete payload;
  }
  return 0u;
}

LRESULT CALLBACK uninstall_wnd_proc(HWND hwnd,
                                    UINT msg,
                                    WPARAM wparam,
                                    LPARAM lparam) {
  auto* st = reinterpret_cast<UninstallWindowState*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (msg) {
    case WM_CREATE: {
      const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lparam);
      st = static_cast<UninstallWindowState*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
      st->dpi = get_window_dpi(hwnd);

      auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                      DWORD ex_style, int id) -> HWND {
        return CreateWindowExW(
            ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            cs->hInstance, nullptr);
      };

      st->title_lbl = make(L"STATIC", L"Uninstall WinLVM", 0u, 0u, 0);
      const std::wstring desc =
          L"This will stop the WinLVM service, unload and remove the kernel "
          L"driver, and delete files in " +
          st->opts.install_dir + L".";
      st->desc_lbl = make(L"STATIC", desc.c_str(), 0u, 0u, 0);
      st->progress_bar =
          make(PROGRESS_CLASSW, L"", PBS_SMOOTH, 0u, kIdProgress);
      SendMessageW(st->progress_bar, PBM_SETRANGE32, 0u, 100);
      st->log_edit =
          make(L"EDIT", L"",
               ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
               WS_EX_CLIENTEDGE, kIdLogEdit);
      st->uninstall_btn =
          make(L"BUTTON", L"Uninstall", WS_TABSTOP | BS_DEFPUSHBUTTON, 0u,
               kIdUninstallBtn);
      st->close_btn =
          make(L"BUTTON", L"Close", WS_TABSTOP | BS_PUSHBUTTON, 0u,
               kIdCloseBtn);

      apply_fonts(st);
      layout_uninstall_window(hwnd, st);
      append_log(st->log_edit, L"Click Uninstall to remove WinLVM.");
      return 0;
    }

    case WM_SIZE:
      if (st != nullptr) layout_uninstall_window(hwnd, st);
      return 0;

    case WM_DPICHANGED:
      if (st != nullptr) {
        st->dpi = HIWORD(wparam);
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        apply_fonts(st);
        layout_uninstall_window(hwnd, st);
      }
      return 0;

    case WM_COMMAND: {
      if (st == nullptr) break;
      const int id = LOWORD(wparam);
      if (id == kIdUninstallBtn && !st->running && !st->finished) {
        const std::wstring prompt =
            L"Remove WinLVM, its kernel driver, service, and installed files "
            L"from:\n\n" +
            st->opts.install_dir + L"?";
        if (MessageBoxW(hwnd, prompt.c_str(), L"Uninstall WinLVM",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
          return 0;
        }
        st->running = true;
        EnableWindow(st->uninstall_btn, FALSE);
        EnableWindow(st->close_btn, FALSE);
        SetWindowTextW(st->log_edit, L"");

        auto* args = new UninstallWorkerArgs{hwnd, st};
        HANDLE thread = CreateThread(nullptr, 0u, uninstall_worker_thread, args,
                                     0u, nullptr);
        if (thread != nullptr) {
          CloseHandle(thread);
        } else {
          delete args;
          st->running = false;
          EnableWindow(st->uninstall_btn, TRUE);
          EnableWindow(st->close_btn, TRUE);
          append_log(st->log_edit,
                     L"[ERROR] Failed to start uninstaller thread.");
        }
      } else if (id == kIdCloseBtn && !st->running) {
        DestroyWindow(hwnd);
      }
      return 0;
    }

    case WM_APP_PROGRESS:
      if (st != nullptr) SendMessageW(st->progress_bar, PBM_SETPOS, wparam, 0);
      return 0;

    case WM_APP_LOG: {
      auto* line = reinterpret_cast<std::wstring*>(lparam);
      if (st != nullptr && line != nullptr) append_log(st->log_edit, *line);
      delete line;
      return 0;
    }

    case WM_APP_DONE: {
      auto* payload = reinterpret_cast<DonePayload*>(lparam);
      if (st != nullptr && payload != nullptr) {
        st->running = false;
        st->finished = true;
        st->exit_code = payload->outcome.exit_code;
        st->reboot_required = payload->outcome.reboot_required;
        st->should_self_delete = payload->should_self_delete;
        SetWindowTextW(st->uninstall_btn, L"Removed");
        EnableWindow(st->close_btn, TRUE);
        const UINT icon =
            (payload->outcome.exit_code == kExitError) ? MB_ICONWARNING
                                                       : MB_ICONINFORMATION;
        MessageBoxW(hwnd, payload->outcome.summary.c_str(), L"Uninstall WinLVM",
                    MB_OK | icon);
      }
      delete payload;
      return 0;
    }

    case WM_CLOSE:
      if (st != nullptr && st->running) return 0;
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      if (st != nullptr) {
        if (st->title_font != nullptr) DeleteObject(st->title_font);
        if (st->body_font != nullptr) DeleteObject(st->body_font);
      }
      PostQuitMessage(st != nullptr ? st->exit_code : 0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance,
                    HINSTANCE,
                    LPWSTR,
                    int nCmdShow) {
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

  UninstallOptions opts;
  std::wstring arg_err;
  if (!parse_args(opts, arg_err)) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
      FILE* err_stream = nullptr;
      freopen_s(&err_stream, "CONOUT$", "w", stderr);
      std::fwprintf(stderr, L"%ls\n%ls", arg_err.c_str(), usage_text().c_str());
    } else {
      MessageBoxW(nullptr, (arg_err + L"\n\n" + usage_text()).c_str(),
                  L"Uninstall WinLVM", MB_OK | MB_ICONERROR);
    }
    return kExitError;
  }

  if (opts.show_help) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
      FILE* out_stream = nullptr;
      freopen_s(&out_stream, "CONOUT$", "w", stdout);
      std::fwprintf(stdout, L"%ls", usage_text().c_str());
    } else {
      MessageBoxW(nullptr, usage_text().c_str(), L"Uninstall WinLVM",
                  MB_OK | MB_ICONINFORMATION);
    }
    return kExitOk;
  }

  if (opts.silent) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
      FILE* err_stream = nullptr;
      freopen_s(&err_stream, "CONOUT$", "w", stderr);
    }
    Logger log;
    if (!opts.log_file.empty()) log.set_file(opts.log_file);
    log.set_stderr_echo(true);
    bool self_del = false;
    UninstallOutcome outcome = run_uninstallation(opts, log, {}, self_del);
    if (self_del) {
      schedule_self_cleanup(get_self_exe_path(), opts.install_dir,
                            outcome.reboot_required, log);
      if (outcome.exit_code == kExitOk && outcome.reboot_required) {
        outcome.exit_code = kExitRebootRequired;
      }
    }
    return outcome.exit_code;
  }

  INITCOMMONCONTROLSEX icc{};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
  InitCommonControlsEx(&icc);

  UninstallWindowState state;
  state.opts = opts;
  if (!opts.log_file.empty()) state.logger.set_file(opts.log_file);

  const wchar_t kWndClass[] = L"WinLvmUninstallWindow";
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = uninstall_wnd_proc;
  wc.hInstance = hInstance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = kWndClass;
  if (!RegisterClassExW(&wc)) return kExitError;

  const UINT init_dpi = get_window_dpi(nullptr);
  const int win_w = scale_dpi(500, init_dpi);
  const int win_h = scale_dpi(340, init_dpi);
  const int screen_w = GetSystemMetrics(SM_CXSCREEN);
  const int screen_h = GetSystemMetrics(SM_CYSCREEN);
  const int x = (screen_w - win_w) / 2;
  const int y = (screen_h - win_h) / 2;

  HWND hwnd = CreateWindowExW(
      0u, kWndClass, L"Uninstall WinLVM",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, x, y, win_w,
      win_h, nullptr, nullptr, hInstance, &state);
  if (hwnd == nullptr) return kExitError;

  ShowWindow(hwnd, nCmdShow);
  UpdateWindow(hwnd);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0u, 0u) > 0) {
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  if (state.should_self_delete) {
    schedule_self_cleanup(get_self_exe_path(), state.opts.install_dir,
                          state.reboot_required, state.logger);
    if (state.exit_code == kExitOk && state.reboot_required) {
      state.exit_code = kExitRebootRequired;
    }
  }
  return state.exit_code;
}
