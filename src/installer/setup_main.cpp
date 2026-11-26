#include "archive_format.h"
#include "win_common.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace {

using namespace winlvm::installer;

struct InstallOptions {
  bool silent = false;
  bool verify_only = false;
  bool show_help = false;
  bool trust_test_cert = false;
  bool trust_test_cert_explicit = false;
  bool tray_autostart = true;
  std::wstring install_dir;
  std::wstring log_file;
};

struct InstallOutcome {
  int exit_code = kExitError;
  bool reboot_required = false;
  std::wstring summary;
};

constexpr UINT WM_APP_PROGRESS = WM_APP + 1u;
constexpr UINT WM_APP_LOG = WM_APP + 2u;
constexpr UINT WM_APP_DONE = WM_APP + 3u;

constexpr int kIdDirEdit = 101;
constexpr int kIdBrowseBtn = 102;
constexpr int kIdCompDriver = 103;
constexpr int kIdCompService = 104;
constexpr int kIdCompGui = 105;
constexpr int kIdCompCli = 106;
constexpr int kIdAutostartChk = 107;
constexpr int kIdTrustCertChk = 108;
constexpr int kIdProgress = 109;
constexpr int kIdLogEdit = 110;
constexpr int kIdInstallBtn = 111;
constexpr int kIdCloseBtn = 112;

bool parse_args(InstallOptions& opts, std::wstring& err_out) {
  opts.install_dir = get_default_install_dir();
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) return true;

  bool ok = true;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (_wcsicmp(arg.c_str(), L"/S") == 0 ||
        _wcsicmp(arg.c_str(), L"--silent") == 0) {
      opts.silent = true;
    } else if (_wcsicmp(arg.c_str(), L"--trust-test-cert") == 0) {
      opts.trust_test_cert = true;
      opts.trust_test_cert_explicit = true;
    } else if (_wcsicmp(arg.c_str(), L"--no-tray-autostart") == 0) {
      opts.tray_autostart = false;
    } else if (_wcsicmp(arg.c_str(), L"--verify-only") == 0) {
      opts.verify_only = true;
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
  return L"Usage: winlvm-setup-x64.exe [options]\n"
         L"  /S, --silent            Run unattended installation\n"
         L"  --install-dir <dir>     Target directory (default: C:\\Program Files\\WinLVM)\n"
         L"  --trust-test-cert       Import bundled test certificate to machine stores\n"
         L"  --no-tray-autostart     Do not register tray autostart at sign-in\n"
         L"  --log <file>            Write UTF-8 install log to file\n"
         L"  --verify-only           Verify embedded archive integrity and exit\n";
}

bool is_safe_install_dir(const std::wstring& dir) {
  const std::wstring norm = normalize_dir_path(dir);
  if (norm.size() < 4u || norm[1] != L':' || norm[2] != L'\\') return false;
  return true;
}

