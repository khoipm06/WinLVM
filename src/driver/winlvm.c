/*
 * winlvm.c — DriverEntry, control device security, ring log, WMI provider,
 *            and top-level IRP dispatchers for winlvm.sys.
 */
#include <initguid.h>
#include "winlvm.h"
#include <ntstrsafe.h>

/* Private device class GUID for \Device\WinLvmControl SDDL enforcement. */
/* {8C3A4E71-9B42-4D1F-A6E5-3F1D8C2B9A04} */
DEFINE_GUID(GUID_DEVCLASS_WINLVM_CONTROL,
    0x8c3a4e71, 0x9b42, 0x4d1f, 0xa6, 0xe5, 0x3f, 0x1d, 0x8c, 0x2b, 0x9a, 0x04);

static KSPIN_LOCK g_log_lock;
static char       g_kernel_log[WINLVM_LOG_BUFFER_SIZE];
static ULONG      g_log_head = 0;
static ULONG      g_log_len = 0;
static BOOLEAN    g_log_ready = FALSE;

WINLVM_BUS_EXT *g_bus_ext = NULL;
PVOID           g_disk_notify_cookie = NULL;
PVOID           g_volume_notify_cookie = NULL;
PDEVICE_OBJECT  g_control_device = NULL;
BOOLEAN         g_control_link_created = FALSE;

void WinLvmLog(const char *fmt, ...) {
    va_list args;
    char buf[256];
    va_start(args, fmt);
    NTSTATUS st = RtlStringCbVPrintfA(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (!NT_SUCCESS(st)) return;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "%s", buf);
    if (!g_log_ready) return;

    const char *src = buf;
    ULONG n = (ULONG)strlen(src);
    if (n == 0) return;
    if (n > WINLVM_LOG_BUFFER_SIZE) {
        src += (n - WINLVM_LOG_BUFFER_SIZE);
        n = WINLVM_LOG_BUFFER_SIZE;
    }

    KIRQL irql;
    KeAcquireSpinLock(&g_log_lock, &irql);
    ULONG tail = (g_log_head + g_log_len) % WINLVM_LOG_BUFFER_SIZE;
    ULONG first = WINLVM_LOG_BUFFER_SIZE - tail;
    if (first > n) first = n;
    RtlCopyMemory(&g_kernel_log[tail], src, first);
    if (n > first) {
        RtlCopyMemory(&g_kernel_log[0], src + first, n - first);
    }
    if (g_log_len + n <= WINLVM_LOG_BUFFER_SIZE) {
        g_log_len += n;
    } else {
        ULONG drop = (g_log_len + n) - WINLVM_LOG_BUFFER_SIZE;
        g_log_head = (g_log_head + drop) % WINLVM_LOG_BUFFER_SIZE;
        g_log_len = WINLVM_LOG_BUFFER_SIZE;
    }
    KeReleaseSpinLock(&g_log_lock, irql);
}

ULONG WinLvmLogDrain(char *out, ULONG cap) {
    if (!g_log_ready || !out || cap == 0) return 0;

    KIRQL irql;
    KeAcquireSpinLock(&g_log_lock, &irql);
    ULONG to_copy = (g_log_len < cap) ? g_log_len : cap;
    if (to_copy > 0) {
        ULONG first = WINLVM_LOG_BUFFER_SIZE - g_log_head;
        if (first > to_copy) first = to_copy;
        RtlCopyMemory(out, &g_kernel_log[g_log_head], first);
        if (to_copy > first) {
            RtlCopyMemory(out + first, &g_kernel_log[0], to_copy - first);
        }
        g_log_head = (g_log_head + to_copy) % WINLVM_LOG_BUFFER_SIZE;
        g_log_len -= to_copy;
    }
    KeReleaseSpinLock(&g_log_lock, irql);
    return to_copy;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNICODE_STRING device_name;
    UNICODE_STRING dos_name;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(RegistryPath);

    KeInitializeSpinLock(&g_log_lock);
    g_log_head = 0;
    g_log_len = 0;
    g_log_ready = TRUE;
    WinLvmProbeSubsystemInit();

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
    status = IoCreateDeviceSecure(
        DriverObject,
        sizeof(WINLVM_CONTROL_EXT),
        &device_name,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &SDDL_DEVOBJ_SYS_ALL_ADM_ALL,
        &GUID_DEVCLASS_WINLVM_CONTROL,
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

    WL_TRACE("DriverEntry");
    return STATUS_SUCCESS;
}

void WinLvmUnload(PDRIVER_OBJECT DriverObject) {
    UNICODE_STRING dos_name;

    UNREFERENCED_PARAMETER(DriverObject);
    WinLvmStopAndDrainProbeWork();

    RtlInitUnicodeString(&dos_name, WINLVM_CONTROL_DOS_NAME);
    if (g_control_link_created) {
        IoDeleteSymbolicLink(&dos_name);
        g_control_link_created = FALSE;
    }
    if (g_control_device) {
        IoDeleteDevice(g_control_device);
        g_control_device = NULL;
    }
}

NTSTATUS WinLvmCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS WinLvmQueryWmiRegInfo(PDEVICE_OBJECT DeviceObject, PULONG RegFlags,
                               PUNICODE_STRING InstanceName,
                               PUNICODE_STRING *RegistryPath,
                               PUNICODE_STRING MofResourceName,
                               PDEVICE_OBJECT *Pdo) {
    UNREFERENCED_PARAMETER(InstanceName);
    UNREFERENCED_PARAMETER(MofResourceName);

    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    if (kind != WL_EXT_BUS && kind != WL_EXT_LV) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    *RegFlags = WMIREG_FLAG_INSTANCE_PDO;
    *RegistryPath = NULL;
    *Pdo = (kind == WL_EXT_BUS)
        ? ((WINLVM_BUS_EXT *)DeviceObject->DeviceExtension)->pdo
        : DeviceObject;
    return STATUS_SUCCESS;
}

void WinLvmWmiInit(PDEVICE_OBJECT DeviceObject) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    WMILIB_CONTEXT *wmi = NULL;
    BOOLEAN *registered = NULL;

    if (kind == WL_EXT_BUS) {
        WINLVM_BUS_EXT *bus = (WINLVM_BUS_EXT *)DeviceObject->DeviceExtension;
        wmi = &bus->wmi;
        registered = &bus->wmi_registered;
    } else if (kind == WL_EXT_LV) {
        WINLVM_LV_EXT *lv = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
        wmi = &lv->wmi;
        registered = &lv->wmi_registered;
    } else {
        return;
    }

    if (*registered) return;
    RtlZeroMemory(wmi, sizeof(*wmi));
    wmi->QueryWmiRegInfo = WinLvmQueryWmiRegInfo;

    NTSTATUS st = IoWMIRegistrationControl(DeviceObject, WMIREG_ACTION_REGISTER);
    if (NT_SUCCESS(st)) {
        *registered = TRUE;
    }
    WL_TRACE_1("wmi register 0x%x", (unsigned)st);
}

