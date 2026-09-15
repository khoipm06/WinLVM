/* winlvm_io.c — LV read/write: extent translation + IRP forwarding.
 *
 * Extent→PV mapping is delegated to the core mapper (wl_map_begin/next):
 * per-PV pe_start (heterogeneous PVs), striped chunk math, overflow checks
 * and dead-PV refusal all live there and fail closed. The v22 pass-through
 * forwards ONE backing IRP, so a request spanning a segment/PV boundary is
 * refused whole (multi-piece fan-out is the W3 write path). */
#include "winlvm.h"

size_t winlvm_disk_source_read(void *impl, uint64_t off, void *buf, size_t len) {
    WINLVM_DISK_SOURCE *src = (WINLVM_DISK_SOURCE *)impl;

    /* The storage stack rejects IRP_MJ_READs whose byte offset or length is
     * not a sector multiple — the 2026-09-11 "no LV children" bug: LVM2's
     * commit_size is an arbitrary byte count (1066 on the corpus PV), so
     * read_metadata_text's unaligned read failed with
     * STATUS_INVALID_PARAMETER and the probe silently gave up. Read an
     * aligned window and copy the requested part out. */
    if (len == 0 || off + len < off) return 0;
    uint64_t start = off & ~511ULL;
    uint64_t end = (off + len + 511ULL) & ~511ULL;
    size_t window = (size_t)(end - start);
    size_t skip = (size_t)(off - start);

    unsigned char *bounce = (unsigned char *)ExAllocatePoolZero(
        NonPagedPool, window, WINLVM_WIN_TAG);
    if (!bounce) return 0;

    LARGE_INTEGER offset;
    offset.QuadPart = (LONGLONG)start;

    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IO_STATUS_BLOCK io_status = {0};

    PIRP irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, src->disk_pdo, bounce,
                                            (ULONG)window, &offset, &event,
                                            &io_status);
    if (!irp) {
        ExFreePoolWithTag(bounce, WINLVM_WIN_TAG);
        return 0;
    }

    PIO_STACK_LOCATION irpSp = IoGetNextIrpStackLocation(irp);
    irpSp->FileObject = src->file;

    NTSTATUS status = IoCallDriver(src->disk_pdo, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = io_status.Status;
    }
    /* IoBuildSynchronousFsdRequest IRPs are owned by the caller and freed
     * by the I/O manager; do not IoFreeIrp here. */

    size_t got = 0;
    if (NT_SUCCESS(status) && (size_t)io_status.Information > skip) {
        size_t avail = (size_t)io_status.Information - skip;
        if (avail > len) avail = len;
        RtlCopyMemory(buf, bounce + skip, avail);
        got = avail;
    }
    ExFreePoolWithTag(bounce, WINLVM_WIN_TAG);
    return got;
}

static NTSTATUS WinLvmForwardCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Context);
    if (Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }
    return STATUS_SUCCESS;
}

/* Pass-through: one backing IRP for the whole request. The mapper refuses
 * (err) when the request crosses a segment/PV boundary or touches a dead
 * PV — the WHOLE IRP fails, never a partial map. Backing offsets are the
 * mapper's per-PV pe_start + extent + position math. */
static NTSTATUS LvForwardIo(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

    /* Parameters is a union: Read.ByteOffset aliases Write.ByteOffset,
     * Read.Length aliases Write.Length — one mapper serves both. */
    uint64_t lv_byte = (uint64_t)irpSp->Parameters.Read.ByteOffset.QuadPart;
    uint32_t len = irpSp->Parameters.Read.Length;

    const lvmcore_volume_group *view = ext->record->view
        ? ext->record->view : NULL;
    if (!view) {
        Irp->IoStatus.Status = STATUS_DEVICE_REMOVED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_REMOVED;
    }

    wl_map_iter it;
    uint64_t pv_byte;
    uint32_t pv_slot;
    uint64_t piece_len;
    wl_map_begin(&it, view, ext->record, ext->lv_index, lv_byte, len);
    if (wl_map_next(&it, &pv_slot, &pv_byte, &piece_len) != 1 ||
        piece_len != len) {
        /* Multi-piece (cross-segment/PV/stripe) or unmappable: fail whole —
         * a partial forward would silently serve corrupt data. */
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }

    /* v22 PDOs are single-backing (CreateLvPdo resolved ONE record slot
     * every stripe lives on), so the mapper's pv_slot is always the PDO's
     * slot and ext->backing_pdo is the correct target. That reference is
     * child-PDO-owned (ObReferenceObject at create) — unlike a pv_tag
     * deref, removal cannot free it while this child lives. */
    PDEVICE_OBJECT backing = ext->backing_pdo;

    IoCopyCurrentIrpStackLocationToNext(Irp);
    PIO_STACK_LOCATION nextSp = IoGetNextIrpStackLocation(Irp);
    nextSp->Parameters.Read.ByteOffset.QuadPart = (LONGLONG)pv_byte;

    IoSetCompletionRoutine(Irp, WinLvmForwardCompletion, NULL, TRUE, TRUE, TRUE);
    return IoCallDriver(backing, Irp);
}

NTSTATUS WinLvmRead(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    if (ext->kind != WL_EXT_LV || ext->record->dead) {
        Irp->IoStatus.Status = STATUS_DEVICE_REMOVED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_REMOVED;
    }
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    if (irpSp->Parameters.Read.Length == 0) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
    if ((ULONGLONG)irpSp->Parameters.Read.ByteOffset.QuadPart >= ext->byte_size) {
        Irp->IoStatus.Status = STATUS_END_OF_FILE;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_END_OF_FILE;
    }
    if ((ULONGLONG)irpSp->Parameters.Read.ByteOffset.QuadPart
        + irpSp->Parameters.Read.Length > ext->byte_size) {
        irpSp->Parameters.Read.Length =
            (ULONG)(ext->byte_size - irpSp->Parameters.Read.ByteOffset.QuadPart);
    }
    return LvForwardIo(DeviceObject, Irp);
}

NTSTATUS WinLvmWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    if (ext->kind != WL_EXT_LV || ext->record->dead) {
        Irp->IoStatus.Status = STATUS_DEVICE_REMOVED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_REMOVED;
    }
    /* Honor LVM2 system_id: a VG owned by another host must not be
     * written by us. (view is pinned by the child-PDO ref — refs == 0
     * reconciles refuse to swap while this PDO lives.) */
    const lvmcore_volume_group *view = ext->record->view;
    if (!view) {
        Irp->IoStatus.Status = STATUS_DEVICE_REMOVED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_REMOVED;
    }
    if (view->system_id[0] != '\0') {
        Irp->IoStatus.Status = STATUS_MEDIA_WRITE_PROTECTED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
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
    if ((ULONGLONG)irpSp->Parameters.Write.ByteOffset.QuadPart
        + irpSp->Parameters.Write.Length > ext->byte_size) {
        irpSp->Parameters.Write.Length =
            (ULONG)(ext->byte_size - irpSp->Parameters.Write.ByteOffset.QuadPart);
    }
    return LvForwardIo(DeviceObject, Irp);
}

NTSTATUS WinLvmFlush(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    if (ext->kind != WL_EXT_LV) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    /* Surprise-removed backing disk: fail closed like WinLvmRead/Write —
     * forwarding into the removed disk stack (or through the freed record)
     * was the delayed-wedge path (2026-09-14 NMI repro). */
    if (ext->record->dead) {
        Irp->IoStatus.Status = STATUS_DEVICE_REMOVED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_REMOVED;
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->backing_pdo, Irp);
}

NTSTATUS WinLvmShutdown(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    return WinLvmFlush(DeviceObject, Irp);
}
