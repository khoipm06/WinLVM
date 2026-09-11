/*
 * winlvm.c — winlvm.sys: root bus FDO + LV PDO creation.
 *
 * WDM shape (mirrors maharmstone/winmd; FEASIBILITY.md §5.1):
 *   DriverEntry  — dispatch tables + PnP disk-interface registration.
 *                  No control device object: the INF enumerates ROOT\winlvm,
 *                  PnP creates the root bus PDO and calls our AddDevice.
 *   AddDevice     — create the bus FDO, attach to the root PDO stack.
 *   QUERY_DEVICE_RELATIONS (BusRelations) — report one child PDO per
 *                  mappable LV found on LVM PVs.
 *   LV PDO        — FILE_DEVICE_DISK, answers MOUNTDEV/geometry IOCTLs,
 *                  forwards reads/writes to the backing disk device object.
 *
 * NEVER ZwCreateFile: backing devices are addressed through the device
 * objects we already hold (IoGetDeviceObjectPointer in the notify callback,
 * IoCallDriver everywhere).
 */
#include <initguid.h>
#include "winlvm.h"
/* ntstrsafe must come after the WDK headers: it needs the STATUS_* constants
 * that ntddk.h defines (including it first breaks the build with
 * 'STATUS_INVALID_PARAMETER: undeclared identifier' inside ntstrsafe.h). */
#include <ntstrsafe.h>

/* Globals. g_bus_ext is set by AddDevice and cleared on REMOVE; the disk
 * arrival callback may fire before AddDevice, so it must be null-checked. */
WINLVM_BUS_EXT *g_bus_ext = NULL;
PVOID           g_disk_notify_cookie = NULL;

/* Per-call metadata scratch for lvm_read.c's kernel path: probe workers
 * for several disks run concurrently, so no shared statics. */
void *winlvm_probe_scratch_alloc(void) {
    return ExAllocatePoolZero(NonPagedPool, WINLVM_MAX_METADATA_BYTES + 1,
                             WINLVM_WIN_TAG);
}

void winlvm_probe_scratch_free(void *p) {
    if (p) ExFreePoolWithTag(p, WINLVM_WIN_TAG);
}

#define WL_TRACE(msg) DbgPrint("winlvm: " msg "\n")
#define WL_TRACE_1(msg, a) DbgPrint("winlvm: " msg "\n", a)

static const WCHAR kBusDeviceName[] = L"\\Device\\WinLvmBus";
static const WCHAR kBusSymLink[] = L"\\DosDevices\\WinLvmBus";

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = WinLvmCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = WinLvmCreateClose;
    DriverObject->MajorFunction[IRP_MJ_READ]           = WinLvmRead;
    DriverObject->MajorFunction[IRP_MJ_WRITE]          = WinLvmWrite;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS]  = WinLvmFlush;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN]       = WinLvmShutdown;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = WinLvmDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP]            = WinLvmPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER]          = WinLvmPower;
    DriverObject->DriverExtension->AddDevice           = WinLvmAddDevice;
    DriverObject->DriverUnload                         = WinLvmUnload;

    /* PnP registration must happen after the dispatch table so a callback
     * racing DriverEntry's return never sees a NULL handler. */
    NTSTATUS status = IoRegisterPlugPlayNotification(
        EventCategoryDeviceInterfaceChange,
        PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
        (PVOID)&GUID_DEVINTERFACE_DISK,
        DriverObject,
        WinLvmPnpDiskNotify,
        NULL,
        &g_disk_notify_cookie);
    return status;
}

/* --------------------------------------------------------------- */
/* Bus FDO                                                          */
/* --------------------------------------------------------------- */

