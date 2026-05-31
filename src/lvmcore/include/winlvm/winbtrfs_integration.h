#pragma once

#include <string>

#include "winlvm/model.h"

namespace winlvm {

struct VirtualDeviceIdentity {
  std::string vendor;
  std::string product;
  std::string revision;
  std::string serial;
  std::string stable_instance_id;
};

VirtualDeviceIdentity make_virtual_device_identity(
    const VolumeGroup& vg,
    const LogicalVolume& lv);

}  // namespace winlvm
