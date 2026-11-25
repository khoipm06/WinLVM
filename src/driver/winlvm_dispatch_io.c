/*
 * winlvm_dispatch_io.c — WDM Read, Write, Flush, and Shutdown dispatch for LV PDOs.
 */
#include "winlvm.h"

static NTSTATUS WinLvmGateCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)Context;
    UNREFERENCED_PARAMETER(DeviceObject);
    if (Irp->PendingReturned) IoMarkIrpPending(Irp);
    WinLvmIoGateLeave(&ext->io_gate);
    return STATUS_SUCCESS;
}

static BOOLEAN WinLvmLvIsUnavailable(const WINLVM_LV_EXT *ext) {
    if (!ext || !ext->record || ext->record->dead ||
        ext->unavailable || ext->deactivating) {
        return TRUE;
    }
    if (!ext->record->view || ext->lv_index >= ext->record->view->lv_count) {
        return TRUE;
    }
    if (!wl_vg_lv_pv_complete(ext->record, ext->lv_index)) {
        return TRUE;
    }
    return FALSE;
}

/* Admit into the I/O gate or queue on quiesce. Returns STATUS_SUCCESS when
 * admitted (caller must leave gate), STATUS_PENDING when queued, or a
 * terminal failure status (IRP already completed). */
static NTSTATUS WinLvmAdmitOrQueueIo(WINLVM_LV_EXT *ext, PIRP Irp) {
    for (int attempt = 0; attempt < 4; attempt++) {
        if (WinLvmIoGateEnter(&ext->io_gate)) {
            return STATUS_SUCCESS;
        }
        NTSTATUS q = WinLvmIoGateQueue(&ext->io_gate, Irp);
        if (q == STATUS_SUCCESS) {
            return STATUS_PENDING;
        }
        if (q == STATUS_RETRY) {
            continue;
        }
        Irp->IoStatus.Status = q;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return q;
    }
    Irp->IoStatus.Status = STATUS_DEVICE_BUSY;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_DEVICE_BUSY;
}