NTSTATUS WinLvmAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject) {
    UNREFERENCED_PARAMETER(DriverObject);

    /* The INF installs our service on the LV child devnodes too; stacking a
     * second bus FDO on a child PDO we created ourselves is bus-on-bus
     * recursion (the 2026-09-11 install crash-loop). Refuse, fail closed. */
    if (PhysicalDeviceObject->DriverObject == DriverObject || g_bus_ext) {
        WL_TRACE("AddDevice refused: own child PDO or bus already up");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    PDEVICE_OBJECT fdo = NULL;
    NTSTATUS status = IoCreateDevice(DriverObject, sizeof(WINLVM_BUS_EXT),
                                     NULL, FILE_DEVICE_BUS_EXTENDER,
                                     FILE_DEVICE_SECURE_OPEN, FALSE, &fdo);
    if (!NT_SUCCESS(status)) return status;

    WINLVM_BUS_EXT *ext = (WINLVM_BUS_EXT *)fdo->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));
    ext->kind = WL_EXT_BUS;  /* dispatch discriminator — MUST precede any IRP */
    ext->fdo = fdo;
    ext->pdo = PhysicalDeviceObject;
    KeInitializeSpinLock(&ext->lock);
    ext->bus_pdo = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
    if (!ext->bus_pdo) {
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }

    g_bus_ext = ext;

    fdo->Flags |= DO_BUFFERED_IO;   /* no direct I/O on the bus FDO itself */
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

/* Power IRPs: LV PDOs are leaf devices (complete immediately); the bus
 * FDO forwards down the root stack. Extension kind is at offset 0 in both. */
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
    IoCopyCurrentIrpStackLocationToNext(Irp);
    return PoCallDriver(ext->bus_pdo, Irp);
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

/* --------------------------------------------------------------- */
/* Disk arrival: probe for LVM PVs, cache VG, create LV PDOs.       */
/* --------------------------------------------------------------- */

typedef struct {
    WORK_QUEUE_ITEM item;
    PDEVICE_OBJECT  target_disk;
    PFILE_OBJECT    target_file;
} WINLVM_PROBE_WORK;

NTSTATUS WinLvmPnpDiskNotify(PVOID notificationStruct, PVOID context) {
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION data =
        (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)notificationStruct;
    UNREFERENCED_PARAMETER(context);
    if (!data || !data->SymbolicLinkName) return STATUS_SUCCESS;
    if (!IsEqualGUID(&data->Event, &GUID_DEVICE_INTERFACE_ARRIVAL)) return STATUS_SUCCESS;

    WINLVM_PROBE_WORK *work = ExAllocatePoolZero(NonPagedPool,
        sizeof(WINLVM_PROBE_WORK), WINLVM_WIN_TAG);
    if (!work) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS status = IoGetDeviceObjectPointer(data->SymbolicLinkName,
        FILE_READ_ATTRIBUTES, &work->target_file, &work->target_disk);
    if (!NT_SUCCESS(status)) {
        WL_TRACE_1("notify: pointer failed 0x%x", (unsigned)status);
        ExFreePoolWithTag(work, WINLVM_WIN_TAG);
        return status;
    }
    /* The pointer keeps the file object referenced; the worker releases it. */

    ExInitializeWorkItem(&work->item, WinLvmProbeWorker, work);
    ExQueueWorkItem(&work->item, DelayedWorkQueue);
    return STATUS_SUCCESS;
}

static VOID WinLvmProbeWorkerCreateLvPdo(WINLVM_BUS_EXT *bus,
                                         WINLVM_VG_RECORD *record,
                                         size_t lv_index,
                                         PDEVICE_OBJECT backing_disk);

