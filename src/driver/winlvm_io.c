/* winlvm_io.c — LV read/write: extent translation + IRP forwarding.
 *
 * Linear single-segment LVs pass through with a byte-offset shift (the
 * winmd pass-through pattern); multi-segment LVs fan out into per-segment
 * sub-IRPs. LUKS-backed PVs decrypt in place after the read completes —
 * the Phase 6 integration point. */
#include "winlvm.h"

static uint64_t LvSegmentMap(WINLVM_LV_EXT *ext, uint64_t lv_byte,
                             size_t *seg_out, uint64_t *off_in_seg) {
    winlvm_volume_group *vg = &ext->record->vg;
    winlvm_logical_volume *lv = &vg->lvs[ext->lv_index];
    uint64_t bytes_per_ext = vg->extent_size_sectors * 512ULL;
    for (size_t s = 0; s < lv->segment_count; s++) {
        uint64_t seg_bytes = lv->segments[s].extent_count * bytes_per_ext;
        if (lv_byte < seg_bytes) {
            *seg_out = s;
            *off_in_seg = lv_byte;
            return 1;
        }
        lv_byte -= seg_bytes;
    }
    return 0;
}

static uint64_t LvPvByteOffset(WINLVM_LV_EXT *ext, size_t seg_index,
                               uint64_t off_in_seg) {
    winlvm_volume_group *vg = &ext->record->vg;
    winlvm_logical_volume *lv = &vg->lvs[ext->lv_index];
    winlvm_segment *seg = &lv->segments[seg_index];
    uint64_t bytes_per_ext = vg->extent_size_sectors * 512ULL;
    /* single-PV groups only in this phase: stripes[0] names pv0 */
    uint64_t pv_ext = seg->stripes[0].start_extent;
    return vg->pvs[0].pe_start * 512ULL + pv_ext * bytes_per_ext + off_in_seg;
}

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

/* Pass-through: I/O entirely within one segment forwards with a shifted
 * byte offset (winmd's linear.c pattern). Parameters is a union, so
 * Read.ByteOffset and Write.ByteOffset alias the same storage. */
static NTSTATUS LvForwardIo(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

    size_t seg;
    uint64_t off_in_seg;
    if (!LvSegmentMap(ext, (uint64_t)irpSp->Parameters.Read.ByteOffset.QuadPart,
                      &seg, &off_in_seg)) {
        Irp->IoStatus.Status = STATUS_END_OF_FILE;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_END_OF_FILE;
    }

    IoCopyCurrentIrpStackLocationToNext(Irp);
    PIO_STACK_LOCATION nextSp = IoGetNextIrpStackLocation(Irp);
    nextSp->Parameters.Read.ByteOffset.QuadPart =
        (LONGLONG)LvPvByteOffset(ext, seg, off_in_seg);

    IoSetCompletionRoutine(Irp, WinLvmForwardCompletion, NULL, TRUE, TRUE, TRUE);
    return IoCallDriver(ext->backing_pdo, Irp);
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
     * written by us. */
    if (ext->record->vg.system_id[0] != '\0') {
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
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->backing_pdo, Irp);
}

NTSTATUS WinLvmShutdown(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    return WinLvmFlush(DeviceObject, Irp);
}
