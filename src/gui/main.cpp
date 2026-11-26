#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <dbt.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../service/ipc_client.h"
#include "../service/protocol.h"
#include "dialogs.h"
#include "dpi.h"
#include "resource.h"
#include "tray.h"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

namespace {

constexpr wchar_t kMainWndClass[] = L"WinLvmVolumeManagerWindow";
constexpr wchar_t kMainTitle[] = L"WinLVM Volume Manager";

constexpr GUID kGuidDevInterfaceDisk = {
    0x53f56307U, 0xb6bfU, 0x11d0U, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
constexpr GUID kGuidDevInterfaceVolume = {
    0x53f5630dU, 0xb6bfU, 0x11d0U, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

enum class NodeKind {
  RootDisks,
  RootPvs,
  RootVgs,
  Disk,
  Pv,
  Vg,
  Lv,
};

struct TreeTag {
  NodeKind kind = NodeKind::RootVgs;
  std::string key;       // device_path, pv_uuid, vg_name, or lv_uuid
  std::string vg_name;   // for Vg/Lv
};

struct ListRowTag {
  NodeKind kind = NodeKind::Lv;
  std::size_t index = 0;  // index into topology.disks / pvs / vgs / lvs
};

struct AppState {
  HINSTANCE hinst = nullptr;
  HWND hwnd = nullptr;
  HWND toolbar = nullptr;
  HWND tree = nullptr;
  HWND list = nullptr;
  HWND status = nullptr;
  HICON icon = nullptr;
  HFONT ui_font = nullptr;
  UINT dpi = 96;
  UINT msg_taskbar_created = 0;

  HDEVNOTIFY notify_disk = nullptr;
  HDEVNOTIFY notify_vol = nullptr;

  winlvm::service::ServiceClient client;
  winlvm::service::Topology topo;
  std::string last_topo_json;
  std::string last_error;

  winlvm::gui::SystemTray tray;
  std::vector<winlvm::gui::TrayLvEntry> tray_lvs;

  std::vector<TreeTag> tree_tags;
  std::vector<ListRowTag> list_rows;
  NodeKind selected_tree_kind = NodeKind::RootVgs;
  std::string selected_tree_key;

  int splitter_x_96 = 250;
  bool dragging_splitter = false;
  bool show_internal_lvs = false;
  bool in_modal_dialog = false;
  bool suppress_tree_sel_change = false;
  bool force_exit = false;
  bool smoke_test = false;
};

using winlvm::gui::apply_explorer_theme;
using winlvm::gui::create_ui_font;
using winlvm::gui::format_bytes_w;
using winlvm::gui::get_window_dpi;
using winlvm::gui::scale_dpi;
using winlvm::gui::utf8_to_wide;

void update_toolbar_states(AppState& app);
void refresh_topology(AppState& app, bool force_rebuild);

const winlvm::service::LvInfo* get_selected_lv(const AppState& app) {
  const int sel = ListView_GetNextItem(app.list, -1, LVNI_SELECTED);
  if (sel >= 0 && static_cast<std::size_t>(sel) < app.list_rows.size()) {
    const auto& row = app.list_rows[static_cast<std::size_t>(sel)];
    if (row.kind == NodeKind::Lv && row.index < app.topo.lvs.size()) {
      return &app.topo.lvs[row.index];
    }
  }
  if (app.selected_tree_kind == NodeKind::Lv) {
    for (const auto& lv : app.topo.lvs) {
      if (lv.lv_uuid == app.selected_tree_key) return &lv;
    }
  }
  return nullptr;
}

std::string get_selected_vg_name(const AppState& app) {
  if (const auto* lv = get_selected_lv(app)) return lv->vg_name;
  const int sel = ListView_GetNextItem(app.list, -1, LVNI_SELECTED);
  if (sel >= 0 && static_cast<std::size_t>(sel) < app.list_rows.size()) {
    const auto& row = app.list_rows[static_cast<std::size_t>(sel)];
    if (row.kind == NodeKind::Vg && row.index < app.topo.vgs.size()) {
      return app.topo.vgs[row.index].name;
    }
  }
  if (app.selected_tree_kind == NodeKind::Vg) return app.selected_tree_key;
  return app.topo.vgs.empty() ? std::string() : app.topo.vgs.front().name;
}

std::string get_selected_disk_path(const AppState& app) {
  const int sel = ListView_GetNextItem(app.list, -1, LVNI_SELECTED);
  if (sel >= 0 && static_cast<std::size_t>(sel) < app.list_rows.size()) {
    const auto& row = app.list_rows[static_cast<std::size_t>(sel)];
    if (row.kind == NodeKind::Disk && row.index < app.topo.disks.size()) {
      return app.topo.disks[row.index].device_path;
    }
  }
  if (app.selected_tree_kind == NodeKind::Disk) return app.selected_tree_key;
  return {};
}

void layout_controls(AppState& app) {
  RECT rc{};
  GetClientRect(app.hwnd, &rc);
  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;

  SendMessageW(app.toolbar, TB_AUTOSIZE, 0, 0);
  RECT tb_rc{};
  GetWindowRect(app.toolbar, &tb_rc);
  const int tb_h = tb_rc.bottom - tb_rc.top;

  SendMessageW(app.status, WM_SIZE, 0, 0);
  RECT sb_rc{};
  GetWindowRect(app.status, &sb_rc);
  const int sb_h = sb_rc.bottom - sb_rc.top;

  int parts[3] = {
      std::max(scale_dpi(260, app.dpi), cw / 3),
      std::max(scale_dpi(500, app.dpi), (cw * 2) / 3),
      -1,
  };
  SendMessageW(app.status, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));

  const int split_gap = scale_dpi(5, app.dpi);
  int split_x = scale_dpi(app.splitter_x_96, app.dpi);
  const int min_left = scale_dpi(160, app.dpi);
  const int min_right = scale_dpi(240, app.dpi);
  if (cw > min_left + min_right + split_gap) {
    split_x = std::clamp(split_x, min_left, cw - min_right - split_gap);
  } else {
    split_x = cw / 3;
  }

  const int body_y = tb_h;
  const int body_h = std::max(0, ch - tb_h - sb_h);
  MoveWindow(app.tree, 0, body_y, split_x, body_h, TRUE);
  MoveWindow(app.list, split_x + split_gap, body_y,
             std::max(0, cw - split_x - split_gap), body_h, TRUE);
}

void apply_font_to_controls(AppState& app) {
  HFONT next = create_ui_font(app.dpi);
  if (!next) return;
  HFONT old_font = app.ui_font;
  app.ui_font = next;
  SendMessageW(app.toolbar, WM_SETFONT, reinterpret_cast<WPARAM>(app.ui_font), TRUE);
  SendMessageW(app.tree, WM_SETFONT, reinterpret_cast<WPARAM>(app.ui_font), TRUE);
  SendMessageW(app.list, WM_SETFONT, reinterpret_cast<WPARAM>(app.ui_font), TRUE);
  SendMessageW(app.status, WM_SETFONT, reinterpret_cast<WPARAM>(app.ui_font), TRUE);
  if (old_font) DeleteObject(old_font);
}

void add_list_row(AppState& app, const ListRowTag& tag, const std::wstring cols[8]) {
  LVITEMW item{};
  item.mask = LVIF_TEXT;
  item.iItem = static_cast<int>(app.list_rows.size());
  item.pszText = const_cast<LPWSTR>(cols[0].c_str());
  const int row = ListView_InsertItem(app.list, &item);
  if (row < 0) return;
  for (int c = 1; c < 8; ++c) {
    ListView_SetItemText(app.list, row, c, const_cast<LPWSTR>(cols[c].c_str()));
  }
  app.list_rows.push_back(tag);
}

void populate_list_view(AppState& app) {
  SendMessageW(app.list, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(app.list);
  app.list_rows.clear();

  auto append_lv = [&](std::size_t idx) {
    const auto& lv = app.topo.lvs[idx];
    if (lv.internal && !app.show_internal_lvs) return;
    std::wstring status = lv.active ? L"Active" : L"Inactive";
    if (!lv.supported) status += L" (Unsupported)";
    std::wstring drive = utf8_to_wide(lv.drive_letter);
    if (!lv.filesystem.empty()) {
      if (!drive.empty()) drive += L" ";
      drive += L"(" + utf8_to_wide(lv.filesystem);
      if (!lv.volume_label.empty()) drive += L": " + utf8_to_wide(lv.volume_label);
      drive += L")";
    }
    std::wstring cols[8] = {
        utf8_to_wide(lv.vg_name + "/" + lv.lv_name),
        utf8_to_wide(lv.segment_type),
        format_bytes_w(lv.size_bytes),
        status,
        lv.read_only ? L"RO" : L"RW",
        drive,
        utf8_to_wide(lv.backing_info),
        utf8_to_wide(lv.lv_uuid),
    };
    add_list_row(app, {NodeKind::Lv, idx}, cols);
  };

  switch (app.selected_tree_kind) {
    case NodeKind::RootDisks:
    case NodeKind::Disk: {
      for (std::size_t i = 0; i < app.topo.disks.size(); ++i) {
        const auto& d = app.topo.disks[i];
        if (app.selected_tree_kind == NodeKind::Disk && d.device_path != app.selected_tree_key) {
          continue;
        }
        std::wstring status = d.is_pv ? (d.vg_name.empty() ? L"Orphan PV" : (L"VG: " + utf8_to_wide(d.vg_name)))
                                      : (d.is_system_disk ? L"System Disk" : L"Unallocated");
        std::wstring cols[8] = {
            utf8_to_wide(d.device_path),
            utf8_to_wide(d.kind),
            format_bytes_w(d.size_bytes),
            status,
            d.is_system_disk ? L"Protected" : L"RW",
            L"",
            utf8_to_wide(d.nt_target),
            utf8_to_wide(d.pv_uuid),
        };
        add_list_row(app, {NodeKind::Disk, i}, cols);
      }
      break;
    }
    case NodeKind::RootPvs:
    case NodeKind::Pv: {
      for (std::size_t i = 0; i < app.topo.pvs.size(); ++i) {
        const auto& pv = app.topo.pvs[i];
        if (app.selected_tree_kind == NodeKind::Pv && pv.pv_uuid != app.selected_tree_key) {
          continue;
        }
        std::wstring status = pv.missing ? L"Missing"
                                         : (pv.vg_name.empty() ? L"Orphan" : (L"VG: " + utf8_to_wide(pv.vg_name)));
        std::wstring backing = L"Free: " + format_bytes_w(pv.free_bytes) + L", MDAs: " +
                               std::to_wstring(pv.mda_count);
        std::wstring cols[8] = {
            utf8_to_wide(pv.device_path.empty() ? pv.pv_key : pv.device_path),
            L"physical volume",
            format_bytes_w(pv.size_bytes),
            status,
            pv.allocatable ? L"Allocatable" : L"Non-allocatable",
            L"",
            backing,
            utf8_to_wide(pv.pv_uuid),
        };
        add_list_row(app, {NodeKind::Pv, i}, cols);
      }
      break;
    }
    case NodeKind::RootVgs: {
      for (std::size_t i = 0; i < app.topo.lvs.size(); ++i) {
        append_lv(i);
      }
      if (app.list_rows.empty()) {
        for (std::size_t i = 0; i < app.topo.vgs.size(); ++i) {
          const auto& vg = app.topo.vgs[i];
          std::wstring status = vg.exported ? L"Exported" : L"Available";
          std::wstring backing = L"PVs: " + std::to_wstring(vg.pv_count) + L", Free: " +
                                 format_bytes_w(vg.free_bytes) + L", Seq: " +
                                 std::to_wstring(vg.seqno);
          std::wstring cols[8] = {
              utf8_to_wide(vg.name),
              L"volume group",
              format_bytes_w(vg.size_bytes),
              status,
              vg.read_only ? L"RO" : L"RW",
              L"",
              backing,
              utf8_to_wide(vg.vg_uuid),
          };
          add_list_row(app, {NodeKind::Vg, i}, cols);
        }
      }
      break;
    }
    case NodeKind::Vg: {
      for (std::size_t i = 0; i < app.topo.lvs.size(); ++i) {
        if (app.topo.lvs[i].vg_name == app.selected_tree_key) {
          append_lv(i);
        }
      }
      break;
    }
    case NodeKind::Lv: {
      for (std::size_t i = 0; i < app.topo.lvs.size(); ++i) {
        if (app.topo.lvs[i].lv_uuid == app.selected_tree_key) {
          append_lv(i);
          break;
        }
      }
      break;
    }
  }

  if (!app.list_rows.empty()) {
    ListView_SetItemState(app.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  }
  SendMessageW(app.list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(app.list, nullptr, TRUE);
  update_toolbar_states(app);
}

HTREEITEM insert_tree_item(AppState& app, HTREEITEM parent, const std::wstring& text,
                           const TreeTag& tag, HTREEITEM& match_out) {
  const std::size_t tag_idx = app.tree_tags.size();
  app.tree_tags.push_back(tag);

  TVINSERTSTRUCTW ins{};
  ins.hParent = parent;
  ins.hInsertAfter = TVI_LAST;
  ins.item.mask = TVIF_TEXT | TVIF_PARAM;
  ins.item.pszText = const_cast<LPWSTR>(text.c_str());
  ins.item.lParam = static_cast<LPARAM>(tag_idx);
  HTREEITEM item = TreeView_InsertItem(app.tree, &ins);
  if (tag.kind == app.selected_tree_kind && tag.key == app.selected_tree_key) {
    match_out = item;
  }
  return item;
}

void populate_tree_view(AppState& app) {
  app.suppress_tree_sel_change = true;
  SendMessageW(app.tree, WM_SETREDRAW, FALSE, 0);
  TreeView_DeleteAllItems(app.tree);
  app.tree_tags.clear();

  HTREEITEM restore_sel = nullptr;

  HTREEITEM h_disks = insert_tree_item(app, TVI_ROOT, L"Disks & WinLUKS Volumes",
                                       {NodeKind::RootDisks, "", ""}, restore_sel);
  for (const auto& d : app.topo.disks) {
    std::wstring label = utf8_to_wide(d.device_path);
    if (d.kind == "winluks") label += L" (WinLUKS)";
    insert_tree_item(app, h_disks, label, {NodeKind::Disk, d.device_path, d.vg_name}, restore_sel);
  }

  HTREEITEM h_pvs = insert_tree_item(app, TVI_ROOT, L"Physical Volumes",
                                     {NodeKind::RootPvs, "", ""}, restore_sel);
  for (const auto& pv : app.topo.pvs) {
    std::wstring label = utf8_to_wide(pv.device_path.empty() ? pv.pv_key : pv.device_path);
    if (!pv.vg_name.empty()) label += L" [" + utf8_to_wide(pv.vg_name) + L"]";
    insert_tree_item(app, h_pvs, label, {NodeKind::Pv, pv.pv_uuid, pv.vg_name}, restore_sel);
  }

  HTREEITEM h_vgs = insert_tree_item(app, TVI_ROOT, L"Volume Groups",
                                     {NodeKind::RootVgs, "", ""}, restore_sel);
  for (const auto& vg : app.topo.vgs) {
    std::wstring vg_label = utf8_to_wide(vg.name) + L" (" + format_bytes_w(vg.size_bytes) + L")";
    HTREEITEM h_vg = insert_tree_item(app, h_vgs, vg_label, {NodeKind::Vg, vg.name, vg.name},
                                      restore_sel);
    for (const auto& lv : app.topo.lvs) {
      if (lv.vg_name != vg.name) continue;
      if (lv.internal && !app.show_internal_lvs) continue;
      std::wstring lv_label = utf8_to_wide(lv.lv_name);
      if (!lv.drive_letter.empty()) {
        std::wstring d = utf8_to_wide(lv.drive_letter);
        if (!d.empty() && d.back() == L'\\') d.pop_back();
        lv_label += L" (" + d + L")";
      } else if (!lv.active) {
        lv_label += L" [inactive]";
      }
      insert_tree_item(app, h_vg, lv_label, {NodeKind::Lv, lv.lv_uuid, vg.name}, restore_sel);
    }
    TreeView_Expand(app.tree, h_vg, TVE_EXPAND);
  }

  TreeView_Expand(app.tree, h_disks, TVE_EXPAND);
  TreeView_Expand(app.tree, h_pvs, TVE_EXPAND);
  TreeView_Expand(app.tree, h_vgs, TVE_EXPAND);

  if (!restore_sel) {
    restore_sel = h_vgs;
    app.selected_tree_kind = NodeKind::RootVgs;
    app.selected_tree_key.clear();
  }
  TreeView_SelectItem(app.tree, restore_sel);
  app.suppress_tree_sel_change = false;

  SendMessageW(app.tree, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(app.tree, nullptr, TRUE);
}

void update_status_bar(AppState& app) {
  std::wstring p0;
  switch (app.client.mode()) {
    case winlvm::service::ConnectionMode::PipeService:
      p0 = L"Service: Connected (winlvm-svc)";
      break;
    case winlvm::service::ConnectionMode::DirectFallback:
      p0 = L"Service: Direct driver mode";
      break;
    case winlvm::service::ConnectionMode::Disconnected:
      p0 = L"Service: Offline (click Elevate)";
      break;
  }

  std::wstring p1 = app.topo.driver_available ? L"Driver: Online" : L"Driver: Offline";
  p1 += app.client.is_elevated() ? L"  |  Administrator" : L"  |  Standard user (read-only)";

  std::size_t visible_lvs = 0;
  std::size_t active_lvs = 0;
  for (const auto& lv : app.topo.lvs) {
    if (lv.internal) continue;
    ++visible_lvs;
    if (lv.active) ++active_lvs;
  }
  std::wstring p2 = std::to_wstring(app.topo.disks.size()) + L" disks, " +
                    std::to_wstring(app.topo.pvs.size()) + L" PVs, " +
                    std::to_wstring(app.topo.vgs.size()) + L" VGs, " +
                    std::to_wstring(visible_lvs) + L" LVs (" +
                    std::to_wstring(active_lvs) + L" active)";

  SendMessageW(app.status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(p0.c_str()));
  SendMessageW(app.status, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(p1.c_str()));
  SendMessageW(app.status, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(p2.c_str()));

  app.tray.update_tooltip(L"WinLVM Volume Manager (" + std::to_wstring(active_lvs) + L" active)");
}

void update_toolbar_states(AppState& app) {
  const auto* lv = get_selected_lv(app);
  const bool has_lv = (lv != nullptr);
  const bool active = has_lv && lv->active;
  const bool has_drive = active && !lv->drive_letter.empty();
  const bool has_vg = !app.topo.vgs.empty();
  const bool has_pv = !app.topo.pvs.empty();
  const bool elevated = app.client.is_elevated();

  auto set_en = [&](int id, bool en) {
    SendMessageW(app.toolbar, TB_ENABLEBUTTON, static_cast<WPARAM>(id), MAKELONG(en ? TRUE : FALSE, 0));
    HMENU menu = GetMenu(app.hwnd);
    if (menu) {
      EnableMenuItem(menu, static_cast<UINT>(id),
                     static_cast<UINT>(MF_BYCOMMAND | (en ? MF_ENABLED : MF_GRAYED)));
    }
  };

  set_en(IDM_FILE_RESCAN, true);
  set_en(IDM_FILE_ELEVATE, !elevated || app.client.mode() != winlvm::service::ConnectionMode::PipeService);
  set_en(IDM_VOL_ACTIVATE, has_lv && !active && lv->supported);
  set_en(IDM_VOL_DEACTIVATE, active);
  set_en(IDM_VOL_DRIVE_LETTER, active);
  set_en(IDM_VOL_TOGGLE_RO, has_lv);
  set_en(IDM_VOL_OPEN_EXPLORER, has_drive);
  set_en(IDM_VOL_CREATE_PV, !app.topo.disks.empty());
  set_en(IDM_VOL_CREATE_VG, has_pv);
  set_en(IDM_VOL_CREATE_LV, has_vg);
  set_en(IDM_VOL_EXTEND_LV, has_lv && (lv->segment_type == "linear" || lv->segment_type == "striped"));
  set_en(IDM_VOL_RENAME_LV, has_lv);
  set_en(IDM_VOL_DELETE_LV, has_lv);
}

bool ensure_elevated_for_mutation(AppState& app) {
  if (app.client.is_elevated()) return true;
  app.in_modal_dialog = true;
  const int choice = MessageBoxW(
      app.hwnd,
      L"Administrator privileges are required for this operation.\n\n"
      L"Restart WinLVM Volume Manager with administrator privileges?",
      kMainTitle, MB_YESNO | MB_ICONQUESTION);
  app.in_modal_dialog = false;
  if (choice != IDYES) return false;

  std::string err;
  if (winlvm::service::relaunch_current_process_elevated(L"--elevated", err)) {
    app.force_exit = true;
    DestroyWindow(app.hwnd);
  } else {
    MessageBoxW(app.hwnd, utf8_to_wide(err).c_str(), kMainTitle, MB_OK | MB_ICONERROR);
  }
  return false;
}

void report_action_result(AppState& app, const winlvm::service::ActionResponse& resp) {
  if (resp.error == winlvm::service::ErrorCode::AccessDenied && !app.client.is_elevated()) {
    ensure_elevated_for_mutation(app);
    return;
  }
  if (resp.error != winlvm::service::ErrorCode::Ok) {
    const std::wstring msg = utf8_to_wide(
        resp.message.empty() ? std::string(winlvm::service::error_name(resp.error)) : resp.message);
    app.in_modal_dialog = true;
    MessageBoxW(app.hwnd, msg.c_str(), kMainTitle, MB_OK | MB_ICONERROR);
    app.in_modal_dialog = false;
  }
  refresh_topology(app, true);
}

void refresh_topology(AppState& app, bool force_rebuild) {
  winlvm::service::Topology next;
  std::string err;
  const bool got_topology = app.client.get_topology(next, err);
  app.last_error = got_topology ? std::string() : err;

  const std::string encoded = winlvm::service::encode_topology(next);
  if (!force_rebuild && encoded == app.last_topo_json) {
    update_status_bar(app);
    return;
  }
  app.topo = std::move(next);
  app.last_topo_json = encoded;
  populate_tree_view(app);
  populate_list_view(app);
  update_status_bar(app);
}

void open_lv_in_explorer(HWND owner, const std::string& drive_letter) {
  if (drive_letter.empty()) return;
  const std::wstring path = utf8_to_wide(drive_letter);
  ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void show_main_window(AppState& app) {
  ShowWindow(app.hwnd, SW_SHOWNORMAL);
  if (IsIconic(app.hwnd)) ShowWindow(app.hwnd, SW_RESTORE);
  SetForegroundWindow(app.hwnd);
  refresh_topology(app, false);
}

void show_context_menu(AppState& app, POINT screen_pt) {
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  const auto* lv = get_selected_lv(app);
  if (lv) {
    if (lv->active && !lv->drive_letter.empty()) {
      AppendMenuW(menu, MF_STRING, IDM_VOL_OPEN_EXPLORER, L"Open in Explorer");
    }
    if (lv->active) {
      AppendMenuW(menu, MF_STRING, IDM_VOL_DEACTIVATE, L"Deactivate");
      AppendMenuW(menu, MF_STRING, IDM_VOL_DRIVE_LETTER, L"Assign Drive Letter...");
    } else if (lv->supported) {
      AppendMenuW(menu, MF_STRING, IDM_VOL_ACTIVATE, L"Activate");
    }
    AppendMenuW(menu, MF_STRING, IDM_VOL_TOGGLE_RO,
                lv->read_only ? L"Set Read-Write" : L"Set Read-Only");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (lv->segment_type == "linear" || lv->segment_type == "striped") {
      AppendMenuW(menu, MF_STRING, IDM_VOL_EXTEND_LV, L"Extend LV...");
    }
    AppendMenuW(menu, MF_STRING, IDM_VOL_RENAME_LV, L"Rename LV...");
    AppendMenuW(menu, MF_STRING, IDM_VOL_DELETE_LV, L"Delete LV...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  }
  AppendMenuW(menu, MF_STRING, IDM_VOL_CREATE_PV, L"Create PV...");
  AppendMenuW(menu, MF_STRING, IDM_VOL_CREATE_VG, L"Create VG...");
  AppendMenuW(menu, MF_STRING, IDM_VOL_CREATE_LV, L"Create LV...");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, IDM_FILE_RESCAN, L"Rescan");

  TrackPopupMenu(menu, TPM_RIGHTBUTTON, screen_pt.x, screen_pt.y, 0, app.hwnd, nullptr);
  DestroyMenu(menu);
}

void handle_command(AppState& app, UINT cmd) {
  // Handle dynamic tray LV entries first.
  if (cmd >= IDM_TRAY_LV_OPEN_BASE && cmd < IDM_TRAY_LV_OPEN_BASE + app.tray_lvs.size()) {
    const auto& entry = app.tray_lvs[cmd - IDM_TRAY_LV_OPEN_BASE];
    open_lv_in_explorer(app.hwnd, entry.drive_letter);
    return;
  }
  if (cmd >= IDM_TRAY_LV_ACT_BASE && cmd < IDM_TRAY_LV_ACT_BASE + app.tray_lvs.size()) {
    if (!ensure_elevated_for_mutation(app)) return;
    const auto& entry = app.tray_lvs[cmd - IDM_TRAY_LV_ACT_BASE];
    report_action_result(app, app.client.activate_lv({entry.vg_name, entry.lv_name, entry.lv_uuid}));
    return;
  }
  if (cmd >= IDM_TRAY_LV_DEACT_BASE && cmd < IDM_TRAY_LV_DEACT_BASE + app.tray_lvs.size()) {
    if (!ensure_elevated_for_mutation(app)) return;
    const auto& entry = app.tray_lvs[cmd - IDM_TRAY_LV_DEACT_BASE];
    report_action_result(app, app.client.deactivate_lv({entry.vg_name, entry.lv_name, entry.lv_uuid}));
    return;
  }

  switch (cmd) {
    case IDM_TRAY_OPEN_MANAGER:
      show_main_window(app);
      break;
    case IDM_FILE_HIDE_TRAY:
      ShowWindow(app.hwnd, SW_HIDE);
      break;
    case IDM_FILE_EXIT:
    case IDM_TRAY_EXIT:
      app.force_exit = true;
      DestroyWindow(app.hwnd);
      break;
    case IDM_VIEW_REFRESH:
      refresh_topology(app, true);
      break;
    case IDM_VIEW_SHOW_INTERNAL: {
      app.show_internal_lvs = !app.show_internal_lvs;
      if (HMENU menu = GetMenu(app.hwnd)) {
        CheckMenuItem(menu, IDM_VIEW_SHOW_INTERNAL,
                      static_cast<UINT>(MF_BYCOMMAND |
                                        (app.show_internal_lvs ? MF_CHECKED : MF_UNCHECKED)));
      }
      refresh_topology(app, true);
      break;
    }
    case IDM_FILE_RESCAN:
    case IDM_TRAY_RESCAN: {
      const auto resp = app.client.rescan();
      if (resp.error != winlvm::service::ErrorCode::Ok &&
          resp.error != winlvm::service::ErrorCode::AccessDenied) {
        report_action_result(app, resp);
      } else if (resp.error == winlvm::service::ErrorCode::AccessDenied) {
        ensure_elevated_for_mutation(app);
      } else {
        refresh_topology(app, true);
      }
      break;
    }
    case IDM_FILE_ELEVATE: {
      if (!app.client.is_elevated()) {
        std::string err;
        if (winlvm::service::relaunch_current_process_elevated(L"--elevated", err)) {
          app.force_exit = true;
          DestroyWindow(app.hwnd);
        } else {
          MessageBoxW(app.hwnd, utf8_to_wide(err).c_str(), kMainTitle, MB_OK | MB_ICONERROR);
        }
      } else {
        std::string err;
        if (!winlvm::service::try_start_winlvm_service(err)) {
          MessageBoxW(app.hwnd, utf8_to_wide(err).c_str(), kMainTitle, MB_OK | MB_ICONWARNING);
        }
        refresh_topology(app, true);
      }
      break;
    }
    case IDM_VOL_OPEN_EXPLORER:
      if (const auto* lv = get_selected_lv(app)) {
        open_lv_in_explorer(app.hwnd, lv->drive_letter);
      }
      break;
    case IDM_VOL_ACTIVATE:
      if (const auto* lv = get_selected_lv(app)) {
        if (!ensure_elevated_for_mutation(app)) return;
        report_action_result(app, app.client.activate_lv({lv->vg_name, lv->lv_name, lv->lv_uuid}));
      }
      break;
    case IDM_VOL_DEACTIVATE:
      if (const auto* lv = get_selected_lv(app)) {
        if (!ensure_elevated_for_mutation(app)) return;
        report_action_result(app, app.client.deactivate_lv({lv->vg_name, lv->lv_name, lv->lv_uuid}));
      }
      break;
    case IDM_VOL_DRIVE_LETTER:
      if (const auto* lv = get_selected_lv(app)) {
        if (!ensure_elevated_for_mutation(app)) return;
        winlvm::service::SetDriveLetterRequest req;
        app.in_modal_dialog = true;
        const bool ok = winlvm::gui::show_drive_letter_dialog(app.hwnd, *lv, req);
        app.in_modal_dialog = false;
        if (ok) report_action_result(app, app.client.set_drive_letter(req));
      }
      break;
    case IDM_VOL_TOGGLE_RO:
      if (const auto* lv = get_selected_lv(app)) {
        if (!ensure_elevated_for_mutation(app)) return;
        winlvm::service::SetLvReadOnlyRequest req{lv->vg_name, lv->lv_name, lv->lv_uuid, !lv->read_only};
        report_action_result(app, app.client.set_lv_read_only(req));
      }
      break;
    case IDM_VOL_CREATE_PV: {
      if (!ensure_elevated_for_mutation(app)) return;
      winlvm::service::CreatePvRequest req;
      app.in_modal_dialog = true;
      const bool ok = winlvm::gui::show_create_pv_dialog(app.hwnd, app.topo, get_selected_disk_path(app), req);
      app.in_modal_dialog = false;
      if (ok) report_action_result(app, app.client.create_pv(req));
      break;
    }
    case IDM_VOL_CREATE_VG: {
      if (!ensure_elevated_for_mutation(app)) return;
      winlvm::service::CreateVgRequest req;
      app.in_modal_dialog = true;
      const bool ok = winlvm::gui::show_create_vg_dialog(app.hwnd, app.topo, req);
      app.in_modal_dialog = false;
      if (ok) report_action_result(app, app.client.create_vg(req));
      break;
    }
    case IDM_VOL_CREATE_LV: {
      if (!ensure_elevated_for_mutation(app)) return;
      winlvm::service::CreateLvRequest req;
      app.in_modal_dialog = true;
      const bool ok = winlvm::gui::show_create_lv_dialog(app.hwnd, app.topo, get_selected_vg_name(app), req);
      app.in_modal_dialog = false;
      if (ok) report_action_result(app, app.client.create_lv(req));
      break;
    }
    case IDM_VOL_EXTEND_LV:
      if (const auto* lv = get_selected_lv(app)) {
        if (!ensure_elevated_for_mutation(app)) return;
        winlvm::service::ResizeLvRequest req;
        app.in_modal_dialog = true;
        const bool ok = winlvm::gui::show_extend_lv_dialog(app.hwnd, app.topo, *lv, req);
        app.in_modal_dialog = false;
        if (ok) report_action_result(app, app.client.resize_lv(req));
      }
      break;
    case IDM_VOL_RENAME_LV:
      if (const auto* lv = get_selected_lv(app)) {
        if (!ensure_elevated_for_mutation(app)) return;
        winlvm::service::RenameLvRequest req;
        app.in_modal_dialog = true;
        const bool ok = winlvm::gui::show_rename_lv_dialog(app.hwnd, *lv, req);
        app.in_modal_dialog = false;
        if (ok) report_action_result(app, app.client.rename_lv(req));
      }
      break;
    case IDM_VOL_DELETE_LV:
      if (const auto* lv = get_selected_lv(app)) {
        if (!ensure_elevated_for_mutation(app)) return;
        const std::wstring prompt =
            L"Permanently delete logical volume \"" +
            utf8_to_wide(lv->vg_name + "/" + lv->lv_name) +
            L"\"?\n\nAll data stored on this volume will be lost.";
        app.in_modal_dialog = true;
        const int ans = MessageBoxW(app.hwnd, prompt.c_str(), kMainTitle,
                                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        app.in_modal_dialog = false;
        if (ans == IDYES) {
          report_action_result(app, app.client.remove_lv({lv->vg_name, lv->lv_name, lv->lv_uuid}));
        }
      }
      break;
  }
}

HMENU build_main_menu() {
  HMENU bar = CreateMenu();
  HMENU file = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, IDM_FILE_RESCAN, L"&Rescan Volumes\tCtrl+R");
  AppendMenuW(file, MF_STRING, IDM_FILE_ELEVATE, L"&Elevate / Start Service");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, IDM_FILE_HIDE_TRAY, L"&Minimize to Tray");
  AppendMenuW(file, MF_STRING, IDM_FILE_EXIT, L"E&xit");

  HMENU vol = CreatePopupMenu();
  AppendMenuW(vol, MF_STRING, IDM_VOL_ACTIVATE, L"&Activate");
  AppendMenuW(vol, MF_STRING, IDM_VOL_DEACTIVATE, L"&Deactivate");
  AppendMenuW(vol, MF_STRING, IDM_VOL_DRIVE_LETTER, L"Assign &Drive Letter...");
  AppendMenuW(vol, MF_STRING, IDM_VOL_TOGGLE_RO, L"Toggle &Read-Only / Read-Write");
  AppendMenuW(vol, MF_STRING, IDM_VOL_OPEN_EXPLORER, L"&Open in Explorer");
  AppendMenuW(vol, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(vol, MF_STRING, IDM_VOL_CREATE_PV, L"Create &PV...");
  AppendMenuW(vol, MF_STRING, IDM_VOL_CREATE_VG, L"Create V&G...");
  AppendMenuW(vol, MF_STRING, IDM_VOL_CREATE_LV, L"Create &LV...");
  AppendMenuW(vol, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(vol, MF_STRING, IDM_VOL_EXTEND_LV, L"&Extend LV...");
  AppendMenuW(vol, MF_STRING, IDM_VOL_RENAME_LV, L"Re&name LV...");
  AppendMenuW(vol, MF_STRING, IDM_VOL_DELETE_LV, L"De&lete LV...");

  HMENU view = CreatePopupMenu();
  AppendMenuW(view, MF_STRING, IDM_VIEW_REFRESH, L"&Refresh\tF5");
  AppendMenuW(view, MF_STRING, IDM_VIEW_SHOW_INTERNAL, L"Show &Internal Sub-Volumes");

  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(vol), L"&Volume");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
  return bar;
}

bool create_child_controls(AppState& app) {
  app.toolbar = CreateWindowExW(
      0, TOOLBARCLASSNAMEW, nullptr,
      WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_WRAPABLE | CCS_TOP,
      0, 0, 0, 0, app.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TOOLBAR)),
      app.hinst, nullptr);
  if (!app.toolbar) return false;
  SendMessageW(app.toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
  SendMessageW(app.toolbar, TB_SETIMAGELIST, 0, 0);
  SendMessageW(app.toolbar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS);

  struct BtnDef {
    int id;
    const wchar_t* label;
  };
  static constexpr BtnDef kButtons[] = {
      {IDM_FILE_RESCAN, L"Rescan"},
      {0, nullptr},
      {IDM_VOL_ACTIVATE, L"Activate"},
      {IDM_VOL_DEACTIVATE, L"Deactivate"},
      {IDM_VOL_DRIVE_LETTER, L"Drive Letter"},
      {IDM_VOL_TOGGLE_RO, L"RO / RW"},
      {IDM_VOL_OPEN_EXPLORER, L"Open"},
      {0, nullptr},
      {IDM_VOL_CREATE_PV, L"Create PV"},
      {IDM_VOL_CREATE_VG, L"Create VG"},
      {IDM_VOL_CREATE_LV, L"Create LV"},
      {IDM_VOL_EXTEND_LV, L"Extend"},
      {IDM_VOL_RENAME_LV, L"Rename"},
      {IDM_VOL_DELETE_LV, L"Delete"},
      {0, nullptr},
      {IDM_FILE_ELEVATE, L"Elevate"},
  };
  std::vector<TBBUTTON> btns;
  btns.reserve(std::size(kButtons));
  for (const auto& def : kButtons) {
    TBBUTTON b{};
    if (def.id == 0) {
      b.fsStyle = BTNS_SEP;
      b.iBitmap = scale_dpi(8, app.dpi);
    } else {
      b.iBitmap = I_IMAGENONE;
      b.idCommand = def.id;
      b.fsState = TBSTATE_ENABLED;
      b.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT;
      b.iString = reinterpret_cast<INT_PTR>(def.label);
    }
    btns.push_back(b);
  }
  SendMessageW(app.toolbar, TB_ADDBUTTONSW, static_cast<WPARAM>(btns.size()),
               reinterpret_cast<LPARAM>(btns.data()));

  app.tree = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_TREEVIEWW, nullptr,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES | TVS_LINESATROOT |
          TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
      0, 0, 0, 0, app.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TREEVIEW)),
      app.hinst, nullptr);
  if (!app.tree) return false;
  apply_explorer_theme(app.tree);

  app.list = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
      0, 0, 0, 0, app.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LISTVIEW)),
      app.hinst, nullptr);
  if (!app.list) return false;
  ListView_SetExtendedListViewStyle(
      app.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
  apply_explorer_theme(app.list);

  struct ColDef {
    const wchar_t* title;
    int width_96;
  };
  static constexpr ColDef kCols[] = {
      {L"Name", 150},
      {L"Type", 90},
      {L"Capacity", 90},
      {L"Status", 110},
      {L"Access", 60},
      {L"Drive Letter", 125},
      {L"Backing / Pool / Origin", 170},
      {L"UUID", 255},
  };
  for (int i = 0; i < static_cast<int>(std::size(kCols)); ++i) {
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.cx = scale_dpi(kCols[i].width_96, app.dpi);
    col.pszText = const_cast<LPWSTR>(kCols[i].title);
    col.iSubItem = i;
    ListView_InsertColumn(app.list, i, &col);
  }

  app.status = CreateWindowExW(
      0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
      0, 0, 0, 0, app.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUSBAR)),
      app.hinst, nullptr);
  if (!app.status) return false;

