/*
 * winlvm_power.c — WDM power IRP dispatch for control, bus FDO, and LV PDOs.
 */
#include "winlvm.h"

NTSTATUS WinLvmPower(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    UCHAR minor = irpSp->MinorFunction;

    if (kind == WL_EXT_BUS) {
        WINLVM_BUS_EXT *ext = (WINLVM_BUS_EXT *)DeviceObject->DeviceExtension;
        if (minor == IRP_MN_SET_POWER || minor == IRP_MN_QUERY_POWER) {
            Irp->IoStatus.Status = STATUS_SUCCESS;
        }
        PoStartNextPowerIrp(Irp);
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(ext->bus_pdo, Irp);
    }

    if (kind == WL_EXT_CONTROL || kind == WL_EXT_LV) {
        if (minor == IRP_MN_SET_POWER || minor == IRP_MN_QUERY_POWER) {
            Irp->IoStatus.Status = STATUS_SUCCESS;
        }
        NTSTATUS st = Irp->IoStatus.Status;
        PoStartNextPowerIrp(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return st;
    }

    NTSTATUS st = Irp->IoStatus.Status;
    PoStartNextPowerIrp(Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}
