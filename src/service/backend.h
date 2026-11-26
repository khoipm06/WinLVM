#ifndef WINLVM_SERVICE_BACKEND_H
#define WINLVM_SERVICE_BACKEND_H

#include <string>
#include <string_view>

#include "protocol.h"

namespace winlvm::service {

// Live Windows storage/driver control backend used both by winlvm_svc.exe
// and by winlvm_gui.exe's elevated direct-fallback mode.
[[nodiscard]] bool is_driver_available();
[[nodiscard]] Topology query_live_topology(bool service_connected, bool caller_elevated);

[[nodiscard]] ActionResponse op_rescan();
[[nodiscard]] ActionResponse op_activate_lv(const LvTargetRequest& req);
[[nodiscard]] ActionResponse op_deactivate_lv(const LvTargetRequest& req);
[[nodiscard]] ActionResponse op_set_drive_letter(const SetDriveLetterRequest& req);
[[nodiscard]] ActionResponse op_set_lv_read_only(const SetLvReadOnlyRequest& req);
[[nodiscard]] ActionResponse op_create_pv(const CreatePvRequest& req);
[[nodiscard]] ActionResponse op_create_vg(const CreateVgRequest& req);
[[nodiscard]] ActionResponse op_create_lv(const CreateLvRequest& req);
[[nodiscard]] ActionResponse op_resize_lv(const ResizeLvRequest& req);
[[nodiscard]] ActionResponse op_rename_lv(const RenameLvRequest& req);
[[nodiscard]] ActionResponse op_remove_lv(const LvTargetRequest& req);

// Validates policy/schema and executes a decoded protocol request, producing
// the response ErrorCode and UTF-8 JSON payload.
void dispatch_operation(const Request& request,
                        std::string_view request_payload,
                        bool caller_elevated,
                        bool service_connected,
                        ErrorCode& out_error,
                        std::string& out_payload);

}  // namespace winlvm::service
#endif
