#ifndef WINLVM_GUI_TRAY_H
#define WINLVM_GUI_TRAY_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <vector>

#include "../service/protocol.h"

namespace winlvm::gui {

struct TrayLvEntry {
  std::string vg_name;
  std::string lv_name;
  std::string lv_uuid;
  std::string drive_letter;
  bool active = false;
};

class SystemTray {
 public:
  bool install(HWND owner, HICON icon);
  void remove();
  void update_tooltip(const std::wstring& tip);
  void show_context_menu(HWND owner, const winlvm::service::Topology& topo,
                         std::vector<TrayLvEntry>& out_entries);
  [[nodiscard]] bool installed() const noexcept { return installed_; }

 private:
  NOTIFYICONDATAW nid_{};
  bool installed_ = false;
};

}  // namespace winlvm::gui
#endif
