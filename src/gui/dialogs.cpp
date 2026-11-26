#include "dialogs.h"

#include <commctrl.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "dpi.h"

namespace winlvm::gui {
namespace {

struct EmptyDlgTemplate {
  DLGTEMPLATE tmpl;
  WORD menu;
  WORD wnd_class;
  WORD title;
};

EmptyDlgTemplate make_empty_dialog_template() {
  EmptyDlgTemplate t{};
  t.tmpl.style = DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
  t.tmpl.dwExtendedStyle = 0;
  t.tmpl.cdit = 0;
  t.tmpl.x = 0;
  t.tmpl.y = 0;
  t.tmpl.cx = 200;
  t.tmpl.cy = 120;
  return t;
}

void size_and_center_dialog(HWND dlg, HWND owner, int client_w_96, int client_h_96, UINT dpi) {
  RECT rc{0, 0, scale_dpi(client_w_96, dpi), scale_dpi(client_h_96, dpi)};
  const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(dlg, GWL_STYLE));
  const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(dlg, GWL_EXSTYLE));
  AdjustWindowRectEx(&rc, style, FALSE, ex_style);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;

  RECT ref{};
  if (owner && IsWindowVisible(owner)) {
    GetWindowRect(owner, &ref);
  } else {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &ref, 0);
  }
  const int x = ref.left + ((ref.right - ref.left) - w) / 2;
  const int y = ref.top + ((ref.bottom - ref.top) - h) / 2;
  SetWindowPos(dlg, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

HWND add_ctrl(HWND parent, HFONT font, UINT dpi, const wchar_t* cls, const wchar_t* text,
              DWORD style, DWORD ex_style, int x, int y, int w, int h, int id) {
  HWND ctrl = CreateWindowExW(
      ex_style, cls, text, style | WS_CHILD | WS_VISIBLE,
      scale_dpi(x, dpi), scale_dpi(y, dpi), scale_dpi(w, dpi), scale_dpi(h, dpi),
      parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  if (ctrl && font) {
    SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
  }
  return ctrl;
}

std::wstring get_window_text_w(HWND ctrl) {
  const int len = GetWindowTextLengthW(ctrl);
  if (len <= 0) return {};
  std::wstring out(static_cast<std::size_t>(len), L'\0');
  GetWindowTextW(ctrl, out.data(), len + 1);
  return out;
}

bool parse_positive_u64_w(const std::wstring& w, std::uint64_t& out) {
  const std::string s = wide_to_utf8(w);
  std::size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
  std::size_t end = s.size();
  while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
  if (start >= end) return false;
  std::uint64_t val = 0;
  const auto [ptr, ec] = std::from_chars(s.data() + start, s.data() + end, val);
  if (ec != std::errc{} || ptr != s.data() + end || val == 0) return false;
  out = val;
  return true;
}

// ---------------------------------------------------------------------------
// 1. Assign Drive Letter Dialog
// ---------------------------------------------------------------------------
struct DriveLetterCtx {
  HWND owner = nullptr;
  const winlvm::service::LvInfo* lv = nullptr;
  winlvm::service::SetDriveLetterRequest* out = nullptr;
  HFONT font = nullptr;
  std::vector<char> choices;
};

INT_PTR CALLBACK drive_letter_dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  auto* ctx = reinterpret_cast<DriveLetterCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));
  switch (msg) {
    case WM_INITDIALOG: {
      ctx = reinterpret_cast<DriveLetterCtx*>(lp);
      SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
      const UINT dpi = get_window_dpi(dlg);
      ctx->font = create_ui_font(dpi);
      SetWindowTextW(dlg, L"Assign Drive Letter");
      size_and_center_dialog(dlg, ctx->owner, 340, 148, dpi);

      const std::wstring header =
          L"Logical volume: " + utf8_to_wide(ctx->lv->vg_name + "/" + ctx->lv->lv_name);
      add_ctrl(dlg, ctx->font, dpi, L"STATIC", header.c_str(), SS_LEFT, 0, 14, 14, 312, 20, 0);
      add_ctrl(dlg, ctx->font, dpi, L"STATIC", L"Drive letter:", SS_LEFT, 0, 14, 46, 90, 20, 0);
      HWND combo = add_ctrl(dlg, ctx->font, dpi, L"COMBOBOX", L"",
                            CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                            110, 42, 216, 200, 101);

      const DWORD used_mask = GetLogicalDrives();
      const char current_letter =
          ctx->lv->drive_letter.empty() ? '\0' : ctx->lv->drive_letter[0];
      ctx->choices.clear();
      ctx->choices.push_back('\0');
      SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"None (Remove)"));
      int sel_idx = 0;
      for (char c = 'D'; c <= 'Z'; ++c) {
        const bool in_use = (used_mask & (1UL << static_cast<unsigned>(c - 'A'))) != 0;
        const bool is_cur = (c == current_letter || c == current_letter - ('a' - 'A'));
        if (!in_use || is_cur) {
          std::wstring item = {static_cast<wchar_t>(c), L':'};
          if (is_cur) item += L" (Current)";
          const int idx = static_cast<int>(
              SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str())));
          ctx->choices.push_back(c);
          if (is_cur) sel_idx = idx;
        }
      }
      SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(sel_idx), 0);

      add_ctrl(dlg, ctx->font, dpi, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
               156, 104, 80, 26, IDOK);
      add_ctrl(dlg, ctx->font, dpi, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0,
               246, 104, 80, 26, IDCANCEL);
      return TRUE;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK && ctx) {
        const int sel = static_cast<int>(SendDlgItemMessageW(dlg, 101, CB_GETCURSEL, 0, 0));
        if (sel >= 0 && static_cast<std::size_t>(sel) < ctx->choices.size()) {
          ctx->out->vg_name = ctx->lv->vg_name;
          ctx->out->lv_name = ctx->lv->lv_name;
          ctx->out->lv_uuid = ctx->lv->lv_uuid;
          const char ch = ctx->choices[static_cast<std::size_t>(sel)];
          ctx->out->drive_letter = ch == '\0' ? std::string() : std::string(1, ch);
          EndDialog(dlg, IDOK);
          return TRUE;
        }
      } else if (LOWORD(wp) == IDCANCEL) {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
      }
      break;
    case WM_DESTROY:
      if (ctx && ctx->font) {
        DeleteObject(ctx->font);
        ctx->font = nullptr;
      }
      break;
  }
  return FALSE;
}

