#ifndef WINLVM_SERVICE_IPC_CLIENT_H
#define WINLVM_SERVICE_IPC_CLIENT_H

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "protocol.h"

namespace winlvm::service {

enum class ConnectionMode {
  Disconnected = 0,
  PipeService = 1,
  DirectFallback = 2,
};

[[nodiscard]] bool is_current_process_elevated();
[[nodiscard]] bool try_start_winlvm_service(std::string& error_out);
[[nodiscard]] bool relaunch_current_process_elevated(const std::wstring& extra_args,
                                                     std::string& error_out);

class ServiceClient {
 public:
  ServiceClient() = default;

  [[nodiscard]] ConnectionMode mode() const noexcept { return mode_; }
  [[nodiscard]] bool is_elevated() const noexcept { return is_current_process_elevated(); }

  [[nodiscard]] bool get_topology(Topology& out, std::string& error_out);
  [[nodiscard]] ActionResponse rescan();
  [[nodiscard]] ActionResponse activate_lv(const LvTargetRequest& req);
  [[nodiscard]] ActionResponse deactivate_lv(const LvTargetRequest& req);
  [[nodiscard]] ActionResponse set_drive_letter(const SetDriveLetterRequest& req);
  [[nodiscard]] ActionResponse set_lv_read_only(const SetLvReadOnlyRequest& req);
  [[nodiscard]] ActionResponse create_pv(const CreatePvRequest& req);
  [[nodiscard]] ActionResponse create_vg(const CreateVgRequest& req);
  [[nodiscard]] ActionResponse create_lv(const CreateLvRequest& req);
  [[nodiscard]] ActionResponse resize_lv(const ResizeLvRequest& req);
  [[nodiscard]] ActionResponse rename_lv(const RenameLvRequest& req);
  [[nodiscard]] ActionResponse remove_lv(const LvTargetRequest& req);

 private:
  ActionResponse call_action(Operation op, std::string_view payload);
  bool send_pipe_roundtrip(Operation op,
                           std::string_view req_payload,
                           ErrorCode& resp_error,
                           std::string& resp_payload,
                           std::string& transport_error);

  ConnectionMode mode_ = ConnectionMode::Disconnected;
  bool attempted_scm_start_ = false;
  std::atomic<std::uint64_t> next_request_id_{1};
};

}  // namespace winlvm::service
#endif