InstallOutcome run_installation(const InstallOptions& opts,
                                HWND ui_hwnd,
                                Logger& log,
                                const std::function<void(int)>& on_progress) {
  InstallOutcome outcome;
  auto report_progress = [&](int pct) {
    if (on_progress) on_progress(pct);
  };

  if (!is_safe_install_dir(opts.install_dir)) {
    log.error(L"Invalid installation directory: " + opts.install_dir);
    outcome.summary = log.last_error();
    return outcome;
  }

  const std::wstring self_path = get_self_exe_path();
  if (self_path.empty()) {
    log.error(L"Failed to resolve installer executable path.");
    outcome.summary = log.last_error();
    return outcome;
  }

  // Step 1: Verify payload trailer SHA-256 and per-entry CRC-32/SHA-256 before
  // touching system state.
  report_progress(5);
  log.info(L"Verifying embedded installer payload integrity...");
  ParsedArchive archive;
  std::string archive_err;
  if (!verify_archive_file_full(std::filesystem::path(self_path), archive,
                                archive_err)) {
    log.error(L"Installer payload integrity check failed: " +
              utf8_to_wide(archive_err));
    outcome.summary = log.last_error();
    return outcome;
  }
  log.info(L"Payload verified (" + std::to_wstring(archive.entries.size()) +
           L" files, SHA-256 " +
           utf8_to_wide(sha256_hex(archive.footer.archive_sha256)) + L").");

  if (opts.verify_only) {
    report_progress(100);
    outcome.exit_code = kExitOk;
    outcome.summary = L"Archive verification succeeded.";
    return outcome;
  }

  // Step 2: Check test-signing policy and import bundled certificate if needed.
  report_progress(15);
  const ArchiveEntryMeta* cert_entry = archive.find_entry("driver/winlvm.cer");
  const bool testsign_on = is_testsigning_enabled();
  if (cert_entry != nullptr) {
    if (!testsign_on) {
      log.warn(
          L"Windows test-signing mode is not enabled. Non-WHQL test-signed "
          L"driver builds require running 'bcdedit /set testsigning on' as "
          L"Administrator and rebooting before the kernel driver can load.");
    }
    if (opts.trust_test_cert) {
      std::vector<uint8_t> cert_bytes;
      if (!read_entry_from_file(std::filesystem::path(self_path), *cert_entry,
                                cert_bytes, archive_err)) {
        log.error(L"Failed reading bundled certificate: " +
                  utf8_to_wide(archive_err));
        outcome.summary = log.last_error();
        return outcome;
      }
      if (!import_certificate_to_machine_stores(cert_bytes.data(),
                                                cert_bytes.size(), log)) {
        outcome.summary = log.last_error();
        return outcome;
      }
    } else if (testsign_on) {
      log.info(L"Test-signing is enabled, but certificate trust was not "
               L"selected; driver installation relies on an already-trusted "
               L"signer.");
    }
  }

  // Step 3: Stop existing GUI and control service cleanly for in-place upgrade.
  report_progress(25);
  stop_gui_processes(log);
  if (!stop_service_if_running(kServiceName, 10000u, log)) {
    outcome.summary = log.last_error();
    return outcome;
  }

  // Step 4: Extract all verified files atomically to <InstallDir>.
  report_progress(35);
  log.info(L"Extracting files to " + opts.install_dir + L"...");
  if (!ensure_directory_tree(opts.install_dir, log)) {
    outcome.summary = log.last_error();
    return outcome;
  }

  for (size_t i = 0u; i < archive.entries.size(); ++i) {
    const auto& entry = archive.entries[i];
    std::vector<uint8_t> data;
    if (!read_entry_from_file(std::filesystem::path(self_path), entry, data,
                              archive_err)) {
      log.error(L"Failed extracting " + utf8_to_wide(entry.path) + L": " +
                utf8_to_wide(archive_err));
      outcome.summary = log.last_error();
      return outcome;
    }
    const std::wstring dest =
        join_path(opts.install_dir, utf8_to_wide(entry.path));
    if (!write_file_atomic(dest, data.data(), data.size(), log)) {
      outcome.summary = log.last_error();
      return outcome;
    }
    const int pct =
        35 + static_cast<int>(((i + 1u) * 25u) / archive.entries.size());
    report_progress(pct);
  }
  log.info(L"Extracted " + std::to_wstring(archive.entries.size()) +
           L" files.");

  bool need_reboot = false;

  // Step 5: Stage winlvm-vol.inf into Driver Store via DiInstallDriverW.
  report_progress(65);
  const std::wstring vol_inf =
      join_path(opts.install_dir, L"driver\\vol\\winlvm-vol.inf");
  if (GetFileAttributesW(vol_inf.c_str()) != INVALID_FILE_ATTRIBUTES) {
    if (!stage_volume_driver(opts.silent ? nullptr : ui_hwnd, vol_inf,
                             need_reboot, log)) {
      if (cert_entry != nullptr && !testsign_on) {
        log.error(
            L"Driver staging failed because Windows test-signing mode is off. "
            L"Run 'bcdedit /set testsigning on', reboot, and re-run setup.");
      }
      outcome.summary = log.last_error();
      return outcome;
    }
  } else {
    log.error(L"Required volume driver INF missing from payload: " + vol_inf);
    outcome.summary = log.last_error();
    return outcome;
  }

  // Step 6: Create/update ROOT\winlvm bus devnode via SetupAPI + NewDev.
  report_progress(75);
  const std::wstring bus_inf =
      join_path(opts.install_dir, L"driver\\winlvm.inf");
  if (GetFileAttributesW(bus_inf.c_str()) != INVALID_FILE_ATTRIBUTES) {
    if (!install_or_update_bus_driver(opts.silent ? nullptr : ui_hwnd, bus_inf,
                                      opts.silent, need_reboot, log)) {
      if (cert_entry != nullptr && !testsign_on) {
        log.error(
            L"Bus driver installation failed because Windows test-signing mode "
            L"is off. Run 'bcdedit /set testsigning on', reboot, and re-run "
            L"setup.");
      }
      outcome.summary = log.last_error();
      return outcome;
    }
  } else {
    log.error(L"Required bus driver INF missing from payload: " + bus_inf);
    outcome.summary = log.last_error();
    return outcome;
  }

  // Step 7: Register and start winlvm-svc SCM service.
  report_progress(85);
  const std::wstring svc_exe =
      join_path(opts.install_dir, L"bin\\winlvm_svc.exe");
  if (GetFileAttributesW(svc_exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
    if (!install_and_start_service(svc_exe, log)) {
      outcome.summary = log.last_error();
      return outcome;
    }
  } else {
    log.error(L"Required service binary missing from payload: " + svc_exe);
    outcome.summary = log.last_error();
    return outcome;
  }

  // Step 8: Add <InstallDir>\bin to System PATH and broadcast WM_SETTINGCHANGE.
  report_progress(92);
  const std::wstring bin_dir = join_path(opts.install_dir, L"bin");
  if (!add_dir_to_system_path(bin_dir, log)) {
    outcome.summary = log.last_error();
    return outcome;
  }

  // Step 9: Start Menu shortcut, optional tray autostart, Uninstall registry.
  report_progress(96);
  const std::wstring gui_exe =
      join_path(opts.install_dir, L"bin\\winlvm_gui.exe");
  const bool has_gui =
      (GetFileAttributesW(gui_exe.c_str()) != INVALID_FILE_ATTRIBUTES);
  if (has_gui) {
    create_start_menu_shortcut(gui_exe, log);
    set_tray_autostart(gui_exe, opts.tray_autostart, log);
  } else {
    set_tray_autostart(L"", false, log);
  }

  const std::wstring uninstall_exe =
      join_path(opts.install_dir, L"winlvm-uninstall.exe");
  const std::wstring icon_path = has_gui ? gui_exe : uninstall_exe;
  const uint64_t estimated_kb =
      (archive.total_unpacked_bytes + 1023ULL) / 1024ULL;
  if (!register_uninstall_entry(opts.install_dir, uninstall_exe, icon_path,
                                estimated_kb, log)) {
    outcome.summary = log.last_error();
    return outcome;
  }

  report_progress(100);
  outcome.reboot_required = need_reboot;
  outcome.exit_code = need_reboot ? kExitRebootRequired : kExitOk;
  outcome.summary =
      need_reboot
          ? L"WinLVM installation completed. Restart Windows to finish driver "
            L"setup."
          : L"WinLVM installation completed successfully.";
  log.info(outcome.summary);
  return outcome;
}

struct SetupWindowState {
  InstallOptions opts;
  Logger logger;
  bool has_test_cert = false;
  bool has_gui = false;
  bool testsign_enabled = false;
  bool running = false;
  bool finished = false;
  int exit_code = kExitOk;
  UINT dpi = 96u;
  HFONT title_font = nullptr;
  HFONT body_font = nullptr;

  HWND title_lbl = nullptr;
  HWND dir_lbl = nullptr;
  HWND dir_edit = nullptr;
  HWND browse_btn = nullptr;
  HWND group_box = nullptr;
  HWND comp_driver = nullptr;
  HWND comp_service = nullptr;
  HWND comp_gui = nullptr;
  HWND comp_cli = nullptr;
  HWND autostart_chk = nullptr;
  HWND trust_cert_chk = nullptr;
  HWND progress_bar = nullptr;
  HWND log_edit = nullptr;
  HWND install_btn = nullptr;
  HWND close_btn = nullptr;
};

void append_log_text(HWND edit, const std::wstring& line) {
  if (edit == nullptr) return;
  const int len = GetWindowTextLengthW(edit);
  SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(len),
               static_cast<LPARAM>(len));
  const std::wstring with_nl = line + L"\r\n";
  SendMessageW(edit, EM_REPLACESEL, FALSE,
               reinterpret_cast<LPARAM>(with_nl.c_str()));
  SendMessageW(edit, EM_SCROLLCARET, 0u, 0);
}

