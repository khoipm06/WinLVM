/*
 * winlvm.c — DriverEntry, CDO, PnP notification, and LV PDO creation.
 */
#include "winlvm.h"

PDEVICE_OBJECT g_cdo = NULL;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);

    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\Device\\winlvm");
    UNICODE_STRING sym;
    RtlInitUnicodeString(&sym, L"\\DosDevices\\winlvm");

    NTSTATUS status = IoCreateDevice(DriverObject, sizeof(WINLVM_CDO_EXT),
                                     &name, FILE_DEVICE_UNKNOWN,
                                     FILE_DEVICE_SECURE_OPEN, FALSE, &g_cdo);
    if (!NT_SUCCESS(status)) return status;

    g_cdo->Flags |= DO_DIRECT_IO;
    WINLVM_CDO_EXT *ext = (WINLVM_CDO_EXT *)g_cdo->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));
    ext->control_device = g_cdo;
    KeInitializeSpinLock(&ext->lock);

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = WinLvmCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = WinLvmCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = WinLvmCreateClose;
    DriverObject->MajorFunction[IRP_MJ_READ]           = WinLvmRead;
    DriverObject->MajorFunction[IRP_MJ_WRITE]          = WinLvmWrite;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS]  = WinLvmFlush;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN]       = WinLvmShutdown;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = WinLvmDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP]            = WinLvmPnp;
    DriverObject->DriverExtension->AddDevice           = WinLvmAddDevice;
    DriverObject->DriverUnload                         = WinLvmUnload;

    IoCreateSymbolicLink(&sym, &name);

    /* Watch for disks arriving. */
    status = IoRegisterPlugPlayNotification(EventCategoryDeviceInterfaceChange,
                                            PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
                                            (PVOID)&GUID_DEVINTERFACE_DISK,
                                            DriverObject,
                                            WinLvmPnpDiskNotify,
                                            NULL,
                                            &ext->disk_notify_cookie);
    if (!NT_SUCCESS(status)) {
        IoDeleteSymbolicLink(&sym);
        IoDeleteDevice(g_cdo);
        return status;
    }

    g_cdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

void WinLvmUnload(PDRIVER_OBJECT DriverObject) {
    WINLVM_CDO_EXT *ext = (WINLVM_CDO_EXT *)g_cdo->DeviceExtension;
    if (ext->disk_notify_cookie) {
        IoUnregisterPlugPlayNotification(ext->disk_notify_cookie);
    }
    UNICODE_STRING sym;
    RtlInitUnicodeString(&sym, L"\\DosDevices\\winlvm");
    IoDeleteSymbolicLink(&sym);
    IoDeleteDevice(g_cdo);
}

NTSTATUS WinLvmAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject) {
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);
    return STATUS_SUCCESS;
}