  apply_font_to_controls(app);
  layout_controls(app);
  return true;
}

void register_device_notifications(AppState& app) {
  DEV_BROADCAST_DEVICEINTERFACE_W filter{};
  filter.dbcc_size = sizeof(filter);
  filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;

  filter.dbcc_classguid = kGuidDevInterfaceDisk;
  app.notify_disk = RegisterDeviceNotificationW(app.hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

  filter.dbcc_classguid = kGuidDevInterfaceVolume;
  app.notify_vol = RegisterDeviceNotificationW(app.hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
}

LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* app = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (app && app->msg_taskbar_created != 0 && msg == app->msg_taskbar_created) {
    app->tray.install(hwnd, app->icon);
    return 0;
  }

  switch (msg) {
    case WM_CREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      app = static_cast<AppState*>(cs->lpCreateParams);
      app->hwnd = hwnd;
      app->dpi = get_window_dpi(hwnd);
      app->msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
      if (!create_child_controls(*app)) return -1;
      register_device_notifications(*app);
      app->tray.install(hwnd, app->icon);
      SetTimer(hwnd, IDT_POLL_REFRESH, 3000, nullptr);
      return 0;
    }
    case WM_SIZE:
      if (app) {
        if (wp == SIZE_MINIMIZED && !app->smoke_test) {
          ShowWindow(hwnd, SW_HIDE);
          return 0;
        }
        layout_controls(*app);
      }
      return 0;
    case WM_DPICHANGED:
      if (app) {
        app->dpi = HIWORD(wp);
        apply_font_to_controls(*app);
        const auto* suggested = reinterpret_cast<const RECT*>(lp);
        if (suggested) {
          SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left, suggested->bottom - suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        }
        layout_controls(*app);
      }
      return 0;
    case WM_SETCURSOR:
      if (app && LOWORD(lp) == HTCLIENT) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        RECT tree_rc{}, list_rc{};
        GetWindowRect(app->tree, &tree_rc);
        GetWindowRect(app->list, &list_rc);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<LPPOINT>(&tree_rc), 2);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<LPPOINT>(&list_rc), 2);
        if (pt.x >= tree_rc.right && pt.x <= list_rc.left && pt.y >= tree_rc.top && pt.y <= tree_rc.bottom) {
          SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
          return TRUE;
        }
      }
      break;
    case WM_LBUTTONDOWN:
      if (app) {
        const int x = static_cast<short>(LOWORD(lp));
        RECT tree_rc{}, list_rc{};
        GetWindowRect(app->tree, &tree_rc);
        GetWindowRect(app->list, &list_rc);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<LPPOINT>(&tree_rc), 2);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<LPPOINT>(&list_rc), 2);
        if (x >= tree_rc.right - 2 && x <= list_rc.left + 2) {
          app->dragging_splitter = true;
          SetCapture(hwnd);
          return 0;
        }
      }
      break;
    case WM_MOUSEMOVE:
      if (app && app->dragging_splitter) {
        const int x = static_cast<short>(LOWORD(lp));
        app->splitter_x_96 = MulDiv(x, 96, static_cast<int>(app->dpi));
        layout_controls(*app);
        return 0;
      }
      break;
    case WM_LBUTTONUP:
      if (app && app->dragging_splitter) {
        app->dragging_splitter = false;
        ReleaseCapture();
        return 0;
      }
      break;
    case WM_NOTIFY:
      if (app) {
        const auto* hdr = reinterpret_cast<const NMHDR*>(lp);
        if (hdr->idFrom == IDC_TREEVIEW && hdr->code == TVN_SELCHANGEDW) {
          if (app->suppress_tree_sel_change) return 0;
          const auto* nmtv = reinterpret_cast<const NMTREEVIEWW*>(lp);
          if (!nmtv->itemNew.hItem) return 0;
          const std::size_t idx = static_cast<std::size_t>(nmtv->itemNew.lParam);
          if (idx < app->tree_tags.size()) {
            app->selected_tree_kind = app->tree_tags[idx].kind;
            app->selected_tree_key = app->tree_tags[idx].key;
            populate_list_view(*app);
          }
          return 0;
        }
        if (hdr->idFrom == IDC_LISTVIEW) {
          if (hdr->code == LVN_ITEMCHANGED) {
            update_toolbar_states(*app);
            return 0;
          }
          if (hdr->code == NM_DBLCLK) {
            if (const auto* lv = get_selected_lv(*app)) {
              if (lv->active && !lv->drive_letter.empty()) {
                open_lv_in_explorer(hwnd, lv->drive_letter);
              }
            }
            return 0;
          }
          if (hdr->code == NM_RCLICK) {
            POINT pt{};
            GetCursorPos(&pt);
            show_context_menu(*app, pt);
            return 0;
          }
        }
        if (hdr->idFrom == IDC_TREEVIEW && hdr->code == NM_RCLICK) {
          POINT pt{};
          GetCursorPos(&pt);
          POINT client_pt = pt;
          ScreenToClient(app->tree, &client_pt);
          TVHITTESTINFO ht{};
          ht.pt = client_pt;
          if (HTREEITEM hit = TreeView_HitTest(app->tree, &ht)) {
            TreeView_SelectItem(app->tree, hit);
          }
          show_context_menu(*app, pt);
          return 0;
        }
      }
      break;
    case WM_COMMAND:
      if (app) {
        handle_command(*app, LOWORD(wp));
        return 0;
      }
      break;
    case WM_APP_TRAY_ICON:
      if (app) {
        const UINT ev = LOWORD(lp);
        if (ev == WM_LBUTTONDBLCLK) {
          show_main_window(*app);
        } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
          refresh_topology(*app, false);
          app->tray.show_context_menu(hwnd, app->topo, app->tray_lvs);
        }
      }
      return 0;
    case WM_DEVICECHANGE:
      if (app && !app->smoke_test) {
        if (wp == DBT_DEVICEARRIVAL) {
          SetTimer(hwnd, IDT_DEBOUNCE_RESCAN, 800, nullptr);
        } else if (wp == DBT_DEVICEREMOVECOMPLETE || wp == DBT_DEVNODES_CHANGED) {
          if (!app->in_modal_dialog) refresh_topology(*app, false);
        }
      }
      return TRUE;
    case WM_TIMER:
      if (app && !app->smoke_test) {
        if (wp == IDT_POLL_REFRESH) {
          if (IsWindowVisible(hwnd) && !app->in_modal_dialog) {
            refresh_topology(*app, false);
          }
        } else if (wp == IDT_DEBOUNCE_RESCAN) {
          KillTimer(hwnd, IDT_DEBOUNCE_RESCAN);
          if (!app->in_modal_dialog) {
            const auto rescan_resp = app->client.rescan();
            (void)rescan_resp;
            refresh_topology(*app, false);
          }
        }
      }
      return 0;
    case WM_SYSCOMMAND:
      if (app && (wp & 0xFFF0U) == SC_CLOSE && !app->force_exit && !app->smoke_test &&
          app->tray.installed()) {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
      }
      break;
    case WM_APP_FORCE_EXIT:
      if (app) {
        app->force_exit = true;
        DestroyWindow(hwnd);
      }
      return 0;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_ENDSESSION:
      if (app && wp) {
        app->force_exit = true;
        DestroyWindow(hwnd);
      }
      return 0;
    case WM_DESTROY:
      if (app) {
        KillTimer(hwnd, IDT_POLL_REFRESH);
        KillTimer(hwnd, IDT_DEBOUNCE_RESCAN);
        if (app->notify_disk) UnregisterDeviceNotification(app->notify_disk);
        if (app->notify_vol) UnregisterDeviceNotification(app->notify_vol);
        app->tray.remove();
        if (app->ui_font) {
          DeleteObject(app->ui_font);
          app->ui_font = nullptr;
        }
      }
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int show_cmd) {
  winlvm::gui::enable_per_monitor_dpi();

  bool start_tray = false;
  bool elevated_relaunch = false;
  bool smoke_test = false;

  int argc = 0;
  if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    for (int i = 1; i < argc; ++i) {
      if (lstrcmpiW(argv[i], L"--tray") == 0) start_tray = true;
      else if (lstrcmpiW(argv[i], L"--elevated") == 0) elevated_relaunch = true;
      else if (lstrcmpiW(argv[i], L"--smoke-test") == 0) smoke_test = true;
    }
    LocalFree(argv);
  }

  if (!smoke_test) {
    if (HWND existing = FindWindowW(kMainWndClass, nullptr)) {
      if (elevated_relaunch) {
        PostMessageW(existing, WM_APP_FORCE_EXIT, 0, 0);
        Sleep(250);
      } else {
        if (!start_tray) {
          ShowWindow(existing, SW_SHOWNORMAL);
          SetForegroundWindow(existing);
        }
        return 0;
      }
    }
  }

  INITCOMMONCONTROLSEX icc{};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&icc);

  AppState app;
  app.hinst = hinst;
  app.smoke_test = smoke_test;
  app.icon = LoadIconW(hinst, MAKEINTRESOURCEW(IDI_WINLVM_APP));
  if (!app.icon) app.icon = LoadIconW(nullptr, IDI_APPLICATION);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = main_wnd_proc;
  wc.hInstance = hinst;
  wc.hIcon = app.icon;
  wc.hIconSm = app.icon;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = kMainWndClass;
  if (!RegisterClassExW(&wc)) return 1;

  HMENU menu = build_main_menu();
  HWND hwnd = CreateWindowExW(
      0, kMainWndClass, kMainTitle, WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, 1120, 680,
      nullptr, menu, hinst, &app);
  if (!hwnd) {
    if (menu) DestroyMenu(menu);
    return 1;
  }

  // Scale initial window size to monitor DPI.
  const int init_w = scale_dpi(1120, app.dpi);
  const int init_h = scale_dpi(680, app.dpi);
  SetWindowPos(hwnd, nullptr, 0, 0, init_w, init_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

  if (smoke_test) {
    // Verify controls, tray, and live IPC/fallback topology round-trip headlessly.
    if (!app.toolbar || !app.tree || !app.list || !app.status) {
      DestroyWindow(hwnd);
      return 2;
    }
    winlvm::service::Topology topo;
    std::string err;
    const bool ok = app.client.get_topology(topo, err);
    if (!ok && app.client.is_elevated()) {
      DestroyWindow(hwnd);
      return 3;
    }
    app.topo = std::move(topo);
    populate_tree_view(app);
    populate_list_view(app);
    update_status_bar(app);

    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    app.force_exit = true;
    DestroyWindow(hwnd);
    return 0;
  }

  refresh_topology(app, true);
  if (!start_tray) {
    ShowWindow(hwnd, show_cmd == SW_HIDE ? SW_SHOWNORMAL : show_cmd);
    UpdateWindow(hwnd);
  }

  // Keyboard accelerators: F5=Refresh, Ctrl+R=Rescan, F2=Rename, Del=Delete
  ACCEL accels[] = {
      {FVIRTKEY, VK_F5, IDM_VIEW_REFRESH},
      {FVIRTKEY | FCONTROL, 'R', IDM_FILE_RESCAN},
      {FVIRTKEY, VK_F2, IDM_VOL_RENAME_LV},
      {FVIRTKEY, VK_DELETE, IDM_VOL_DELETE_LV},
  };
  HACCEL haccel = CreateAcceleratorTableW(accels, static_cast<int>(std::size(accels)));

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!haccel || !TranslateAcceleratorW(hwnd, haccel, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  if (haccel) DestroyAcceleratorTable(haccel);
  return static_cast<int>(msg.wParam);
}