// ---------------------------------------------------------------------------
// 2. Create PV Dialog
// ---------------------------------------------------------------------------
struct CreatePvCtx {
  HWND owner = nullptr;
  const winlvm::service::Topology* topo = nullptr;
  std::string default_device;
  winlvm::service::CreatePvRequest* out = nullptr;
  HFONT font = nullptr;
  std::vector<std::string> device_paths;
};

INT_PTR CALLBACK create_pv_dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  auto* ctx = reinterpret_cast<CreatePvCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));
  switch (msg) {
    case WM_INITDIALOG: {
      ctx = reinterpret_cast<CreatePvCtx*>(lp);
      SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
      const UINT dpi = get_window_dpi(dlg);
      ctx->font = create_ui_font(dpi);
      SetWindowTextW(dlg, L"Initialize Physical Volume");
      size_and_center_dialog(dlg, ctx->owner, 430, 168, dpi);

      add_ctrl(dlg, ctx->font, dpi, L"STATIC", L"Target device:", SS_LEFT, 0, 14, 16, 110, 20, 0);
      HWND combo = add_ctrl(dlg, ctx->font, dpi, L"COMBOBOX", L"",
                            CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, 0,
                            14, 38, 402, 220, 101);
      int sel_idx = 0;
      for (const auto& d : ctx->topo->disks) {
        if (d.is_system_disk) continue;
        std::wstring label = utf8_to_wide(d.device_path) + L"  [" + utf8_to_wide(d.kind) + L", " +
                             format_bytes_w(d.size_bytes) +
                             (d.is_pv ? L", existing PV]" : L"]");
        const int idx = static_cast<int>(
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())));
        ctx->device_paths.push_back(d.device_path);
        if (d.device_path == ctx->default_device) sel_idx = idx;
      }
      if (!ctx->device_paths.empty()) {
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(sel_idx), 0);
      } else if (!ctx->default_device.empty()) {
        SetWindowTextW(combo, utf8_to_wide(ctx->default_device).c_str());
      }

      add_ctrl(dlg, ctx->font, dpi, L"BUTTON", L"Force overwrite existing LVM label (-f)",
               BS_AUTOCHECKBOX | WS_TABSTOP, 0, 14, 76, 360, 22, 102);

      add_ctrl(dlg, ctx->font, dpi, L"BUTTON", L"Initialize", BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
               246, 124, 80, 26, IDOK);
      add_ctrl(dlg, ctx->font, dpi, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0,
               336, 124, 80, 26, IDCANCEL);
      return TRUE;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK && ctx) {
        HWND combo = GetDlgItem(dlg, 101);
        const int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        std::string dev;
        if (sel >= 0 && static_cast<std::size_t>(sel) < ctx->device_paths.size()) {
          dev = ctx->device_paths[static_cast<std::size_t>(sel)];
        } else {
          dev = wide_to_utf8(get_window_text_w(combo));
        }
        if (!winlvm::service::is_valid_device_path(dev)) {
          MessageBoxW(dlg, L"Select or enter a valid non-system device path.",
                      L"Invalid Device", MB_OK | MB_ICONWARNING);
          return TRUE;
        }
        ctx->out->device_path = dev;
        ctx->out->force = IsDlgButtonChecked(dlg, 102) == BST_CHECKED;
        EndDialog(dlg, IDOK);
        return TRUE;
      } else if (LOWORD(wp) == IDCANCEL) {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
      }
      break;
    case WM_DESTROY:
      if (ctx && ctx->font) {
        DeleteObject(ctx->font);
        ctx->font = nullptr;
      }
      break;
  }
  return FALSE;
}

