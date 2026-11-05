/*
 * winlvm_subirp.c — Multi-piece sub-IRP allocation, partial MDL construction,
 *                   and completion aggregation for LV read/write fan-out.
 */
#include "winlvm.h"

static NTSTATUS WinLvmFanOutCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                                       PVOID Context) {
    UNREFERENCED_PARAMETER(DeviceObject);
    WL_IO_PIECE *pc = (WL_IO_PIECE *)Context;
    WL_IO_CTX *wctx = pc->ctx;
    NTSTATUS st = Irp->IoStatus.Status;

    if (!NT_SUCCESS(st)) {
        InterlockedCompareExchange((volatile LONG *)&wctx->status,
                                   (LONG)st, (LONG)STATUS_SUCCESS);
    } else {
        ULONG_PTR transferred = 0;
        if (wctx->major_function == IRP_MJ_READ) {
            if (pc->bounce) {
                if (Irp->IoStatus.Information > pc->bounce_skip) {
                    ULONG_PTR avail = Irp->IoStatus.Information - pc->bounce_skip;
                    if (avail > pc->len) avail = pc->len;
                    RtlCopyMemory(wctx->parent_base + pc->parent_off,
                                  pc->bounce_buf + pc->bounce_skip, (size_t)avail);
                    transferred = avail;
                }
            } else {
                transferred = Irp->IoStatus.Information;
            }
        } else {
            /* Storage class drivers completing IRP_MJ_WRITE with STATUS_SUCCESS
             * may report Information == 0 when the full transfer succeeded. */
            transferred = Irp->IoStatus.Information;
            if (transferred == 0) transferred = pc->len;
        }

        if (transferred < pc->len) {
            InterlockedCompareExchange((volatile LONG *)&wctx->status,
                                       (LONG)STATUS_END_OF_FILE,
                                       (LONG)STATUS_SUCCESS);
        } else {
            InterlockedAdd64((volatile LONG64 *)&wctx->bytes_copied,
                             (LONG64)transferred);
        }
    }

    if (Irp->MdlAddress) IoFreeMdl(Irp->MdlAddress);
    IoFreeIrp(Irp);
    if (pc->bounce_buf) ExFreePoolWithTag(pc->bounce_buf, WINLVM_WIN_TAG);

    if (InterlockedDecrement(&wctx->remaining) == 0) {
        PIRP parent = wctx->parent;
        WL_IO_GATE *gate = wctx->gate;
        NTSTATUS final = (NTSTATUS)wctx->status;
        parent->IoStatus.Status = final;
        parent->IoStatus.Information = NT_SUCCESS(final) ? (ULONG_PTR)wctx->bytes_copied : 0;
        ExFreePoolWithTag(wctx, WINLVM_WIN_TAG);
        IoCompleteRequest(parent, IO_NO_INCREMENT);
        WinLvmIoGateLeave(gate);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS LvFanOutIo(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    UCHAR major = irpSp->MajorFunction;

    uint64_t lv_byte = (uint64_t)irpSp->Parameters.Read.ByteOffset.QuadPart;
    uint32_t len = irpSp->Parameters.Read.Length;

    const lvmcore_volume_group *view = ext->record ? ext->record->view : NULL;
    if (!view || !g_bus_ext) {
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

    WL_IO_CTX *wctx = (WL_IO_CTX *)ExAllocatePoolZero(
        NonPagedPool, sizeof(WL_IO_CTX), WINLVM_WIN_TAG);
    if (!wctx) {
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    wctx->parent = Irp;
    wctx->parent_dev = DeviceObject;
    wctx->gate = &ext->io_gate;
    wctx->major_function = major;
    wctx->total_len = len;
    wctx->status = STATUS_SUCCESS;

    if (major == IRP_MJ_READ) {
        wctx->parent_base = (PUCHAR)MmGetSystemAddressForMdlSafe(
            Irp->MdlAddress, NormalPagePriority);
        if (!wctx->parent_base) {
            ExFreePoolWithTag(wctx, WINLVM_WIN_TAG);
            WinLvmIoGateLeave(&ext->io_gate);
            Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    size_t n = LvMapToPieces(ext, view, lv_byte, len, wctx->pieces, WL_MAX_READ_PIECES);
    if (n == 0) {
        ExFreePoolWithTag(wctx, WINLVM_WIN_TAG);
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }

    if (major == IRP_MJ_WRITE) {
        for (size_t i = 0; i < n; i++) {
            if (wctx->pieces[i].bounce) {
                ExFreePoolWithTag(wctx, WINLVM_WIN_TAG);
                WinLvmIoGateLeave(&ext->io_gate);
                Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
                Irp->IoStatus.Information = 0;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_INVALID_PARAMETER;
            }
        }
    }

    size_t built = 0;
    NTSTATUS fail = STATUS_SUCCESS;
    for (size_t i = 0; i < n && fail == STATUS_SUCCESS; i++) {
        WL_IO_PIECE *pc = &wctx->pieces[i];
        pc->ctx = wctx;
        PDEVICE_OBJECT backing = ext->backing_pdo[pc->backing_idx];
        if (!backing || !ext->backing_file[pc->backing_idx]) {
            fail = STATUS_DEVICE_NOT_CONNECTED;
            break;
        }

        PIRP sub = IoAllocateIrp((CCHAR)(backing->StackSize + 1), FALSE);
        if (!sub) {
            fail = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        PMDL mdl = NULL;
        if (pc->bounce) {
            pc->bounce_buf = (PUCHAR)ExAllocatePoolZero(
                NonPagedPool, pc->sub_len, WINLVM_WIN_TAG);
            if (!pc->bounce_buf) {
                IoFreeIrp(sub);
                fail = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            mdl = IoAllocateMdl(pc->bounce_buf, pc->sub_len, FALSE, FALSE, NULL);
            if (mdl) MmBuildMdlForNonPagedPool(mdl);
        } else {
            /* Partial MDL virtual address must be relative to the source MDL's
             * base virtual address (MmGetMdlVirtualAddress), not the system VA. */
            PVOID va = (PUCHAR)MmGetMdlVirtualAddress(Irp->MdlAddress) + pc->parent_off;
            mdl = IoAllocateMdl(va, pc->len, FALSE, FALSE, NULL);
            if (mdl) {
                IoBuildPartialMdl(Irp->MdlAddress, mdl, va, pc->len);
            }
        }
        if (!mdl) {
            if (pc->bounce_buf) ExFreePoolWithTag(pc->bounce_buf, WINLVM_WIN_TAG);
            IoFreeIrp(sub);
            fail = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        sub->MdlAddress = mdl;

        PIO_STACK_LOCATION nextSp = IoGetNextIrpStackLocation(sub);
        nextSp->MajorFunction = major;
        nextSp->FileObject = ext->backing_file[pc->backing_idx];
        if (major == IRP_MJ_READ) {
            nextSp->Parameters.Read.ByteOffset.QuadPart = (LONGLONG)pc->sub_offset;
            nextSp->Parameters.Read.Length = pc->sub_len;
        } else {
            nextSp->Parameters.Write.ByteOffset.QuadPart = (LONGLONG)pc->sub_offset;
            nextSp->Parameters.Write.Length = pc->sub_len;
        }
        IoSetCompletionRoutine(sub, WinLvmFanOutCompletion, pc, TRUE, TRUE, TRUE);
        pc->sub_irp = sub;
        built++;
    }

    if (fail != STATUS_SUCCESS) {
        for (size_t i = 0; i < built; i++) {
            WL_IO_PIECE *pc = &wctx->pieces[i];
            if (pc->sub_irp && pc->sub_irp->MdlAddress) {
                IoFreeMdl(pc->sub_irp->MdlAddress);
            }
            if (pc->bounce_buf) ExFreePoolWithTag(pc->bounce_buf, WINLVM_WIN_TAG);
            if (pc->sub_irp) IoFreeIrp(pc->sub_irp);
        }
        ExFreePoolWithTag(wctx, WINLVM_WIN_TAG);
        WinLvmIoGateLeave(&ext->io_gate);
        Irp->IoStatus.Status = fail;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return fail;
    }

    IoMarkIrpPending(Irp);
    InterlockedExchange(&wctx->remaining, (LONG)built);
    for (size_t i = 0; i < built; i++) {
        WL_IO_PIECE *pc = &wctx->pieces[i];
        IoCallDriver(ext->backing_pdo[pc->backing_idx], pc->sub_irp);
    }
    return STATUS_PENDING;
}
