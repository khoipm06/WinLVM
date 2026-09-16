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
PDEVICE_OBJECT  g_control_device = NULL;
BOOLEAN         g_control_link_created = FALSE;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNICODE_STRING device_name;
    UNICODE_STRING dos_name;
    NTSTATUS status;

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

    RtlInitUnicodeString(&device_name, WINLVM_CONTROL_DEVICE_NAME);
    status = IoCreateDevice(DriverObject, sizeof(WINLVM_CONTROL_EXT), &device_name,
                            FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE,
                            &g_control_device);
    if (!NT_SUCCESS(status)) {
        g_control_device = NULL;
        return status;
    }
    ((WINLVM_CONTROL_EXT *)g_control_device->DeviceExtension)->kind = WL_EXT_CONTROL;

    RtlInitUnicodeString(&dos_name, WINLVM_CONTROL_DOS_NAME);
    status = IoCreateSymbolicLink(&dos_name, &device_name);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(g_control_device);
        g_control_device = NULL;
        return status;
    }
    g_control_link_created = TRUE;
    g_control_device->Flags &= ~DO_DEVICE_INITIALIZING;

    /* PnP registration happens at bus START (WinLvmPnp IRP_MN_START_DEVICE),
     * not here: existing-disk callbacks fired in DriverEntry would race
     * ahead of AddDevice and see g_bus_ext == NULL, so their probes would
     * be dropped and the LVs never enumerated. */
    WL_TRACE("DriverEntry");
    return STATUS_SUCCESS;
}

