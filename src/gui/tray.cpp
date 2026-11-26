#include "tray.h"

#include "dpi.h"
#include "resource.h"

namespace winlvm::gui {

bool SystemTray::install(HWND owner, HICON icon) {
  if (installed_) remove();
  ZeroMemory(&nid_, sizeof(nid_));
  nid_.cbSize = sizeof(nid_);
  nid_.hWnd = owner;
  nid_.uID = 1;
  nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  nid_.uCallbackMessage = WM_APP_TRAY_ICON;
  nid_.hIcon = icon;
  lstrcpynW(nid_.szTip, L"WinLVM Volume Manager", static_cast<int>(std::size(nid_.szTip)));
  installed_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
  return installed_;
}

void SystemTray::remove() {
  if (!installed_) return;
  Shell_NotifyIconW(NIM_DELETE, &nid_);
  installed_ = false;
}

void SystemTray::update_tooltip(const std::wstring& tip) {
  if (!installed_) return;
  nid_.uFlags = NIF_TIP;
  lstrcpynW(nid_.szTip, tip.c_str(), static_cast<int>(std::size(nid_.szTip)));
  Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void SystemTray::show_context_menu(HWND owner, const winlvm::service::Topology& topo,
                                   std::vector<TrayLvEntry>& out_entries) {
  out_entries.clear();
  HMENU menu = CreatePopupMenu();
  if (!menu) return;

  constexpr std::size_t kMaxTrayLvs = 32;
  std::size_t idx = 0;
  for (const auto& lv : topo.lvs) {
    if (lv.internal || idx >= kMaxTrayLvs) continue;
    TrayLvEntry entry;
    entry.vg_name = lv.vg_name;
    entry.lv_name = lv.lv_name;
    entry.lv_uuid = lv.lv_uuid;
    entry.drive_letter = lv.drive_letter;
    entry.active = lv.active;
    out_entries.push_back(entry);

    const std::wstring lv_label = utf8_to_wide(lv.vg_name + "/" + lv.lv_name);
    if (lv.active && !lv.drive_letter.empty()) {
      std::wstring drive = utf8_to_wide(lv.drive_letter);
      if (!drive.empty() && drive.back() == L'\\') drive.pop_back();
      const std::wstring open_text = L"Open " + lv_label + L" (" + drive + L")";
      AppendMenuW(menu, MF_STRING, IDM_TRAY_LV_OPEN_BASE + idx, open_text.c_str());
    }
    if (lv.active) {
      const std::wstring deact_text = L"Deactivate " + lv_label;
      AppendMenuW(menu, MF_STRING, IDM_TRAY_LV_DEACT_BASE + idx, deact_text.c_str());
    } else if (lv.supported) {
      const std::wstring act_text = L"Activate " + lv_label;
      AppendMenuW(menu, MF_STRING, IDM_TRAY_LV_ACT_BASE + idx, act_text.c_str());
    }
    ++idx;
  }

  if (idx > 0) {
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  }
  AppendMenuW(menu, MF_STRING, IDM_TRAY_RESCAN, L"Rescan Volumes");
  AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN_MANAGER, L"Open WinLVM Volume Manager");
  SetMenuDefaultItem(menu, IDM_TRAY_OPEN_MANAGER, FALSE);
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Exit");

  POINT pt{};
  GetCursorPos(&pt);
  SetForegroundWindow(owner);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, owner, nullptr);
  PostMessageW(owner, WM_NULL, 0, 0);
  DestroyMenu(menu);
}

}  // namespace winlvm::gui
