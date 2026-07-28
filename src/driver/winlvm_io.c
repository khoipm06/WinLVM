/* winlvm_io.c */
#include "winlvm.h"

extern PDEVICE_OBJECT g_cdo;

typedef struct {
    KEVENT event;
    IO_STATUS_BLOCK io_status;
} WINLVM_READ_CONTEXT;

static NTSTATUS WinLvmReadCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    WINLVM_READ_CONTEXT *ctx = (WINLVM_READ_CONTEXT *)Context;
    ctx->io_status = Irp->IoStatus;
    KeSetEvent(&ctx->event, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

size_t winlvm_disk_source_read(void *impl, uint64_t off, void *buf, size_t len) {
    WINLVM_DISK_SOURCE *src = (WINLVM_DISK_SOURCE *)impl;

    LARGE_INTEGER offset;
    offset.QuadPart = off;

    WINLVM_READ_CONTEXT ctx;
    KeInitializeEvent(&ctx.event, NotificationEvent, FALSE);

    PIRP irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, src->disk_pdo, buf, (ULONG)len, &offset, &ctx.event, &ctx.io_status);
    if (!irp) return 0;

    PIO_STACK_LOCATION irpSp = IoGetNextIrpStackLocation(irp);
    irpSp->FileObject = src->file;

    IoSetCompletionRoutine(irp, WinLvmReadCompletion, &ctx, TRUE, TRUE, TRUE);

    NTSTATUS status = IoCallDriver(src->disk_pdo, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&ctx.event, Executive, KernelMode, FALSE, NULL);
        status = ctx.io_status.Status;
    }

    /* We took ownership of completion via STATUS_MORE_PROCESSING_REQUIRED */
    IoFreeIrp(irp);

    if (NT_SUCCESS(status)) {
        return ctx.io_status.Information;
    }
    return 0;
}

NTSTATUS WinLvmRead(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    if (DeviceObject == g_cdo) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
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

    if ((ULONGLONG)irpSp->Parameters.Read.ByteOffset.QuadPart + irpSp->Parameters.Read.Length > ext->byte_size) {
        irpSp->Parameters.Read.Length = (ULONG)(ext->byte_size - irpSp->Parameters.Read.ByteOffset.QuadPart);
    }

    winlvm_logical_volume *lv = &ext->vg->vg.lvs[ext->lv_index];

    if (lv->segment_count == 1 && lv->segments[0].type == WINLVM_SEG_STRIPED) {
        PIO_STACK_LOCATION nextSp = IoGetNextIrpStackLocation(Irp);
        *nextSp = *irpSp;

        uint64_t pv_ext_offset = lv->segments[0].stripes[0].start_extent;
        uint64_t bytes_per_ext = ext->vg->vg.extent_size_sectors * 512;
        uint64_t pv_byte_offset = ext->vg->vg.pvs[0].pe_start * 512 + (pv_ext_offset * bytes_per_ext);

        nextSp->Parameters.Read.ByteOffset.QuadPart += pv_byte_offset;

        IoSetCompletionRoutine(Irp, NULL, NULL, FALSE, FALSE, FALSE);
        return IoCallDriver(ext->backing_pdo, Irp);
    }

    Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NOT_IMPLEMENTED;
}