void layout_setup_window(HWND hwnd, SetupWindowState* st) {
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
  y += scale_dpi(32, d);

  MoveWindow(st->dir_lbl, pad, y, inner_w, scale_dpi(18, d), TRUE);
  y += scale_dpi(20, d);

  const int browse_w = scale_dpi(88, d);
  const int edit_h = scale_dpi(24, d);
  MoveWindow(st->dir_edit, pad, y, inner_w - browse_w - gap, edit_h, TRUE);
  MoveWindow(st->browse_btn, pad + inner_w - browse_w, y, browse_w, edit_h,
             TRUE);
  y += edit_h + scale_dpi(12, d);

  const int chk_h = scale_dpi(20, d);
  const int rows = st->has_test_cert ? 6 : 5;
  const int group_h = scale_dpi(26, d) + rows * (chk_h + scale_dpi(4, d)) +
                      scale_dpi(6, d);
  MoveWindow(st->group_box, pad, y, inner_w, group_h, TRUE);

  int cy = y + scale_dpi(22, d);
  const int cx = pad + scale_dpi(12, d);
  const int cw = inner_w - scale_dpi(24, d);
  MoveWindow(st->comp_driver, cx, cy, cw, chk_h, TRUE);
  cy += chk_h + scale_dpi(4, d);
  MoveWindow(st->comp_service, cx, cy, cw, chk_h, TRUE);
  cy += chk_h + scale_dpi(4, d);
  MoveWindow(st->comp_gui, cx, cy, cw, chk_h, TRUE);
  cy += chk_h + scale_dpi(4, d);
  MoveWindow(st->comp_cli, cx, cy, cw, chk_h, TRUE);
  cy += chk_h + scale_dpi(4, d);
  MoveWindow(st->autostart_chk, cx, cy, cw, chk_h, TRUE);
  cy += chk_h + scale_dpi(4, d);
  if (st->trust_cert_chk != nullptr) {
    MoveWindow(st->trust_cert_chk, cx, cy, cw, chk_h, TRUE);
    ShowWindow(st->trust_cert_chk, st->has_test_cert ? SW_SHOW : SW_HIDE);
  }
  y += group_h + scale_dpi(12, d);

  const int prog_h = scale_dpi(18, d);
  MoveWindow(st->progress_bar, pad, y, inner_w, prog_h, TRUE);
  y += prog_h + gap;

  const int btn_w = scale_dpi(96, d);
  const int btn_h = scale_dpi(28, d);
  const int btn_y = h - pad - btn_h;
  const int log_h = std::max(scale_dpi(72, d), btn_y - y - scale_dpi(12, d));
  MoveWindow(st->log_edit, pad, y, inner_w, log_h, TRUE);

  MoveWindow(st->close_btn, w - pad - btn_w, btn_y, btn_w, btn_h, TRUE);
  MoveWindow(st->install_btn, w - pad - 2 * btn_w - gap, btn_y, btn_w, btn_h,
             TRUE);
}