static NTSTATUS LvForwardIo(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

    /* Parameters.Read and Parameters.Write alias ByteOffset and Length. */
    uint64_t lv_byte = (uint64_t)irpSp->Parameters.Read.ByteOffset.QuadPart;
    uint32_t len = irpSp->Parameters.Read.Length;

    const lvmcore_volume_group *view = ext->record ? ext->record->view : NULL;
    if (!view) {
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    if (!Irp->MdlAddress || MmGetMdlByteCount(Irp->MdlAddress) < len) {
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }

    wl_map_iter it;
    uint64_t pv_byte = 0;
    uint32_t pv_slot = 0;
    uint64_t piece_len = 0;
    wl_map_begin(&it, view, ext->record, ext->lv_index, lv_byte, len);
    int r = wl_map_next(&it, &pv_slot, &pv_byte, &piece_len);
    if (r < 0) {
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }

    if (r == 1 && piece_len == len) {
        size_t backing_idx = SIZE_MAX;
        for (size_t k = 0; k < ext->backing_count; k++) {
            if (ext->pv_slot[k] == pv_slot) {
                backing_idx = k;
                break;
            }
        }
        if (backing_idx != SIZE_MAX) {
            ULONG logical = ext->backing_logical[backing_idx];
            uint64_t mask = (uint64_t)logical - 1;
            PDEVICE_OBJECT backing = ext->backing_pdo[backing_idx];
            PFILE_OBJECT bfile = ext->backing_file[backing_idx];
            if (backing && bfile && logical &&
                Irp->CurrentLocation > backing->StackSize &&
                (pv_byte & mask) == 0 && (piece_len & mask) == 0) {
                IoCopyCurrentIrpStackLocationToNext(Irp);
                PIO_STACK_LOCATION nextSp = IoGetNextIrpStackLocation(Irp);
                nextSp->FileObject = bfile;
                nextSp->Parameters.Read.ByteOffset.QuadPart = (LONGLONG)pv_byte;
                nextSp->Parameters.Read.Length = len;

                IoSetCompletionRoutine(Irp, WinLvmGateCompletion, ext,
                                       TRUE, TRUE, TRUE);
                return IoCallDriver(backing, Irp);
            }
        }
    }

    return LvFanOutIo(DeviceObject, Irp);
}

NTSTATUS WinLvmRead(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    if (kind != WL_EXT_LV) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    NTSTATUS admit = WinLvmAdmitOrQueueIo(ext, Irp);
    if (admit != STATUS_SUCCESS) return admit;

    if (WinLvmLvIsUnavailable(ext)) {
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    if (irpSp->Parameters.Read.ByteOffset.QuadPart < 0) {
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }
    if (irpSp->Parameters.Read.Length == 0) {
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    ULONGLONG off = (ULONGLONG)irpSp->Parameters.Read.ByteOffset.QuadPart;
    if (off >= ext->byte_size) {
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_END_OF_FILE;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_END_OF_FILE;
    }
    ULONGLONG avail = ext->byte_size - off;
    if ((ULONGLONG)irpSp->Parameters.Read.Length > avail) {
        irpSp->Parameters.Read.Length = (ULONG)avail;
    }
    return LvForwardIo(DeviceObject, Irp);
}

NTSTATUS WinLvmWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    if (kind != WL_EXT_LV) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    NTSTATUS admit = WinLvmAdmitOrQueueIo(ext, Irp);
    if (admit != STATUS_SUCCESS) return admit;

    if (WinLvmLvIsUnavailable(ext)) {
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    const lvmcore_volume_group *view = ext->record->view;
    if (ext->read_only || view->read_only || view->system_id[0] != '\0' ||
        (ext->lv_index < view->lv_count && view->lvs[ext->lv_index].read_only)) {
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_MEDIA_WRITE_PROTECTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    if (irpSp->Parameters.Write.ByteOffset.QuadPart < 0) {
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }
    if (irpSp->Parameters.Write.Length == 0) {
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    ULONGLONG off = (ULONGLONG)irpSp->Parameters.Write.ByteOffset.QuadPart;
    if (off >= ext->byte_size) {
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_END_OF_FILE;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_END_OF_FILE;
    }
    ULONGLONG avail = ext->byte_size - off;
    if ((ULONGLONG)irpSp->Parameters.Write.Length > avail) {
        irpSp->Parameters.Write.Length = (ULONG)avail;
    }
    return LvForwardIo(DeviceObject, Irp);
}

typedef struct {
    PIRP              parent;
    WL_IO_GATE       *gate;
    volatile LONG     remaining;
    volatile NTSTATUS status;
    PIRP              sub_irps[WL_VG_MAX_PVS];
} WL_STORAGE_FLUSH_CTX;

static void WinLvmCompleteFlushParent(WL_STORAGE_FLUSH_CTX *fctx) {
    PIRP parent = fctx->parent;
    WL_IO_GATE *gate = fctx->gate;
    NTSTATUS final = (NTSTATUS)fctx->status;
    ExFreePoolWithTag(fctx, WINLVM_WIN_TAG);
    parent->IoStatus.Status = final;
    parent->IoStatus.Information = 0;
    IoCompleteRequest(parent, IO_NO_INCREMENT);
    WinLvmIoGateLeave(gate);
}

static NTSTATUS WinLvmStorageFlushCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    UNREFERENCED_PARAMETER(DeviceObject);
    WL_STORAGE_FLUSH_CTX *fctx = (WL_STORAGE_FLUSH_CTX *)Context;
    NTSTATUS st = Irp->IoStatus.Status;
    if (!NT_SUCCESS(st)) {
        InterlockedCompareExchange((volatile LONG *)&fctx->status,
                                   (LONG)st, (LONG)STATUS_SUCCESS);
    }
    IoFreeIrp(Irp);
    if (InterlockedDecrement(&fctx->remaining) == 0) {
        WinLvmCompleteFlushParent(fctx);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS WinLvmFlush(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    if (kind != WL_EXT_LV) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    NTSTATUS admit = WinLvmAdmitOrQueueIo(ext, Irp);
    if (admit != STATUS_SUCCESS) return admit;

    if (WinLvmLvIsUnavailable(ext) || ext->backing_count == 0) {
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    if (ext->backing_count > 1 ||
        (ext->backing_pdo[0] && Irp->CurrentLocation <= ext->backing_pdo[0]->StackSize)) {
        WL_STORAGE_FLUSH_CTX *fctx = (WL_STORAGE_FLUSH_CTX *)ExAllocatePoolZero(
            NonPagedPool, sizeof(WL_STORAGE_FLUSH_CTX), WINLVM_WIN_TAG);
        if (!fctx) {
            WinLvmIoGateLeave(&ext->io_gate);
            Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        fctx->parent = Irp;
        fctx->gate = &ext->io_gate;
        fctx->status = STATUS_SUCCESS;

        UCHAR major = IoGetCurrentIrpStackLocation(Irp)->MajorFunction;
        size_t built = 0;
        NTSTATUS fail = STATUS_SUCCESS;
        for (size_t k = 0; k < ext->backing_count; k++) {
            PDEVICE_OBJECT backing = ext->backing_pdo[k];
            if (!backing || !ext->backing_file[k]) {
                fail = STATUS_DEVICE_NOT_CONNECTED;
                break;
            }
            PIRP sub = IoAllocateIrp((CCHAR)(backing->StackSize + 1), FALSE);
            if (!sub) {
                fail = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            PIO_STACK_LOCATION nextSp = IoGetNextIrpStackLocation(sub);
            nextSp->MajorFunction = major;
            nextSp->FileObject = ext->backing_file[k];
            IoSetCompletionRoutine(sub, WinLvmStorageFlushCompletion, fctx,
                                   TRUE, TRUE, TRUE);
            fctx->sub_irps[built++] = sub;
        }
        if (fail != STATUS_SUCCESS) {
            for (size_t k = 0; k < built; k++) IoFreeIrp(fctx->sub_irps[k]);
            ExFreePoolWithTag(fctx, WINLVM_WIN_TAG);
            WinLvmIoGateLeave(&ext->io_gate);
            Irp->IoStatus.Status = fail;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return fail;
        }

        /* Hold a +1 dispatch reference so fctx and ext->io_gate remain valid
         * across the entire submission loop even on synchronous completions. */
        IoMarkIrpPending(Irp);
        InterlockedExchange(&fctx->remaining, (LONG)built + 1);
        for (size_t k = 0; k < built; k++) {
            (void)IoCallDriver(ext->backing_pdo[k], fctx->sub_irps[k]);
        }
        if (InterlockedDecrement(&fctx->remaining) == 0) {
            WinLvmCompleteFlushParent(fctx);
        }
        return STATUS_PENDING;
    }

    if (!ext->backing_pdo[0] || !ext->backing_file[0]) {
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoGetNextIrpStackLocation(Irp)->FileObject = ext->backing_file[0];
    IoSetCompletionRoutine(Irp, WinLvmGateCompletion, ext, TRUE, TRUE, TRUE);
    return IoCallDriver(ext->backing_pdo[0], Irp);
}

NTSTATUS WinLvmShutdown(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    return WinLvmFlush(DeviceObject, Irp);
}