// ---------------------------------------------------------------------------
// 3. Create VG Dialog
// ---------------------------------------------------------------------------
struct CreateVgCtx {
  HWND owner = nullptr;
  const winlvm::service::Topology* topo = nullptr;
  winlvm::service::CreateVgRequest* out = nullptr;
  HFONT font = nullptr;
  std::vector<std::string> pv_paths;
};

INT_PTR CALLBACK create_vg_dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  auto* ctx = reinterpret_cast<CreateVgCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));
  switch (msg) {
    case WM_INITDIALOG: {
      ctx = reinterpret_cast<CreateVgCtx*>(lp);
      SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
      const UINT dpi = get_window_dpi(dlg);
      ctx->font = create_ui_font(dpi);
      SetWindowTextW(dlg, L"Create Volume Group");
      size_and_center_dialog(dlg, ctx->owner, 420, 290, dpi);

      add_ctrl(dlg, ctx->font, dpi, L"STATIC", L"Volume group name:", SS_LEFT, 0, 14, 16, 130, 20, 0);
      add_ctrl(dlg, ctx->font, dpi, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
               150, 12, 256, 24, 101);

      add_ctrl(dlg, ctx->font, dpi, L"STATIC", L"Extent size:", SS_LEFT, 0, 14, 48, 130, 20, 0);
      HWND ext_cb = add_ctrl(dlg, ctx->font, dpi, L"COMBOBOX", L"",
                             CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                             150, 44, 140, 180, 102);
      static constexpr const wchar_t* kExtLabels[] = {
          L"1 MiB", L"4 MiB", L"8 MiB", L"16 MiB", L"32 MiB", L"64 MiB"};
      for (const wchar_t* s : kExtLabels) {
        SendMessageW(ext_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
      }
      SendMessageW(ext_cb, CB_SETCURSEL, 1, 0);  // 4 MiB default

      add_ctrl(dlg, ctx->font, dpi, L"STATIC", L"Member physical volumes:", SS_LEFT, 0,
               14, 80, 220, 20, 0);
      HWND list = add_ctrl(dlg, ctx->font, dpi, L"LISTBOX", L"",
                           LBS_MULTIPLESEL | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP,
                           WS_EX_CLIENTEDGE, 14, 102, 392, 126, 103);
      for (const auto& pv : ctx->topo->pvs) {
        if (pv.device_path.empty()) continue;
        std::wstring text = utf8_to_wide(pv.device_path) + L"  (" + format_bytes_w(pv.size_bytes) +
                            L", " + (pv.vg_name.empty() ? L"orphan" : (L"VG: " + utf8_to_wide(pv.vg_name))) +
                            L")";
        const int idx = static_cast<int>(
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())));
        ctx->pv_paths.push_back(pv.device_path);
        if (pv.vg_name.empty()) {
          SendMessageW(list, LB_SETSEL, TRUE, static_cast<LPARAM>(idx));
        }
      }

      add_ctrl(dlg, ctx->font, dpi, L"BUTTON", L"Create", BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
               236, 246, 80, 26, IDOK);
      add_ctrl(dlg, ctx->font, dpi, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0,
               326, 246, 80, 26, IDCANCEL);
      return TRUE;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK && ctx) {
        const std::string vg_name = wide_to_utf8(get_window_text_w(GetDlgItem(dlg, 101)));
        if (!winlvm::service::is_valid_lvm_name(vg_name)) {
          MessageBoxW(dlg, L"Enter a valid volume group name (letters, digits, _, +, -, .).",
                      L"Invalid Name", MB_OK | MB_ICONWARNING);
          return TRUE;
        }
        HWND list = GetDlgItem(dlg, 103);
        const int sel_count = static_cast<int>(SendMessageW(list, LB_GETSELCOUNT, 0, 0));
        if (sel_count <= 0) {
          MessageBoxW(dlg, L"Select at least one physical volume.",
                      L"No Physical Volume Selected", MB_OK | MB_ICONWARNING);
          return TRUE;
        }
        std::vector<int> indices(static_cast<std::size_t>(sel_count), 0);
        SendMessageW(list, LB_GETSELITEMS, static_cast<WPARAM>(sel_count),
                     reinterpret_cast<LPARAM>(indices.data()));

        static constexpr std::uint64_t kExtBytes[] = {
            1ULL * 1024 * 1024,  4ULL * 1024 * 1024,  8ULL * 1024 * 1024,
            16ULL * 1024 * 1024, 32ULL * 1024 * 1024, 64ULL * 1024 * 1024};
        const int ext_sel = static_cast<int>(SendDlgItemMessageW(dlg, 102, CB_GETCURSEL, 0, 0));

        ctx->out->vg_name = vg_name;
        ctx->out->pv_paths.clear();
        for (int idx : indices) {
          if (idx >= 0 && static_cast<std::size_t>(idx) < ctx->pv_paths.size()) {
            ctx->out->pv_paths.push_back(ctx->pv_paths[static_cast<std::size_t>(idx)]);
          }
        }
        ctx->out->extent_size_bytes =
            (ext_sel >= 0 && static_cast<std::size_t>(ext_sel) < std::size(kExtBytes))
                ? kExtBytes[static_cast<std::size_t>(ext_sel)]
                : (4ULL * 1024 * 1024);
        ctx->out->force = false;
        EndDialog(dlg, IDOK);
        return TRUE;
      } else if (LOWORD(wp) == IDCANCEL) {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
      }
      break;
    case WM_DESTROY:
      if (ctx && ctx->font) {
        DeleteObject(ctx->font);
        ctx->font = nullptr;
      }
      break;
  }
  return FALSE;
}

