#ifndef WINLVM_GUI_DIALOGS_H
#define WINLVM_GUI_DIALOGS_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

#include "../service/protocol.h"

namespace winlvm::gui {

[[nodiscard]] bool show_drive_letter_dialog(HWND owner,
                                            const winlvm::service::LvInfo& lv,
                                            winlvm::service::SetDriveLetterRequest& out_req);

[[nodiscard]] bool show_create_pv_dialog(HWND owner,
                                         const winlvm::service::Topology& topo,
                                         const std::string& default_device,
                                         winlvm::service::CreatePvRequest& out_req);

[[nodiscard]] bool show_create_vg_dialog(HWND owner,
                                         const winlvm::service::Topology& topo,
                                         winlvm::service::CreateVgRequest& out_req);

[[nodiscard]] bool show_create_lv_dialog(HWND owner,
                                         const winlvm::service::Topology& topo,
                                         const std::string& default_vg,
                                         winlvm::service::CreateLvRequest& out_req);

[[nodiscard]] bool show_extend_lv_dialog(HWND owner,
                                         const winlvm::service::Topology& topo,
                                         const winlvm::service::LvInfo& lv,
                                         winlvm::service::ResizeLvRequest& out_req);

[[nodiscard]] bool show_rename_lv_dialog(HWND owner,
                                         const winlvm::service::LvInfo& lv,
                                         winlvm::service::RenameLvRequest& out_req);

}  // namespace winlvm::gui
#endif