void apply_fonts(SetupWindowState* st) {
  HFONT old_title = st->title_font;
  HFONT old_body = st->body_font;
  st->title_font = create_ui_font(st->dpi, 12, FW_SEMIBOLD);
  st->body_font = create_ui_font(st->dpi, 9, FW_NORMAL);

  SendMessageW(st->title_lbl, WM_SETFONT,
               reinterpret_cast<WPARAM>(st->title_font), TRUE);
  HWND controls[] = {st->dir_lbl,      st->dir_edit,      st->browse_btn,
                     st->group_box,    st->comp_driver,   st->comp_service,
                     st->comp_gui,     st->comp_cli,      st->autostart_chk,
                     st->trust_cert_chk, st->log_edit,    st->install_btn,
                     st->close_btn};
  for (HWND c : controls) {
    if (c != nullptr) {
      SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st->body_font),
                   TRUE);
    }
  }
  if (old_title != nullptr) DeleteObject(old_title);
  if (old_body != nullptr) DeleteObject(old_body);
}

std::wstring get_window_text_dynamic(HWND ctrl) {
  const int len = GetWindowTextLengthW(ctrl);
  if (len <= 0) return {};
  std::wstring buf(static_cast<size_t>(len) + 1u, L'\0');
  const int copied = GetWindowTextW(ctrl, buf.data(), len + 1);
  if (copied <= 0) return {};
  buf.resize(static_cast<size_t>(copied));
  return buf;
}