// ---------------------------------------------------------------------------
// 4. Create LV Dialog
// ---------------------------------------------------------------------------
struct CreateLvCtx {
  HWND owner = nullptr;
  const winlvm::service::Topology* topo = nullptr;
  std::string default_vg;
  winlvm::service::CreateLvRequest* out = nullptr;
  HFONT font = nullptr;
  std::vector<std::string> vg_names;
  std::vector<std::string> pool_names;
  std::vector<std::string> origin_names;
};

void refresh_create_lv_dependent_lists(HWND dlg, CreateLvCtx* ctx) {
  const int vg_idx = static_cast<int>(SendDlgItemMessageW(dlg, 101, CB_GETCURSEL, 0, 0));
  const std::string vg =
      (vg_idx >= 0 && static_cast<std::size_t>(vg_idx) < ctx->vg_names.size())
          ? ctx->vg_names[static_cast<std::size_t>(vg_idx)]
          : std::string();

  HWND pool_cb = GetDlgItem(dlg, 107);
  HWND orig_cb = GetDlgItem(dlg, 108);
  SendMessageW(pool_cb, CB_RESETCONTENT, 0, 0);
  SendMessageW(orig_cb, CB_RESETCONTENT, 0, 0);
  ctx->pool_names.clear();
  ctx->origin_names.clear();

  for (const auto& lv : ctx->topo->lvs) {
    if (lv.vg_name != vg || lv.internal) continue;
    if (lv.segment_type == "thin-pool") {
      ctx->pool_names.push_back(lv.lv_name);
      SendMessageW(pool_cb, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(utf8_to_wide(lv.lv_name).c_str()));
    } else {
      ctx->origin_names.push_back(lv.lv_name);
      SendMessageW(orig_cb, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(utf8_to_wide(lv.lv_name).c_str()));
    }
  }
  if (!ctx->pool_names.empty()) SendMessageW(pool_cb, CB_SETCURSEL, 0, 0);
  if (!ctx->origin_names.empty()) SendMessageW(orig_cb, CB_SETCURSEL, 0, 0);
}

void update_create_lv_field_states(HWND dlg) {
  const int type_idx = static_cast<int>(SendDlgItemMessageW(dlg, 103, CB_GETCURSEL, 0, 0));
  // 0=linear, 1=striped, 2=thin-pool, 3=thin, 4=snapshot
  EnableWindow(GetDlgItem(dlg, 106), type_idx == 1);  // stripes
  EnableWindow(GetDlgItem(dlg, 107), type_idx == 3);  // thin pool
  EnableWindow(GetDlgItem(dlg, 108), type_idx == 4);  // snapshot origin
  EnableWindow(GetDlgItem(dlg, 109), type_idx == 0 || type_idx == 1);  // read-only
  EnableWindow(GetDlgItem(dlg, 110), type_idx != 2);  // activate immediately
}