void WinLvmProbeWorker(PVOID context) {
    WINLVM_PROBE_WORK *work = (WINLVM_PROBE_WORK *)context;
    WINLVM_BUS_EXT *bus = g_bus_ext;
    WL_TRACE("probe worker start");

    if (bus) {
        WINLVM_DISK_SOURCE dsrc;
        dsrc.disk_pdo = work->target_disk;
        dsrc.file = work->target_file;
        KeInitializeEvent(&dsrc.done, NotificationEvent, FALSE);

        winlvm_byte_source src;
        src.impl = &dsrc;
        src.size = ~0ULL;
        src.read = winlvm_disk_source_read;

        winlvm_volume_group *vg = ExAllocatePoolZero(NonPagedPool,
            sizeof(winlvm_volume_group), WINLVM_WIN_TAG);
        if (vg) {
            winlvm_lvm_err err;
            if (winlvm_read_vg(&src, vg, &err)) {
                WL_TRACE_1("probe: VG parsed, lvs=%u", (unsigned)vg->lv_count);
                KIRQL old;
                KeAcquireSpinLock(&bus->lock, &old);
                WINLVM_VG_RECORD *record = bus->vgs;
                KeReleaseSpinLock(&bus->lock, old);

                if (!record) {
                    record = ExAllocatePoolZero(NonPagedPool,
                        sizeof(WINLVM_VG_RECORD), WINLVM_WIN_TAG);
                    if (record) {
                        record->vg = *vg;
                        ObReferenceObject(work->target_disk);
                        record->backing_pdo = work->target_disk;
                        KeAcquireSpinLock(&bus->lock, &old);
                        if (!bus->vgs) { bus->vgs = record; }
                        else {
                            WINLVM_VG_RECORD *r = bus->vgs;
                            /* single-PV groups only for now: a second VG on
                             * another disk is dropped with a ref leak-free */
                            if (r != record) {
                                KeReleaseSpinLock(&bus->lock, old);
                                ObDereferenceObject(work->target_disk);
                                ExFreePoolWithTag(record, WINLVM_WIN_TAG);
                                record = NULL;
                                KeAcquireSpinLock(&bus->lock, &old);
                            }
                        }
                        KeReleaseSpinLock(&bus->lock, old);
                    }
                }

                if (record) {
                    for (size_t i = 0; i < vg->lv_count; i++) {
                        if (!vg->lvs[i].mappable) continue;
                        WinLvmProbeWorkerCreateLvPdo(bus, record, i,
                                                     work->target_disk);
                    }
                    IoInvalidateDeviceRelations(bus->pdo, BusRelations);
                }
            } else {
                WL_TRACE_1("probe: parse failed: %s", err.message);
            }
            ExFreePoolWithTag(vg, WINLVM_WIN_TAG);
        }
    }

    ObDereferenceObject(work->target_file);  /* pair from IoGetDeviceObjectPointer */
    ExFreePoolWithTag(work, WINLVM_WIN_TAG);
}

static VOID WinLvmProbeWorkerCreateLvPdo(WINLVM_BUS_EXT *bus,
                                         WINLVM_VG_RECORD *record,
                                         size_t lv_index,
                                         PDEVICE_OBJECT backing_disk) {
    /* Named device object: mountmgr requires QUERY_DEVICE_NAME to answer
     * with a \Device namespace name it can open; the volume interface
     * (\??\Volume{guid}) symlink is NOT a valid answer there. */
    winlvm_logical_volume *lv0 = &record->vg.lvs[lv_index];
    WCHAR namebuf[64];
    RtlZeroMemory(namebuf, sizeof(namebuf));
    UNICODE_STRING devName;
    devName.Buffer = namebuf;
    devName.MaximumLength = sizeof(namebuf);
    NTSTATUS status = RtlUnicodeStringPrintf(&devName, L"\\Device\\WinLvm%hs",
                                             lv0->id);
    if (!NT_SUCCESS(status)) return;

    PDEVICE_OBJECT lv_pdo = NULL;
    status = IoCreateDevice(bus->fdo->DriverObject,
                                     sizeof(WINLVM_LV_EXT), &devName,
                                     FILE_DEVICE_DISK, FILE_DEVICE_SECURE_OPEN,
                                     FALSE, &lv_pdo);
    if (!NT_SUCCESS(status)) {
        WL_TRACE_1("pdo create failed 0x%x", (unsigned)status);
        return;
    }

    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)lv_pdo->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));
    ext->kind = WL_EXT_LV;  /* dispatch discriminator — MUST precede any IRP */
    RtlCopyMemory(ext->devname, namebuf, sizeof(namebuf));
    ext->pdo = lv_pdo;
    ext->record = record;
    ext->lv_index = lv_index;
    ext->backing_pdo = backing_disk;
    ObReferenceObject(backing_disk);

    winlvm_logical_volume *lv = &record->vg.lvs[lv_index];
    ext->byte_size = 0;
    for (size_t s = 0; s < lv->segment_count; s++)
        ext->byte_size += lv->segments[s].extent_count
                          * record->vg.extent_size_sectors * 512ULL;

    /* ISSUE-21: forwarded IRPs need a stack location for the backing
     * disk's whole stack or IoCallDriver writes past the IRP — BSOD. */
    lv_pdo->StackSize = (UCHAR)(backing_disk->StackSize + 1);
    lv_pdo->Flags |= DO_DIRECT_IO;
    lv_pdo->Flags &= ~DO_DEVICE_INITIALIZING;

    UNICODE_STRING ifname;
    status = IoRegisterDeviceInterface(lv_pdo, &GUID_DEVINTERFACE_VOLUME,
                                       NULL, &ifname);
    if (NT_SUCCESS(status)) {
        IoSetDeviceInterfaceState(&ifname, TRUE);
        ext->ifname = ifname;
    }

    KIRQL old;
    KeAcquireSpinLock(&bus->lock, &old);
    if (bus->child_count < WL_MAX_CHILDREN) bus->children[bus->child_count++] = lv_pdo;
    else {
        /* over cap: undo rather than corrupt the array */
        KeReleaseSpinLock(&bus->lock, old);
        if (ext->ifname.Buffer) {
            IoSetDeviceInterfaceState(&ext->ifname, FALSE);
            RtlFreeUnicodeString(&ext->ifname);
        }
        ObDereferenceObject(backing_disk);
        IoDeleteDevice(lv_pdo);
        return;
    }
    KeReleaseSpinLock(&bus->lock, old);
}