NTSTATUS WinLvmCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS WinLvmWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    /* Phase 3 territory. We now actually write through to the backing PV. */
    if (DeviceObject == g_cdo) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

    if (ext->vg->vg.seqno == 0 && !IsListEmpty(&ext->vg->volume_ifname)) {
        /* Defensive check: if we have not parsed a VG (seqno==0 is the
         * value zero-initialised CDO extension falls back to), refuse. */
        Irp->IoStatus.Status = STATUS_NO_MEDIA;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NO_MEDIA;
    }

    if (irpSp->Parameters.Write.Length == 0) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    if ((ULONGLONG)irpSp->Parameters.Write.ByteOffset.QuadPart >= ext->byte_size) {
        Irp->IoStatus.Status = STATUS_END_OF_FILE;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_END_OF_FILE;
    }

    if ((ULONGLONG)irpSp->Parameters.Write.ByteOffset.QuadPart + irpSp->Parameters.Write.Length > ext->byte_size) {
        irpSp->Parameters.Write.Length = (ULONG)(ext->byte_size - irpSp->Parameters.Write.ByteOffset.QuadPart);
    }

    winlvm_logical_volume *lv = &ext->vg->vg.lvs[ext->lv_index];

    /* Honor LVM2 system_id. If the VG has a system_id, it is owned by a
     * specific host (likely the Linux machine). We refuse to write to it. */
    if (ext->vg->vg.system_id[0] != '\0') {
        Irp->IoStatus.Status = STATUS_MEDIA_WRITE_PROTECTED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    if (lv->segment_count == 1 && lv->segments[0].type == WINLVM_SEG_STRIPED) {
        PIO_STACK_LOCATION nextSp = IoGetNextIrpStackLocation(Irp);
        *nextSp = *irpSp;

        uint64_t pv_ext_offset = lv->segments[0].stripes[0].start_extent;
        uint64_t bytes_per_ext = ext->vg->vg.extent_size_sectors * 512;
        uint64_t pv_byte_offset = ext->vg->vg.pvs[0].pe_start * 512 + (pv_ext_offset * bytes_per_ext);

        nextSp->Parameters.Write.ByteOffset.QuadPart += pv_byte_offset;

        IoSetCompletionRoutine(Irp, NULL, NULL, FALSE, FALSE, FALSE);
        return IoCallDriver(ext->backing_pdo, Irp);
    }

    Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS WinLvmFlush(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    /* FLUSH_BUFFERS must propagate to the backing PV so that NTFS's
     * own consistency guarantees are preserved. We pass the IRP down
     * to the backing_pdo with no changes (no completion routine needed). */
    if (DeviceObject == g_cdo) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    PIO_STACK_LOCATION nextSp = IoGetNextIrpStackLocation(Irp);
    *nextSp = *irpSp;

    IoSetCompletionRoutine(Irp, NULL, NULL, FALSE, FALSE, FALSE);
    return IoCallDriver(ext->backing_pdo, Irp);
}

NTSTATUS WinLvmShutdown(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    /* SHUTDOWN is the same idea: ensure all dirty writes are persisted
     * before Windows turns the machine off. Pass to the backing_pdo. */
    if (DeviceObject == g_cdo) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    PIO_STACK_LOCATION nextSp = IoGetNextIrpStackLocation(Irp);
    *nextSp = *irpSp;

    IoSetCompletionRoutine(Irp, NULL, NULL, FALSE, FALSE, FALSE);
    return IoCallDriver(ext->backing_pdo, Irp);
}

NTSTATUS WinLvmPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    /* Pass down or complete. WDM PnP is famously complex. We complete
     * immediately for the CDO and LV PDOs since they have no bus above them. */
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS WinLvmDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    if (DeviceObject == g_cdo) {
        Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
    } else {
        WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
        Irp->IoStatus.Status = WinLvmMountIoctl(DeviceObject, Irp, ext);
    }
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}

/* ============================================================== */
/* PnP disk-arrival handler + probe worker.                        */
/* ============================================================== */

typedef struct {
    WORK_QUEUE_ITEM item;
    PDEVICE_OBJECT  cdo;
    PDEVICE_OBJECT  target_disk;
    PFILE_OBJECT    target_file;
} WINLVM_PROBE_WORK;

NTSTATUS WinLvmPnpDiskNotify(PVOID notificationStruct, PVOID context) {
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION data = (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)notificationStruct;
    UNREFERENCED_PARAMETER(context);
    if (!data || !data->SymbolicLinkName) {
        return STATUS_SUCCESS;
    }

    if (!IsEqualGUID(&data->Event, &GUID_DEVICE_INTERFACE_ARRIVAL)) {
        return STATUS_SUCCESS;
    }

    WINLVM_PROBE_WORK *work = ExAllocatePoolZero(NonPagedPool, sizeof(WINLVM_PROBE_WORK), WINLVM_WIN_TAG);
    if (!work) return STATUS_INSUFFICIENT_RESOURCES;

    work->cdo = g_cdo;

    /* Get the device object without re-opening it like a filesystem */
    NTSTATUS status = IoGetDeviceObjectPointer(
        data->SymbolicLinkName, FILE_READ_ATTRIBUTES,
        &work->target_file, &work->target_disk);

    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(work, WINLVM_WIN_TAG);
        return status;
    }

    ObReferenceObject(work->target_disk);
    ExInitializeWorkItem(&work->item, WinLvmProbeWorker, work);
    ExQueueWorkItem(&work->item, DelayedWorkQueue);
    return STATUS_SUCCESS;
}