void WinLvmWmiDeregister(PDEVICE_OBJECT DeviceObject) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    BOOLEAN *registered = NULL;

    if (kind == WL_EXT_BUS) {
        registered = &((WINLVM_BUS_EXT *)DeviceObject->DeviceExtension)->wmi_registered;
    } else if (kind == WL_EXT_LV) {
        registered = &((WINLVM_LV_EXT *)DeviceObject->DeviceExtension)->wmi_registered;
    } else {
        return;
    }

    if (*registered) {
        *registered = FALSE;
        IoWMIRegistrationControl(DeviceObject, WMIREG_ACTION_DEREGISTER);
    }
}

NTSTATUS WinLvmSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    if (kind != WL_EXT_BUS && kind != WL_EXT_LV) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    WMILIB_CONTEXT *wmi = (kind == WL_EXT_BUS)
        ? &((WINLVM_BUS_EXT *)DeviceObject->DeviceExtension)->wmi
        : &((WINLVM_LV_EXT *)DeviceObject->DeviceExtension)->wmi;

    SYSCTL_IRP_DISPOSITION disposition = IrpNotWmi;
    NTSTATUS status = WmiSystemControl(wmi, DeviceObject, Irp, &disposition);

    switch (disposition) {
    case IrpProcessed:
        return status;
    case IrpNotCompleted:
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    case IrpForward:
    case IrpNotWmi:
        if (kind == WL_EXT_BUS) {
            WINLVM_BUS_EXT *ext = (WINLVM_BUS_EXT *)DeviceObject->DeviceExtension;
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(ext->bus_pdo, Irp);
        }
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    default:
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

NTSTATUS WinLvmPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    if (kind == WL_EXT_BUS) return WinLvmBusPnp(DeviceObject, Irp);
    if (kind == WL_EXT_LV) return WinLvmChildPnp(DeviceObject, Irp);

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS WinLvmDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;

    if (kind == WL_EXT_CONTROL) {
        if (METHOD_FROM_CTL_CODE(code) == METHOD_BUFFERED) {
            return WinLvmControlDeviceControl(DeviceObject, Irp);
        }
    } else if (kind == WL_EXT_LV) {
        WINLVM_LV_EXT *lv = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
        /* FSCTL_ALLOW_EXTENDED_DASD_IO uses METHOD_NEITHER and carries no buffers. */
        if (code == FSCTL_ALLOW_EXTENDED_DASD_IO) {
            status = WinLvmMountIoctl(DeviceObject, Irp, lv);
            information = Irp->IoStatus.Information;
        } else if (METHOD_FROM_CTL_CODE(code) == METHOD_BUFFERED) {
            status = WinLvmMountIoctl(DeviceObject, Irp, lv);
            if (status == STATUS_INVALID_DEVICE_REQUEST) {
                status = WinLvmStorageIoctl(DeviceObject, Irp, lv);
            }
            information = Irp->IoStatus.Information;
        }
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