/* --------------------------------------------------------------- */
/* PnP dispatch: bus FDO + LV PDOs.                                 */
/* --------------------------------------------------------------- */

static NTSTATUS WinLvmQueryBusRelations(WINLVM_BUS_EXT *bus, PIRP Irp);

NTSTATUS WinLvmPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    WINLVM_BUS_EXT *ext = (WINLVM_BUS_EXT *)DeviceObject->DeviceExtension;

    if (kind == WL_EXT_BUS) {
        /* ---- bus FDO ---- */
        switch (irpSp->MinorFunction) {
        case IRP_MN_QUERY_DEVICE_RELATIONS:
            if (irpSp->Parameters.QueryDeviceRelations.Type == BusRelations) {
                NTSTATUS st = WinLvmQueryBusRelations(ext, Irp);
                if (!NT_SUCCESS(st)) {
                    Irp->IoStatus.Status = st;
                    IoCompleteRequest(Irp, IO_NO_INCREMENT);
                    return st;
                }
                /* fall through to forward with our relations attached */
            }
            break;
        case IRP_MN_REMOVE_DEVICE: {
            IoSkipCurrentIrpStackLocation(Irp);
            NTSTATUS st = IoCallDriver(ext->bus_pdo, Irp);
            /* forward down, then detach + delete children then self */
            for (size_t i = 0; i < ext->child_count; i++) {
                WINLVM_LV_EXT *ce = (WINLVM_LV_EXT *)ext->children[i]->DeviceExtension;
                if (ce->ifname.Buffer) {
                    IoSetDeviceInterfaceState(&ce->ifname, FALSE);
                    RtlFreeUnicodeString(&ce->ifname);
                }
                ObDereferenceObject(ce->backing_pdo);
                IoDeleteDevice(ext->children[i]);
            }
            ext->child_count = 0;
            if (ext->vgs) {
                ObDereferenceObject(ext->vgs->backing_pdo);
                ExFreePoolWithTag(ext->vgs, WINLVM_WIN_TAG);
                ext->vgs = NULL;
            }
            IoDetachDevice(ext->bus_pdo);
            if (g_bus_ext == ext) g_bus_ext = NULL;
            IoDeleteDevice(DeviceObject);
            return st;
        }
        default:
            break;
        }
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(ext->bus_pdo, Irp);
    }

    /* ---- LV PDO (child of our bus): we are the bottom of its stack ---- */
    WINLVM_LV_EXT *lvext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    switch (irpSp->MinorFunction) {
    case IRP_MN_QUERY_ID: {
        BUS_QUERY_ID_TYPE type = irpSp->Parameters.QueryId.IdType;
        if (type == BusQueryHardwareIDs) {
            /* must match the INF's WinLvmVolume model entry */
            PWCHAR ids = (PWCHAR)ExAllocatePoolZero(NonPagedPool,
                sizeof(WinLvmHwId) + sizeof(WCHAR) * 2, WINLVM_WIN_TAG);
            if (!ids) break;
            RtlCopyMemory(ids, WinLvmHwId, sizeof(WinLvmHwId));
            Irp->IoStatus.Information = (ULONG_PTR)ids;
            Irp->IoStatus.Status = STATUS_SUCCESS;
        } else if (type == BusQueryDeviceID) {
            PWCHAR id = (PWCHAR)ExAllocatePoolZero(NonPagedPool,
                sizeof(WinLvmHwId), WINLVM_WIN_TAG);
            if (!id) break;
            RtlCopyMemory(id, WinLvmHwId, sizeof(WinLvmHwId));
            Irp->IoStatus.Information = (ULONG_PTR)id;
            Irp->IoStatus.Status = STATUS_SUCCESS;
        } else if (type == BusQueryInstanceID) {
            winlvm_logical_volume *lv = &lvext->record->vg.lvs[lvext->lv_index];
            WCHAR inst[40];
            /* instance = LV UUID, guaranteed unique per device */
            size_t i = 0;
            for (; i < 32 && lv->id[i]; i++) inst[i] = (WCHAR)lv->id[i];
            inst[i] = 0;
            /* QUERY_ID answers are REG_MULTI_SZ lists: the string's NUL must
             * be followed by the list-terminating NUL. A (i+1) allocation
             * left PnP parsing one WCHAR past the block (pool overread) —
             * the 2026-09-11 install crash. */
            PWCHAR id = (PWCHAR)ExAllocatePoolZero(NonPagedPool,
                (i + 2) * sizeof(WCHAR), WINLVM_WIN_TAG);
            if (!id) break;
            RtlCopyMemory(id, inst, (i + 1) * sizeof(WCHAR));
            Irp->IoStatus.Information = (ULONG_PTR)id;
            Irp->IoStatus.Status = STATUS_SUCCESS;
        }
        break;
    }
    case IRP_MN_QUERY_CAPABILITIES: {
        /* Raw-PDO contract (Oracle-verified): with no function driver the
         * child devnode only starts if the PDO marks itself raw-capable. */
        PDEVICE_CAPABILITIES caps = irpSp->Parameters.DeviceCapabilities.Capabilities;
        caps->RawDeviceOK = TRUE;
        caps->SurpriseRemovalOK = TRUE;
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_RESOURCES:
    case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
        /* No hardware resources behind an LV; PnP requires the explicit
         * success-with-NULL or the raw devnode never starts. */
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    case IRP_MN_START_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    case IRP_MN_REMOVE_DEVICE:
        /* children are removed by the bus REMOVE path; if PnP removes a
         * child directly, complete quietly — bus REMOVE does the cleanup */
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    default:
        /* Raw-PDO contract: unknown PnP requests (QUERY_INTERFACE,
         * QUERY_DEVICE_TEXT, ...) must NOT complete with success —
         * STATUS_SUCCESS on IRP_MN_QUERY_INTERFACE makes the querying
         * driver call through its uninitialized output struct
         * (privileged-instruction BSOD 0x7E/c0000096, 2026-09-11). */
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        break;
    }

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}

