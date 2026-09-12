/*
 * winlvm.c — winlvm.sys: DriverEntry, dispatch wiring, and the two
 * kind-discriminating dispatchers (PnP, device control).
 *
 * WDM shape (mirrors maharmstone/winmd; FEASIBILITY.md §5.1): the INF
 * enumerates ROOT\winlvm, PnP creates the root bus PDO and calls our
 * AddDevice. No control device object.
 *
 * NEVER ZwCreateFile: backing devices are addressed through the device
 * objects we already hold (IoGetDeviceObjectPointer in the notify callback,
 * IoCallDriver everywhere).
 */
#include <initguid.h>
#include "winlvm.h"

/* Globals. g_bus_ext is set by AddDevice and cleared on REMOVE; the disk
 * arrival callback may fire before AddDevice, so it must be null-checked. */
WINLVM_BUS_EXT *g_bus_ext = NULL;
PVOID           g_disk_notify_cookie = NULL;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = WinLvmCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = WinLvmCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = WinLvmCreateClose;
    DriverObject->MajorFunction[IRP_MJ_READ]           = WinLvmRead;
    DriverObject->MajorFunction[IRP_MJ_WRITE]          = WinLvmWrite;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS]  = WinLvmFlush;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN]       = WinLvmShutdown;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = WinLvmDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = WinLvmSystemControl;
    DriverObject->MajorFunction[IRP_MJ_PNP]            = WinLvmPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER]          = WinLvmPower;
    DriverObject->DriverExtension->AddDevice           = WinLvmAddDevice;
    DriverObject->DriverUnload                         = WinLvmUnload;

    /* PnP registration happens at bus START (WinLvmPnp IRP_MN_START_DEVICE),
     * not here: existing-disk callbacks fired in DriverEntry would race
     * ahead of AddDevice and see g_bus_ext == NULL, so their probes would
     * be dropped and the LVs never enumerated. */
    WL_TRACE("DriverEntry");
    return STATUS_SUCCESS;
}

void WinLvmUnload(PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);
    if (g_disk_notify_cookie) {
        IoUnregisterPlugPlayNotification(g_disk_notify_cookie);
        g_disk_notify_cookie = NULL;
    }
    /* Child PDOs and VG records are torn down by IRP_MN_REMOVE_DEVICE /
     * IRP_MN_SURPRISE_REMOVAL on the bus FDO; DriverUnload runs only after
     * the last device stack is gone. */
}

NTSTATUS WinLvmCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* Verifier 0x21F root cause (dedicated.dmp, 2026-09-12): VfWmiTestStartedPdoStack
 * sends WMI IRPs at every started devnode; a NULL IRP_MJ_SYSTEM_CONTROL slot in
 * the dispatch table is a bugcheck. We register no WMI guidance. Contract
 * (grounded: ReactOS usbport PDO path + winbtrfs drv_system_control + classpnp
 * ClassSystemControl): FDO forwards WMI to its lower driver; a PDO at the
 * bottom of its stack completes with the IoStatus PnP shipped the IRP with
 * (NOT_SUPPORTED) - overwriting it (0x220: "completed by someone other than
 * the ProviderId") breaks the WMI manager's provider bookkeeping. */
NTSTATUS WinLvmSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    if (kind == WL_EXT_BUS) {
        WINLVM_BUS_EXT *bus = (WINLVM_BUS_EXT *)DeviceObject->DeviceExtension;
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(bus->bus_pdo, Irp);
    }
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}

/* PnP discriminator: the bus FDO half lives in winlvm_bus.c, the LV PDO
 * half in winlvm_child.c. Extension kind is at offset 0 in both. */
NTSTATUS WinLvmPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    if (kind == WL_EXT_BUS)
        return WinLvmBusPnp(DeviceObject, Irp);
    return WinLvmChildPnp(DeviceObject, Irp);
}

NTSTATUS WinLvmDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    if (kind != WL_EXT_LV) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    WINLVM_LV_EXT *lv = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    Irp->IoStatus.Status = WinLvmMountIoctl(DeviceObject, Irp, lv);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}