void set_form_controls_enabled(SetupWindowState* st, bool enabled) {
  const BOOL b = enabled ? TRUE : FALSE;
  EnableWindow(st->dir_edit, b);
  EnableWindow(st->browse_btn, b);
  EnableWindow(st->autostart_chk, (enabled && st->has_gui) ? TRUE : FALSE);
  if (st->trust_cert_chk != nullptr) {
    EnableWindow(st->trust_cert_chk,
                 (enabled && st->has_test_cert) ? TRUE : FALSE);
  }
  EnableWindow(st->install_btn, b);
}

struct WorkerArgs {
  HWND hwnd;
  SetupWindowState* state;
  InstallOptions opts;
};

DWORD WINAPI install_worker_thread(LPVOID param) {
  auto* args = static_cast<WorkerArgs*>(param);
  HWND hwnd = args->hwnd;
  SetupWindowState* st = args->state;
  const InstallOptions opts = args->opts;
  delete args;

  st->logger.set_sink([hwnd](const std::wstring& line) {
    auto* copy = new std::wstring(line);
    if (!PostMessageW(hwnd, WM_APP_LOG, 0u, reinterpret_cast<LPARAM>(copy))) {
      delete copy;
    }
  });

  auto* outcome = new InstallOutcome(run_installation(
      opts, hwnd, st->logger, [hwnd](int pct) {
        PostMessageW(hwnd, WM_APP_PROGRESS, static_cast<WPARAM>(pct), 0);
      }));
  if (!PostMessageW(hwnd, WM_APP_DONE, 0u, reinterpret_cast<LPARAM>(outcome))) {
    delete outcome;
  }
  return 0u;
}

std::wstring browse_for_folder(HWND owner, const std::wstring& current) {
  BROWSEINFOW bi{};
  bi.hwndOwner = owner;
  bi.lpszTitle = L"Select WinLVM installation folder";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
  if (pidl == nullptr) return current;
  wchar_t path[MAX_PATH]{};
  std::wstring out = current;
  if (SHGetPathFromIDListW(pidl, path)) {
    out = normalize_dir_path(path);
  }
  CoTaskMemFree(pidl);
  return out;
}