INT_PTR CALLBACK create_lv_dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  auto* ctx = reinterpret_cast<CreateLvCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));
  switch (msg) {
    case WM_INITDIALOG: {
      ctx = reinterpret_cast<CreateLvCtx*>(lp);
      SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
      const UINT dpi = get_window_dpi(dlg);
      ctx->font = create_ui_font(dpi);
      SetWindowTextW(dlg, L"Create Logical Volume");
      size_and_center_dialog(dlg, ctx->owner, 410, 332, dpi);

      add_ctrl(dlg, ctx->font, dpi, L"STATIC", L"Volume group:", SS_LEFT, 0, 14, 16, 110, 20, 0);
      HWND vg_cb = add_ctrl(dlg, ctx->font, dpi, L"COMBOBOX", L"",
                            CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                            132, 12, 264, 180, 101);
      int vg_sel = 0;
      for (const auto& vg : ctx->topo->vgs) {
        const std::wstring label =
            utf8_to_wide(vg.name) + L"  (free: " + format_bytes_w(vg.free_bytes) + L")";
        const int idx = static_cast<int>(
            SendMessageW(vg_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())));
        ctx->vg_names.push_back(vg.name);
        if (vg.name == ctx->default_vg) vg_sel = idx;
      }
      if (!ctx->vg_names.empty()) SendMessageW(vg_cb, CB_SETCURSEL, static_cast<WPARAM>(vg_sel), 0);

      add_ctrl(dlg, ctx->font, dpi, L"STATIC", L"Volume name:", SS_LEFT, 0, 14, 48, 110, 20, 0);
      add_ctrl(dlg, ctx->font, dpi, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
               132, 44, 264, 24, 102);

      add_ctrl(dlg, ctx->font, dpi, L"STATIC", L"Type:", SS_LEFT, 0, 14, 80, 110, 20, 0);
      HWND type_cb = add_ctrl(dlg, ctx->font, dpi, L"COMBOBOX", L"",
                              CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                              132, 76, 264, 180, 103);
      static constexpr const wchar_t* kTypes[] = {
          L"linear", L"striped", L"thin-pool", L"thin", L"snapshot"};
      for (const wchar_t* t : kTypes) {
        SendMessageW(type_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
      }
      SendMessageW(type_cb, CB_SETCURSEL, 0, 0);

      add_ctrl(dlg, ctx->font, dpi, L"STATIC", L"Size:", SS_LEFT, 0, 14, 112, 110, 20, 0);
      add_ctrl(dlg, ctx->font, dpi, L"EDIT", L"64", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
               WS_EX_CLIENTEDGE, 132, 108, 160, 24, 104);
      HWND unit_cb = add_ctrl(dlg, ctx->font, dpi, L"COMBOBOX", L"",
                              CBS_DROPDOWNLIST | WS_TABSTOP, 0, 300, 108, 96, 120, 105);
      SendMessageW(unit_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MiB"));
      SendMessageW(unit_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GiB"));
      SendMessageW(unit_cb, CB_SETCURSEL, 0, 0);

      add_ctrl(dlg, ctx->font, dpi, L"STATIC", L"Stripes:", SS_LEFT, 0, 14, 144, 110, 20, 0);
      add_ctrl(dlg, ctx->font, dpi, L"EDIT", L"2", ES_NUMBER | WS_TABSTOP, WS_EX_CLIENTEDGE,
               132, 140, 80, 24, 106);

      add_ctrl(dlg, ctx->font, dpi, L"STATIC", L"Thin pool:", SS_LEFT, 0, 14, 176, 110, 20, 0);
      add_ctrl(dlg, ctx->font, dpi, L"COMBOBOX", L"",
               CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 132, 172, 264, 160, 107);

      add_ctrl(dlg, ctx->font, dpi, L"STATIC", L"Origin volume:", SS_LEFT, 0, 14, 208, 110, 20, 0);
      add_ctrl(dlg, ctx->font, dpi, L"COMBOBOX", L"",
               CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 132, 204, 264, 160, 108);

      add_ctrl(dlg, ctx->font, dpi, L"BUTTON", L"Read-only", BS_AUTOCHECKBOX | WS_TABSTOP, 0,
               132, 238, 110, 22, 109);
      HWND act_chk = add_ctrl(dlg, ctx->font, dpi, L"BUTTON", L"Activate immediately",
                              BS_AUTOCHECKBOX | WS_TABSTOP, 0, 248, 238, 150, 22, 110);
      SendMessageW(act_chk, BM_SETCHECK, BST_CHECKED, 0);

      add_ctrl(dlg, ctx->font, dpi, L"BUTTON", L"Create", BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
               226, 286, 80, 26, IDOK);
      add_ctrl(dlg, ctx->font, dpi, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0,
               316, 286, 80, 26, IDCANCEL);

      refresh_create_lv_dependent_lists(dlg, ctx);
      update_create_lv_field_states(dlg);
      return TRUE;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == 101 && HIWORD(wp) == CBN_SELCHANGE && ctx) {
        refresh_create_lv_dependent_lists(dlg, ctx);
        return TRUE;
      }
      if (LOWORD(wp) == 103 && HIWORD(wp) == CBN_SELCHANGE) {
        update_create_lv_field_states(dlg);
        return TRUE;
      }
      if (LOWORD(wp) == IDOK && ctx) {
        const int vg_idx = static_cast<int>(SendDlgItemMessageW(dlg, 101, CB_GETCURSEL, 0, 0));
        if (vg_idx < 0 || static_cast<std::size_t>(vg_idx) >= ctx->vg_names.size()) {
          MessageBoxW(dlg, L"Select a volume group.", L"Missing Volume Group", MB_OK | MB_ICONWARNING);
          return TRUE;
        }
        const std::string lv_name = wide_to_utf8(get_window_text_w(GetDlgItem(dlg, 102)));
        if (!winlvm::service::is_valid_lvm_name(lv_name)) {
          MessageBoxW(dlg, L"Enter a valid logical volume name.", L"Invalid Name", MB_OK | MB_ICONWARNING);
          return TRUE;
        }
        std::uint64_t raw_size = 0;
        if (!parse_positive_u64_w(get_window_text_w(GetDlgItem(dlg, 104)), raw_size)) {
          MessageBoxW(dlg, L"Enter a positive integer size.", L"Invalid Size", MB_OK | MB_ICONWARNING);
          return TRUE;
        }
        const int unit_idx = static_cast<int>(SendDlgItemMessageW(dlg, 105, CB_GETCURSEL, 0, 0));
        const std::uint64_t mult = (unit_idx == 1) ? (1024ULL * 1024ULL * 1024ULL) : (1024ULL * 1024ULL);
        if (raw_size > (std::numeric_limits<std::uint64_t>::max() / mult)) {
          MessageBoxW(dlg, L"Requested size is out of range.", L"Invalid Size", MB_OK | MB_ICONWARNING);
          return TRUE;
        }
        const std::uint64_t bytes = raw_size * mult;

        static constexpr const char* kTypeNames[] = {
            "linear", "striped", "thin-pool", "thin", "snapshot"};
        const int type_idx = static_cast<int>(SendDlgItemMessageW(dlg, 103, CB_GETCURSEL, 0, 0));
        if (type_idx < 0 || static_cast<std::size_t>(type_idx) >= std::size(kTypeNames)) {
          return TRUE;
        }

        winlvm::service::CreateLvRequest req;
        req.vg_name = ctx->vg_names[static_cast<std::size_t>(vg_idx)];
        req.lv_name = lv_name;
        req.segment_type = kTypeNames[static_cast<std::size_t>(type_idx)];
        req.read_only = IsDlgButtonChecked(dlg, 109) == BST_CHECKED;
        req.activate = IsDlgButtonChecked(dlg, 110) == BST_CHECKED;

        if (req.segment_type == "striped") {
          std::uint64_t stripes = 0;
          if (!parse_positive_u64_w(get_window_text_w(GetDlgItem(dlg, 106)), stripes) ||
              stripes < 2 || stripes > 128) {
            MessageBoxW(dlg, L"Striped volumes require a stripe count between 2 and 128.",
                        L"Invalid Stripes", MB_OK | MB_ICONWARNING);
            return TRUE;
          }
          req.stripes = static_cast<std::uint32_t>(stripes);
          req.size_bytes = bytes;
        } else if (req.segment_type == "thin") {
          const int pool_idx = static_cast<int>(SendDlgItemMessageW(dlg, 107, CB_GETCURSEL, 0, 0));
          if (pool_idx < 0 || static_cast<std::size_t>(pool_idx) >= ctx->pool_names.size()) {
            MessageBoxW(dlg, L"Create or select a thin pool in this volume group first.",
                        L"Missing Thin Pool", MB_OK | MB_ICONWARNING);
            return TRUE;
          }
          req.pool_name = ctx->pool_names[static_cast<std::size_t>(pool_idx)];
          req.virtual_size_bytes = bytes;
        } else if (req.segment_type == "snapshot") {
          const int orig_idx = static_cast<int>(SendDlgItemMessageW(dlg, 108, CB_GETCURSEL, 0, 0));
          if (orig_idx < 0 || static_cast<std::size_t>(orig_idx) >= ctx->origin_names.size()) {
            MessageBoxW(dlg, L"Select an origin logical volume for the snapshot.",
                        L"Missing Origin Volume", MB_OK | MB_ICONWARNING);
            return TRUE;
          }
          req.origin_name = ctx->origin_names[static_cast<std::size_t>(orig_idx)];
          req.size_bytes = bytes;
        } else {
          req.size_bytes = bytes;
        }

        *ctx->out = std::move(req);
        EndDialog(dlg, IDOK);
        return TRUE;
      } else if (LOWORD(wp) == IDCANCEL) {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
      }
      break;
    case WM_DESTROY:
      if (ctx && ctx->font) {
        DeleteObject(ctx->font);
        ctx->font = nullptr;
      }
      break;
  }
  return FALSE;
}

// ---------------------------------------------------------------------------
// 5. Extend LV Dialog
// ---------------------------------------------------------------------------
struct ExtendLvCtx {
  HWND owner = nullptr;
  const winlvm::service::Topology* topo = nullptr;
  const winlvm::service::LvInfo* lv = nullptr;
  winlvm::service::ResizeLvRequest* out = nullptr;
  HFONT font = nullptr;
};

INT_PTR CALLBACK extend_lv_dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  auto* ctx = reinterpret_cast<ExtendLvCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));
  switch (msg) {
    case WM_INITDIALOG: {
      ctx = reinterpret_cast<ExtendLvCtx*>(lp);
      SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
      const UINT dpi = get_window_dpi(dlg);
      ctx->font = create_ui_font(dpi);
      SetWindowTextW(dlg, L"Extend Logical Volume");
      size_and_center_dialog(dlg, ctx->owner, 380, 174, dpi);

      std::uint64_t vg_free = 0;
      for (const auto& vg : ctx->topo->vgs) {
        if (vg.name == ctx->lv->vg_name) {
          vg_free = vg.free_bytes;
          break;
        }
      }

      const std::wstring info =
          utf8_to_wide(ctx->lv->vg_name + "/" + ctx->lv->lv_name) + L" - Current: " +
          format_bytes_w(ctx->lv->size_bytes) + L"  (VG free: " + format_bytes_w(vg_free) + L")";
      add_ctrl(dlg, ctx->font, dpi, L"STATIC", info.c_str(), SS_LEFT, 0, 14, 14, 352, 36, 0);

      add_ctrl(dlg, ctx->font, dpi, L"STATIC", L"New total size:", SS_LEFT, 0, 14, 66, 110, 20, 0);
      const std::uint64_t default_mib =
          (ctx->lv->size_bytes / (1024ULL * 1024ULL)) + 16ULL;
      add_ctrl(dlg, ctx->font, dpi, L"EDIT", std::to_wstring(default_mib).c_str(),
               ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
               130, 62, 136, 24, 101);
      HWND unit_cb = add_ctrl(dlg, ctx->font, dpi, L"COMBOBOX", L"",
                              CBS_DROPDOWNLIST | WS_TABSTOP, 0, 274, 62, 92, 120, 102);
      SendMessageW(unit_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MiB"));
      SendMessageW(unit_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GiB"));
      SendMessageW(unit_cb, CB_SETCURSEL, 0, 0);

      add_ctrl(dlg, ctx->font, dpi, L"BUTTON", L"Extend", BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
               196, 128, 80, 26, IDOK);
      add_ctrl(dlg, ctx->font, dpi, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0,
               286, 128, 80, 26, IDCANCEL);
      return TRUE;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK && ctx) {
        std::uint64_t raw = 0;
        if (!parse_positive_u64_w(get_window_text_w(GetDlgItem(dlg, 101)), raw)) {
          MessageBoxW(dlg, L"Enter a positive integer size.", L"Invalid Size", MB_OK | MB_ICONWARNING);
          return TRUE;
        }
        const int unit_idx = static_cast<int>(SendDlgItemMessageW(dlg, 102, CB_GETCURSEL, 0, 0));
        const std::uint64_t mult = (unit_idx == 1) ? (1024ULL * 1024ULL * 1024ULL) : (1024ULL * 1024ULL);
        if (raw > (std::numeric_limits<std::uint64_t>::max() / mult) ||
            raw * mult <= ctx->lv->size_bytes) {
          MessageBoxW(dlg, L"New size must be strictly larger than the current volume size.",
                      L"Invalid Size", MB_OK | MB_ICONWARNING);
          return TRUE;
        }
        ctx->out->vg_name = ctx->lv->vg_name;
        ctx->out->lv_name = ctx->lv->lv_name;
        ctx->out->lv_uuid = ctx->lv->lv_uuid;
        ctx->out->new_size_bytes = raw * mult;
        EndDialog(dlg, IDOK);
        return TRUE;
      } else if (LOWORD(wp) == IDCANCEL) {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
      }
      break;
    case WM_DESTROY:
      if (ctx && ctx->font) {
        DeleteObject(ctx->font);
        ctx->font = nullptr;
      }
      break;
  }
  return FALSE;
}

// ---------------------------------------------------------------------------
// 6. Rename LV Dialog
// ---------------------------------------------------------------------------
struct RenameLvCtx {
  HWND owner = nullptr;
  const winlvm::service::LvInfo* lv = nullptr;
  winlvm::service::RenameLvRequest* out = nullptr;
  HFONT font = nullptr;
};

INT_PTR CALLBACK rename_lv_dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  auto* ctx = reinterpret_cast<RenameLvCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));
  switch (msg) {
    case WM_INITDIALOG: {
      ctx = reinterpret_cast<RenameLvCtx*>(lp);
      SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
      const UINT dpi = get_window_dpi(dlg);
      ctx->font = create_ui_font(dpi);
      SetWindowTextW(dlg, L"Rename Logical Volume");
      size_and_center_dialog(dlg, ctx->owner, 360, 148, dpi);

      const std::wstring cur =
          L"Current volume: " + utf8_to_wide(ctx->lv->vg_name + "/" + ctx->lv->lv_name);
      add_ctrl(dlg, ctx->font, dpi, L"STATIC", cur.c_str(), SS_LEFT, 0, 14, 14, 332, 20, 0);
      add_ctrl(dlg, ctx->font, dpi, L"STATIC", L"New name:", SS_LEFT, 0, 14, 48, 90, 20, 0);
      add_ctrl(dlg, ctx->font, dpi, L"EDIT", utf8_to_wide(ctx->lv->lv_name).c_str(),
               ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 110, 44, 236, 24, 101);

      add_ctrl(dlg, ctx->font, dpi, L"BUTTON", L"Rename", BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
               176, 104, 80, 26, IDOK);
      add_ctrl(dlg, ctx->font, dpi, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0,
               266, 104, 80, 26, IDCANCEL);
      return TRUE;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK && ctx) {
        const std::string name = wide_to_utf8(get_window_text_w(GetDlgItem(dlg, 101)));
        if (!winlvm::service::is_valid_lvm_name(name) || name == ctx->lv->lv_name) {
          MessageBoxW(dlg, L"Enter a valid new logical volume name different from the current name.",
                      L"Invalid Name", MB_OK | MB_ICONWARNING);
          return TRUE;
        }
        ctx->out->vg_name = ctx->lv->vg_name;
        ctx->out->old_lv_name = ctx->lv->lv_name;
        ctx->out->new_lv_name = name;
        EndDialog(dlg, IDOK);
        return TRUE;
      } else if (LOWORD(wp) == IDCANCEL) {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
      }
      break;
    case WM_DESTROY:
      if (ctx && ctx->font) {
        DeleteObject(ctx->font);
        ctx->font = nullptr;
      }
      break;
  }
  return FALSE;
}

}  // namespace