void WinLvmUnload(PDRIVER_OBJECT DriverObject) {
    UNICODE_STRING dos_name;

    UNREFERENCED_PARAMETER(DriverObject);
    RtlInitUnicodeString(&dos_name, WINLVM_CONTROL_DOS_NAME);
    if (g_control_link_created) {
        IoDeleteSymbolicLink(&dos_name);
        g_control_link_created = FALSE;
    }
    if (g_control_device) {
        IoDeleteDevice(g_control_device);
        g_control_device = NULL;
    }
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

static NTSTATUS WinLvmValidateUpdateLv(const WINLVM_UPDATE_LV_REQUEST *request,
                                       ULONG input_length) {
    if (!request || input_length < sizeof(*request) ||
        request->header.size < sizeof(*request) || request->header.version != 1)
        return STATUS_INVALID_PARAMETER;
    if (request->lv_uuid[WL_VG_UUID_LEN - 1] != '\0')
        return STATUS_INVALID_PARAMETER;
    if (request->byte_size == 0 || request->logical_sector < 512 ||
        request->physical_sector < request->logical_sector ||
        (request->logical_sector & (request->logical_sector - 1)) != 0 ||
        (request->physical_sector & (request->physical_sector - 1)) != 0)
        return STATUS_INVALID_PARAMETER;
    if (!g_bus_ext)
        return STATUS_DEVICE_NOT_READY;

    for (size_t i = 0; i < g_bus_ext->child_count; i++) {
        WINLVM_LV_EXT *lv = (WINLVM_LV_EXT *)g_bus_ext->children[i]->DeviceExtension;
        const lvmcore_volume_group *view = lv->record ? lv->record->view : NULL;
        if (!view || lv->lv_index >= view->lv_count)
            continue;
        if (RtlCompareMemory(view->lvs[lv->lv_index].id, request->lv_uuid,
                             WL_VG_UUID_LEN) == WL_VG_UUID_LEN)
            return STATUS_SUCCESS;
    }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS WinLvmDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;
    ULONG input_length = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG output_length = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;

    if (METHOD_FROM_CTL_CODE(code) != METHOD_BUFFERED) {
        status = STATUS_INVALID_DEVICE_REQUEST;
    } else if (*(WL_EXT_KIND *)DeviceObject->DeviceExtension == WL_EXT_CONTROL) {
        if (code == IOCTL_WINLVM_UPDATE_LV) {
            if (Irp->AssociatedIrp.SystemBuffer == NULL) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else {
            status = WinLvmValidateUpdateLv(
                (const WINLVM_UPDATE_LV_REQUEST *)Irp->AssociatedIrp.SystemBuffer,
                input_length);
            if (status == STATUS_SUCCESS) {
                const WINLVM_UPDATE_LV_REQUEST *request =
                    (const WINLVM_UPDATE_LV_REQUEST *)Irp->AssociatedIrp.SystemBuffer;
                WINLVM_LV_EXT *target = NULL;
                for (size_t i = 0; i < g_bus_ext->child_count; i++) {
                    WINLVM_LV_EXT *lv = (WINLVM_LV_EXT *)g_bus_ext->children[i]->DeviceExtension;
                    const lvmcore_volume_group *view = lv->record ? lv->record->view : NULL;
                    if (view && lv->lv_index < view->lv_count &&
                        RtlCompareMemory(view->lvs[lv->lv_index].id, request->lv_uuid,
                                         WL_VG_UUID_LEN) == WL_VG_UUID_LEN) {
                        target = lv;
                        break;
                    }
                }
                if (!target) {
                    status = STATUS_OBJECT_NAME_NOT_FOUND;
                } else {
                    status = WinLvmIoGateBeginQuiesce(&target->io_gate);
                    if (status == STATUS_SUCCESS) {
                        status = STATUS_NOT_SUPPORTED;
                        WinLvmIoGateResume(&target->io_gate, target->pdo);
                    }
                }
            }
            }
        }
        const BOOLEAN known = code == IOCTL_WINLVM_INVENTORY || code == IOCTL_WINLVM_REFRESH ||
                              code == IOCTL_WINLVM_QUIESCE_VG || code == IOCTL_WINLVM_RESUME_VG ||
                              code == IOCTL_WINLVM_ACTIVATE_LV || code == IOCTL_WINLVM_DEACTIVATE_LV ||
                              code == IOCTL_WINLVM_UPDATE_LV;
        if (code != IOCTL_WINLVM_UPDATE_LV && known &&
            input_length >= sizeof(WINLVM_CONTROL_REQUEST) &&
            Irp->AssociatedIrp.SystemBuffer != NULL) {
            const WINLVM_CONTROL_REQUEST *request =
                (const WINLVM_CONTROL_REQUEST *)Irp->AssociatedIrp.SystemBuffer;
            if (request->size >= sizeof(*request) && request->version == 1) {
                if (code == IOCTL_WINLVM_INVENTORY) {
                    if (output_length >= sizeof(WINLVM_INVENTORY)) {
                        WINLVM_INVENTORY *inventory =
                            (WINLVM_INVENTORY *)Irp->AssociatedIrp.SystemBuffer;
                        RtlZeroMemory(inventory, sizeof(*inventory));
                        inventory->version = 1;
                        inventory->size = sizeof(*inventory);
                        status = STATUS_SUCCESS;
                        information = sizeof(*inventory);
                    } else {
                        status = STATUS_BUFFER_TOO_SMALL;
                        information = sizeof(WINLVM_INVENTORY);
                    }
                } else if (code == IOCTL_WINLVM_QUIESCE_VG) {
                    status = STATUS_SUCCESS;
                    if (g_bus_ext) {
                        for (size_t i = 0; i < g_bus_ext->child_count; i++) {
                            WINLVM_LV_EXT *lv = (WINLVM_LV_EXT *)g_bus_ext->children[i]->DeviceExtension;
                            NTSTATUS gate_status = WinLvmIoGateBeginQuiesce(&lv->io_gate);
                            if (gate_status != STATUS_SUCCESS && gate_status != STATUS_PENDING) status = gate_status;
                        }
                    }
                } else if (code == IOCTL_WINLVM_RESUME_VG) {
                    if (g_bus_ext) {
                        for (size_t i = 0; i < g_bus_ext->child_count; i++) {
                            WinLvmIoGateResume(&((WINLVM_LV_EXT *)g_bus_ext->children[i]->DeviceExtension)->io_gate, g_bus_ext->children[i]);
                        }
                    }
                    status = STATUS_SUCCESS;
                } else {
                    status = STATUS_NOT_IMPLEMENTED;
                }
            } else {
                status = STATUS_INVALID_PARAMETER;
            }
        } else if (code != IOCTL_WINLVM_UPDATE_LV && known) {
            status = STATUS_BUFFER_TOO_SMALL;
        }
    } else if (*(WL_EXT_KIND *)DeviceObject->DeviceExtension == WL_EXT_LV) {
        WINLVM_LV_EXT *lv = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
        status = WinLvmMountIoctl(DeviceObject, Irp, lv);
        information = Irp->IoStatus.Information;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