static NTSTATUS WinLvmQueryBusRelations(WINLVM_BUS_EXT *bus, PIRP Irp) {
    KIRQL old;
    KeAcquireSpinLock(&bus->lock, &old);
    size_t n = bus->child_count;
    KeReleaseSpinLock(&bus->lock, old);
    if (n == 0) return STATUS_SUCCESS;  /* leave existing (NULL) relations */

    size_t bytes = sizeof(DEVICE_RELATIONS) + n * sizeof(PDEVICE_OBJECT);
    PDEVICE_RELATIONS rel = (PDEVICE_RELATIONS)ExAllocatePoolZero(
        NonPagedPool, bytes, WINLVM_WIN_TAG);
    if (!rel) return STATUS_INSUFFICIENT_RESOURCES;

    KeAcquireSpinLock(&bus->lock, &old);
    rel->Count = (ULONG)n;
    for (size_t i = 0; i < n; i++) {
        rel->Objects[i] = bus->children[i];
        /* PnP takes its own references when it consumes the relations. */
    }
    KeReleaseSpinLock(&bus->lock, old);

    /* attach to the IRP, replacing any previous answer */
    if (Irp->IoStatus.Information) {
        ExFreePool((PVOID)Irp->IoStatus.Information);
    }
    Irp->IoStatus.Information = (ULONG_PTR)rel;
    Irp->IoStatus.Status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
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