bool show_drive_letter_dialog(HWND owner,
                              const winlvm::service::LvInfo& lv,
                              winlvm::service::SetDriveLetterRequest& out_req) {
  alignas(4) EmptyDlgTemplate tmpl = make_empty_dialog_template();
  DriveLetterCtx ctx{owner, &lv, &out_req, nullptr, {}};
  return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &tmpl.tmpl, owner,
                                 drive_letter_dlg_proc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
}

bool show_create_pv_dialog(HWND owner,
                           const winlvm::service::Topology& topo,
                           const std::string& default_device,
                           winlvm::service::CreatePvRequest& out_req) {
  alignas(4) EmptyDlgTemplate tmpl = make_empty_dialog_template();
  CreatePvCtx ctx{owner, &topo, default_device, &out_req, nullptr, {}};
  return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &tmpl.tmpl, owner,
                                 create_pv_dlg_proc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
}

bool show_create_vg_dialog(HWND owner,
                           const winlvm::service::Topology& topo,
                           winlvm::service::CreateVgRequest& out_req) {
  alignas(4) EmptyDlgTemplate tmpl = make_empty_dialog_template();
  CreateVgCtx ctx{owner, &topo, &out_req, nullptr, {}};
  return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &tmpl.tmpl, owner,
                                 create_vg_dlg_proc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
}

