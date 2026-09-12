/* winlvm_power.c — power dispatch.
 *
 * Power IRPs: LV PDOs are leaf devices (complete immediately); the bus
 * FDO forwards down the root stack. Extension kind is at offset 0 in both.
 *
 * Moved verbatim from winlvm.c (2026-09-12 module split).
 */
#include "winlvm.h"

NTSTATUS WinLvmPower(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    if (kind == WL_EXT_LV) {
        PoStartNextPowerIrp(Irp);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
    WINLVM_BUS_EXT *ext = (WINLVM_BUS_EXT *)DeviceObject->DeviceExtension;
    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(ext->bus_pdo, Irp);
}
