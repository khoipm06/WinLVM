#pragma once

#include <string>
#include <vector>

#include "winlvm/result.h"

namespace winlvm {

enum class VolumeLockMode {
  kReadOnlyShared,
  kExclusiveReadWrite,
};

struct VolumeLockRequest {
  std::vector<std::string> device_paths;
  VolumeLockMode mode = VolumeLockMode::kReadOnlyShared;
  bool allow_mounted_volumes = false;
};

struct VolumeLockHandle {
  std::string device_path;
  bool locked = false;
  bool dismounted = false;
};

struct VolumeLockSet {
  std::vector<VolumeLockHandle> handles;
};

Result<VolumeLockSet> acquire_volume_locks(const VolumeLockRequest& request);
Status release_volume_locks(VolumeLockSet& locks);
Status flush_volume_locks(const VolumeLockSet& locks);

}  // namespace winlvm