bool show_create_lv_dialog(HWND owner,
                           const winlvm::service::Topology& topo,
                           const std::string& default_vg,
                           winlvm::service::CreateLvRequest& out_req) {
  alignas(4) EmptyDlgTemplate tmpl = make_empty_dialog_template();
  CreateLvCtx ctx{owner, &topo, default_vg, &out_req, nullptr, {}, {}, {}};
  return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &tmpl.tmpl, owner,
                                 create_lv_dlg_proc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
}

bool show_extend_lv_dialog(HWND owner,
                           const winlvm::service::Topology& topo,
                           const winlvm::service::LvInfo& lv,
                           winlvm::service::ResizeLvRequest& out_req) {
  alignas(4) EmptyDlgTemplate tmpl = make_empty_dialog_template();
  ExtendLvCtx ctx{owner, &topo, &lv, &out_req, nullptr};
  return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &tmpl.tmpl, owner,
                                 extend_lv_dlg_proc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
}

bool show_rename_lv_dialog(HWND owner,
                           const winlvm::service::LvInfo& lv,
                           winlvm::service::RenameLvRequest& out_req) {
  alignas(4) EmptyDlgTemplate tmpl = make_empty_dialog_template();
  RenameLvCtx ctx{owner, &lv, &out_req, nullptr};
  return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &tmpl.tmpl, owner,
                                 rename_lv_dlg_proc, reinterpret_cast<LPARAM>(&ctx)) == IDOK;
}

}  // namespace winlvm::gui
