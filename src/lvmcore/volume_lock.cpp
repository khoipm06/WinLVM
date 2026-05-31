#include "winlvm/volume_lock.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#endif

namespace winlvm {

Result<VolumeLockSet> acquire_volume_locks(const VolumeLockRequest& request) {
  VolumeLockSet lock_set;

  for (const std::string& device_path : request.device_paths) {
    VolumeLockHandle handle;
    handle.device_path = device_path;

    // v1 policy: read-only shared scans do not lock; RW activations require future
    // service-mediated exclusive handles before kernel driver write enable.
    if (request.mode == VolumeLockMode::kExclusiveReadWrite) {
      return Result<VolumeLockSet>::failure(
          "exclusive read-write locking is not enabled until kernel write path is implemented");
    }

    lock_set.handles.push_back(handle);
  }

  return Result<VolumeLockSet>::success(lock_set);
}

Status release_volume_locks(VolumeLockSet& locks) {
  locks.handles.clear();
  return Status::success();
}

Status flush_volume_locks(const VolumeLockSet& locks) {
  (void)locks;
  // Read-only placeholder: future RW path must flush every backing PV handle.
  return Status::success();
}

}  // namespace winlvm
