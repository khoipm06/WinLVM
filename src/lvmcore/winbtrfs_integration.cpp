#include "winlvm/winbtrfs_integration.h"

namespace winlvm {

VirtualDeviceIdentity make_virtual_device_identity(
    const VolumeGroup& vg,
    const LogicalVolume& lv) {
  VirtualDeviceIdentity identity;
  identity.vendor = "LVM2";
  identity.product = "LogicalVolume";
  identity.revision = "0.1";
  identity.serial = "LVM-" + vg.uuid + "-" + lv.uuid;
  identity.stable_instance_id = "winlvm\\" + vg.uuid + "\\" + lv.uuid;
  return identity;
}

}  // namespace winlvm
