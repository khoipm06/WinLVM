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

/* Verifier 0x220 saga (dedicated.dmp sequence, 2026-09-12): VfWmiTestStartedPdoStack
 * sends WMI IRPs at every started devnode.
 *   - 0x21F: NULL IRP_MJ_SYSTEM_CONTROL dispatch slot → wire a handler.
 *   - v17 0x220: bus FDO forwarded the probe down to PnpManager's root PDO —
 *     the completing driver was outside the ProviderId's stack.
 *   - v18 0x220: bus FDO completed the probe in-driver with no WMI
 *     registration — still not the recognized WMI provider for the LV PDO.
 * Root asymmetry (Oracle verdict, 2026-09-12): the verifier probe's ProviderId
 * is a device object outside the bus stack — the LV child PDO — and with no
 * IoWMIRegistrationControl anywhere, no in-stack routing can ever satisfy it.
 * Decisive fix (H5, classpnp/Toaster pattern): register a zero-GUID
 * WMILIB_CONTEXT on BOTH the bus FDO and every LV child PDO at START and
 * dispatch through WmiSystemControl, so the WMI core has a real "who owns WMI
 * here" answer and routes ProviderId-addressed probes to the LV PDO's own
 * stack, where the PDO (bottom of stack) legitimately completes them. */
NTSTATUS WinLvmQueryWmiRegInfo(PDEVICE_OBJECT DeviceObject, PULONG RegFlags,
                               PUNICODE_STRING InstanceName,
                               PUNICODE_STRING *RegistryPath,
                               PUNICODE_STRING MofResourceName,
                               PDEVICE_OBJECT *Pdo) {
    /* WDK 26100 WMI_QUERY_REGINFO_CALLBACK: GuidCount/GuidList are
     * WMILIB_CONTEXT fields, not callback params (the v18 signature was a
     * silent ABI mismatch). InstanceName and MofResourceName are only
     * consumed when INSTANCE_PDO / a MOF resource are absent — leave intact. */
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    if (kind != WL_EXT_BUS && kind != WL_EXT_LV) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    *RegFlags = WMIREG_FLAG_INSTANCE_PDO;
    *RegistryPath = NULL;   /* no registry-backed WMI data; SAL maybenull */
    *Pdo = (kind == WL_EXT_BUS)
        ? ((WINLVM_BUS_EXT *)DeviceObject->DeviceExtension)->pdo
        : DeviceObject;
    return STATUS_SUCCESS;
}

void WinLvmWmiInit(PDEVICE_OBJECT DeviceObject) {
    /* Caller (START_DEVICE) runs at PASSIVE_LEVEL — IoWMIRegistrationControl
     * requirement. Failure is nonfatal: registering is what makes the WMI
     * core hold a provider entry for this device (Oracle H5); if it fails
     * the stack still functions, only verifier probes stay unsatisfiable. */
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    WMILIB_CONTEXT *wmi = (kind == WL_EXT_BUS)
        ? &((WINLVM_BUS_EXT *)DeviceObject->DeviceExtension)->wmi
        : &((WINLVM_LV_EXT *)DeviceObject->DeviceExtension)->wmi;

    RtlZeroMemory(wmi, sizeof(*wmi));
    wmi->QueryWmiRegInfo = WinLvmQueryWmiRegInfo;

    NTSTATUS st = IoWMIRegistrationControl(DeviceObject, WMIREG_ACTION_REGISTER);
    WL_TRACE_1("wmi register 0x%x", (unsigned)st);
}

NTSTATUS WinLvmSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    if (kind != WL_EXT_BUS && kind != WL_EXT_LV) {
        /* R5 fail-closed: kind must be set before any IRP can arrive. */
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    WMILIB_CONTEXT *wmi = (kind == WL_EXT_BUS)
        ? &((WINLVM_BUS_EXT *)DeviceObject->DeviceExtension)->wmi
        : &((WINLVM_LV_EXT *)DeviceObject->DeviceExtension)->wmi;

    NTSTATUS status = STATUS_SUCCESS;
    SYSCTL_IRP_DISPOSITION disposition =
        WmiSystemControl(wmi, DeviceObject, Irp, &status);

    switch (disposition) {
    case IrpProcessed:
        /* Toaster canonical: IRP already completed by wmilib/DpWmiXxx —
         * completing again = double-complete crash. Do nothing. */
        return status;
    case IrpNotCompleted:
        /* Wmilib filled IoStatus but left completion to us. */
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    case IrpForward:
    case IrpNotWmi: {
        if (kind == WL_EXT_BUS) {
            /* Not our block: classpnp contract hands the IRP down. */
            WINLVM_BUS_EXT *ext = (WINLVM_BUS_EXT *)DeviceObject->DeviceExtension;
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(ext->bus_pdo, Irp);
        }
        /* LV PDO is the bottom of its own stack and, once registered, the
         * ProviderId the verifier probes — completing here is legitimate. */
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }
    default:
        /* Fail closed on any disposition we do not recognize. */
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
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