LRESULT CALLBACK setup_wnd_proc(HWND hwnd,
                                UINT msg,
                                WPARAM wparam,
                                LPARAM lparam) {
  auto* st =
      reinterpret_cast<SetupWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (msg) {
    case WM_CREATE: {
      const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lparam);
      st = static_cast<SetupWindowState*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
      st->dpi = get_window_dpi(hwnd);

      auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                      DWORD ex_style, int id) -> HWND {
        return CreateWindowExW(
            ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            cs->hInstance, nullptr);
      };

      st->title_lbl = make(L"STATIC", L"WinLVM Setup", 0u, 0u, 0);
      st->dir_lbl = make(L"STATIC", L"Install location:", 0u, 0u, 0);
      st->dir_edit = make(L"EDIT", st->opts.install_dir.c_str(),
                          WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
                          kIdDirEdit);
      st->browse_btn =
          make(L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, 0u,
               kIdBrowseBtn);

      st->group_box = make(L"BUTTON", L"Components", BS_GROUPBOX, 0u, 0);
      st->comp_driver = make(L"BUTTON", L"WDM Kernel Driver (winlvm.sys)",
                             BS_AUTOCHECKBOX | WS_DISABLED, 0u, kIdCompDriver);
      st->comp_service = make(L"BUTTON", L"Control Service (winlvm-svc)",
                              BS_AUTOCHECKBOX | WS_DISABLED, 0u, kIdCompService);
      st->comp_gui =
          make(L"BUTTON", L"Volume Manager Desktop && Tray (winlvm_gui.exe)",
               BS_AUTOCHECKBOX | WS_DISABLED, 0u, kIdCompGui);
      st->comp_cli =
          make(L"BUTTON", L"CLI Tools on System PATH (lvm.exe, lvmdump.exe)",
               BS_AUTOCHECKBOX | WS_DISABLED, 0u, kIdCompCli);
      st->autostart_chk =
          make(L"BUTTON", L"Start Volume Manager in system tray at sign-in",
               WS_TABSTOP | BS_AUTOCHECKBOX, 0u, kIdAutostartChk);
      st->trust_cert_chk =
          make(L"BUTTON", L"Trust bundled test-signing certificate",
               WS_TABSTOP | BS_AUTOCHECKBOX, 0u, kIdTrustCertChk);

      SendMessageW(st->comp_driver, BM_SETCHECK, BST_CHECKED, 0);
      SendMessageW(st->comp_service, BM_SETCHECK, BST_CHECKED, 0);
      SendMessageW(st->comp_gui, BM_SETCHECK,
                   st->has_gui ? BST_CHECKED : BST_UNCHECKED, 0);
      SendMessageW(st->comp_cli, BM_SETCHECK, BST_CHECKED, 0);
      SendMessageW(
          st->autostart_chk, BM_SETCHECK,
          (st->has_gui && st->opts.tray_autostart) ? BST_CHECKED : BST_UNCHECKED,
          0);
      if (!st->has_gui) EnableWindow(st->autostart_chk, FALSE);
      if (st->has_test_cert) {
        const bool precheck =
            st->opts.trust_test_cert_explicit ? st->opts.trust_test_cert
                                              : st->testsign_enabled;
        SendMessageW(st->trust_cert_chk, BM_SETCHECK,
                     precheck ? BST_CHECKED : BST_UNCHECKED, 0);
      }

      st->progress_bar =
          make(PROGRESS_CLASSW, L"", PBS_SMOOTH, 0u, kIdProgress);
      SendMessageW(st->progress_bar, PBM_SETRANGE32, 0u, 100);
      SendMessageW(st->progress_bar, PBM_SETPOS, 0u, 0);

      st->log_edit =
          make(L"EDIT", L"",
               ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
               WS_EX_CLIENTEDGE, kIdLogEdit);

      st->install_btn =
          make(L"BUTTON", L"Install", WS_TABSTOP | BS_DEFPUSHBUTTON, 0u,
               kIdInstallBtn);
      st->close_btn =
          make(L"BUTTON", L"Close", WS_TABSTOP | BS_PUSHBUTTON, 0u,
               kIdCloseBtn);

      apply_fonts(st);
      layout_setup_window(hwnd, st);

      if (st->has_test_cert && !st->testsign_enabled) {
        append_log_text(
            st->log_edit,
            L"[WARN] Test-signing is disabled on this machine. Run "
            L"'bcdedit /set testsigning on' as Administrator and reboot to "
            L"load test-signed driver builds.");
      } else {
        append_log_text(st->log_edit,
                        L"Ready to install WinLVM " +
                            std::wstring(kProductVersion) + L".");
      }
      return 0;
    }

    case WM_SIZE:
      if (st != nullptr) layout_setup_window(hwnd, st);
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
        layout_setup_window(hwnd, st);
      }
      return 0;

    case WM_COMMAND: {
      if (st == nullptr) break;
      const int id = LOWORD(wparam);
      if (id == kIdBrowseBtn && !st->running) {
        const std::wstring current = get_window_text_dynamic(st->dir_edit);
        const std::wstring chosen = browse_for_folder(hwnd, current);
        SetWindowTextW(st->dir_edit, chosen.c_str());
      } else if (id == kIdInstallBtn && !st->running && !st->finished) {
        InstallOptions run_opts = st->opts;
        run_opts.install_dir =
            normalize_dir_path(get_window_text_dynamic(st->dir_edit));
        run_opts.tray_autostart =
            (SendMessageW(st->autostart_chk, BM_GETCHECK, 0u, 0) ==
             BST_CHECKED);
        run_opts.trust_test_cert =
            st->has_test_cert &&
            (SendMessageW(st->trust_cert_chk, BM_GETCHECK, 0u, 0) ==
             BST_CHECKED);

        st->running = true;
        set_form_controls_enabled(st, false);
        EnableWindow(st->close_btn, FALSE);
        SetWindowTextW(st->log_edit, L"");

        auto* args = new WorkerArgs{hwnd, st, run_opts};
        HANDLE thread =
            CreateThread(nullptr, 0u, install_worker_thread, args, 0u, nullptr);
        if (thread != nullptr) {
          CloseHandle(thread);
        } else {
          delete args;
          st->running = false;
          set_form_controls_enabled(st, true);
          EnableWindow(st->close_btn, TRUE);
          append_log_text(st->log_edit,
                          L"[ERROR] Failed to start installer thread.");
        }
      } else if (id == kIdCloseBtn && !st->running) {
        DestroyWindow(hwnd);
      }
      return 0;
    }

    case WM_APP_PROGRESS:
      if (st != nullptr) {
        SendMessageW(st->progress_bar, PBM_SETPOS, wparam, 0);
      }
      return 0;

    case WM_APP_LOG: {
      auto* line = reinterpret_cast<std::wstring*>(lparam);
      if (st != nullptr && line != nullptr) {
        append_log_text(st->log_edit, *line);
      }
      delete line;
      return 0;
    }

    case WM_APP_DONE: {
      auto* outcome = reinterpret_cast<InstallOutcome*>(lparam);
      if (st != nullptr && outcome != nullptr) {
        st->running = false;
        st->exit_code = outcome->exit_code;
        if (outcome->exit_code == kExitOk ||
            outcome->exit_code == kExitRebootRequired) {
          st->finished = true;
          SetWindowTextW(st->install_btn, L"Installed");
        } else {
          set_form_controls_enabled(st, true);
        }
        EnableWindow(st->close_btn, TRUE);
        if (outcome->exit_code != kExitOk &&
            outcome->exit_code != kExitRebootRequired) {
          MessageBoxW(hwnd, outcome->summary.c_str(), L"WinLVM Setup",
                      MB_OK | MB_ICONERROR);
        } else if (outcome->reboot_required) {
          MessageBoxW(hwnd, outcome->summary.c_str(), L"WinLVM Setup",
                      MB_OK | MB_ICONINFORMATION);
        }
      }
      delete outcome;
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

  InstallOptions opts;
  std::wstring arg_err;
  if (!parse_args(opts, arg_err)) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
      FILE* err_stream = nullptr;
      freopen_s(&err_stream, "CONOUT$", "w", stderr);
      std::fwprintf(stderr, L"%ls\n%ls", arg_err.c_str(), usage_text().c_str());
    } else {
      MessageBoxW(nullptr, (arg_err + L"\n\n" + usage_text()).c_str(),
                  L"WinLVM Setup", MB_OK | MB_ICONERROR);
    }
    return kExitError;
  }

  if (opts.show_help) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
      FILE* out_stream = nullptr;
      freopen_s(&out_stream, "CONOUT$", "w", stdout);
      std::fwprintf(stdout, L"%ls", usage_text().c_str());
    } else {
      MessageBoxW(nullptr, usage_text().c_str(), L"WinLVM Setup",
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
    const InstallOutcome outcome = run_installation(opts, nullptr, log, {});
    return outcome.exit_code;
  }

  struct ComStaGuard {
    bool initialized = false;
    ComStaGuard() {
      initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    }
    ~ComStaGuard() {
      if (initialized) CoUninitialize();
    }
  } com_guard;

  INITCOMMONCONTROLSEX icc{};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
  InitCommonControlsEx(&icc);

  SetupWindowState state;
  state.opts = opts;
  if (!opts.log_file.empty()) state.logger.set_file(opts.log_file);
  state.testsign_enabled = is_testsigning_enabled();

  // Inspect embedded archive metadata once up front so the UI accurately
  // reflects bundled components (e.g. test certificate, GUI executable).
  ParsedArchive preview;
  std::string preview_err;
  const std::wstring self_path = get_self_exe_path();
  if (!self_path.empty() &&
      parse_and_verify_archive_file(std::filesystem::path(self_path), preview,
                                    preview_err)) {
    state.has_test_cert = (preview.find_entry("driver/winlvm.cer") != nullptr);
    state.has_gui = (preview.find_entry("bin/winlvm_gui.exe") != nullptr);
  } else {
    state.has_gui = true;
  }

  const wchar_t kWndClass[] = L"WinLvmSetupWindow";
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = setup_wnd_proc;
  wc.hInstance = hInstance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = kWndClass;
  if (!RegisterClassExW(&wc)) return kExitError;

  const UINT init_dpi = get_window_dpi(nullptr);
  const int win_w = scale_dpi(540, init_dpi);
  const int win_h = scale_dpi(460, init_dpi);
  const int screen_w = GetSystemMetrics(SM_CXSCREEN);
  const int screen_h = GetSystemMetrics(SM_CYSCREEN);
  const int x = (screen_w - win_w) / 2;
  const int y = (screen_h - win_h) / 2;

  HWND hwnd = CreateWindowExW(
      0u, kWndClass, L"WinLVM Setup",
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
  return static_cast<int>(msg.wParam);
}