void WinLvmProbeWorker(PVOID context) {
    WINLVM_PROBE_WORK *work = (WINLVM_PROBE_WORK *)context;
    WINLVM_CDO_EXT *cdo_ext = (WINLVM_CDO_EXT *)work->cdo->DeviceExtension;

    /* Create the winlvm_byte_source backed by the disk PDO */
    WINLVM_DISK_SOURCE dsrc;
    dsrc.disk_pdo = work->target_disk;
    dsrc.file = work->target_file;
    KeInitializeEvent(&dsrc.done, NotificationEvent, FALSE);

    winlvm_byte_source src;
    src.impl = &dsrc;
    src.size = ~0ULL; /* the parser does bounds checks */
    src.read = winlvm_disk_source_read;

    /* Parse LVM */
    winlvm_volume_group vg;
    winlvm_lvm_err err;
    if (winlvm_read_vg(&src, &vg, &err)) {
        /* Cache the first VG found. */
        KIRQL oldIrql;
        KeAcquireSpinLock(&cdo_ext->lock, &oldIrql);
        BOOLEAN insert = (cdo_ext->vg_count == 0);
        KeReleaseSpinLock(&cdo_ext->lock, oldIrql);

        if (insert) {
            WINLVM_VG_RECORD *records = ExAllocatePoolZero(NonPagedPool, sizeof(WINLVM_VG_RECORD), WINLVM_WIN_TAG);
            if (records) {
                records->vg = vg;
                records->backing_pdos = ExAllocatePoolZero(NonPagedPool, sizeof(PDEVICE_OBJECT), WINLVM_WIN_TAG);
                if (records->backing_pdos) {
                    records->backing_pdos[0] = work->target_disk;
                    ObReferenceObject(work->target_disk);
                }
                KeAcquireSpinLock(&cdo_ext->lock, &oldIrql);
                if (cdo_ext->vg_count == 0) {
                    cdo_ext->vgs = records;
                    cdo_ext->vg_count = 1;
                }
                KeReleaseSpinLock(&cdo_ext->lock, oldIrql);
            }
        }

        WINLVM_VG_RECORD *record = NULL;
        KeAcquireSpinLock(&cdo_ext->lock, &oldIrql);
        if (cdo_ext->vg_count > 0) record = cdo_ext->vgs;
        KeReleaseSpinLock(&cdo_ext->lock, oldIrql);

        if (record) {
            for (size_t i = 0; i < vg.lv_count; i++) {
                if (!vg.lvs[i].mappable) continue;

                PDEVICE_OBJECT lv_pdo = NULL;
                NTSTATUS status = IoCreateDevice(work->cdo->DriverObject, sizeof(WINLVM_LV_EXT),
                                                 NULL, FILE_DEVICE_DISK,
                                                 FILE_DEVICE_SECURE_OPEN, FALSE, &lv_pdo);
                if (!NT_SUCCESS(status)) continue;

                WINLVM_LV_EXT *lv_ext = (WINLVM_LV_EXT *)lv_pdo->DeviceExtension;
                RtlZeroMemory(lv_ext, sizeof(*lv_ext));
                lv_ext->pdo = lv_pdo;
                lv_ext->vg = record;
                lv_ext->lv_index = i;
                lv_ext->backing_pdo = work->target_disk;

                lv_ext->byte_size = 0;
                for (size_t s = 0; s < vg.lvs[i].segment_count; s++) {
                    lv_ext->byte_size += vg.lvs[i].segments[s].extent_count * vg.extent_size_sectors * 512;
                }

                lv_pdo->Flags |= DO_DIRECT_IO;
                lv_pdo->Flags &= ~DO_DEVICE_INITIALIZING;

                /* Register volume interface for the Mount Manager. */
                UNICODE_STRING ifname;
                status = IoRegisterDeviceInterface(lv_pdo, &GUID_DEVINTERFACE_VOLUME, NULL, &ifname);
                if (NT_SUCCESS(status)) {
                    IoSetDeviceInterfaceState(&ifname, TRUE);
                    record->volume_ifname = ifname;
                }
            }
        }
    }

    ObDereferenceObject(work->target_disk);
    ObDereferenceObject(work->target_file);
    ExFreePoolWithTag(work, WINLVM_WIN_TAG);
}
