/* winlvm_io.c — LV read/write: extent translation + IRP forwarding.
 *
 * Extent→PV mapping is delegated to the core mapper (wl_map_begin/next):
 * per-PV pe_start (heterogeneous PVs), striped chunk math, overflow checks
 * and dead-PV refusal all live there and fail closed. Single aligned piece
 * → forwarded parent IRP (fast path). Anything else → read fan-out: one
 * sub-IRP per mapped piece (Task 9 multi-PV, W2), completed exactly once. */
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

static NTSTATUS WinLvmGateCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)Context;
    if (Irp->PendingReturned) IoMarkIrpPending(Irp);
    WinLvmIoGateLeave(&ext->io_gate);
    UNREFERENCED_PARAMETER(DeviceObject);
    return STATUS_SUCCESS;
}

static NTSTATUS WinLvmForwardCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Context);
    if (Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }
    return STATUS_SUCCESS;
}

/* Maps [lv_byte, lv_byte + len) to pieces and fills pieces[]. Returns piece
 * count, or 0 on refusal/overflow (caller completes the parent IRP). */
static size_t LvMapToPieces(WINLVM_LV_EXT *ext,
                            const lvmcore_volume_group *view,
                            uint64_t lv_byte, uint32_t len,
                            WL_READ_PIECE *pieces, size_t cap) {
    wl_map_iter it;
    wl_map_begin(&it, view, ext->record, ext->lv_index, lv_byte, len);
    size_t n = 0;
    uint32_t parent_off = 0;
    for (;;) {
        uint32_t pv_slot;
        uint64_t pv_byte, piece_len;
        int r = wl_map_next(&it, &pv_slot, &pv_byte, &piece_len);
        if (r == 0) break;
        if (r < 0 || n >= cap) return 0; /* refusal / too many pieces: fail whole */
        /* mapper pieces never exceed len, but guard anyway */
        if (piece_len == 0 || piece_len > (uint64_t)len ||
            parent_off + (uint32_t)piece_len > len) return 0;

        size_t backing_idx = SIZE_MAX;
        for (size_t k = 0; k < ext->backing_count; k++) {
            if (ext->pv_slot[k] == pv_slot) {
                backing_idx = k;
                break;
            }
        }
        if (backing_idx == SIZE_MAX) return 0; /* map says PV we don't back */
        ULONG logical = ext->backing_logical[backing_idx];
        if (logical == 0 || (logical & (logical - 1)) != 0) return 0;

        WL_READ_PIECE *pc = &pieces[n];
        RtlZeroMemory(pc, sizeof(*pc));
        pc->pv_slot = pv_slot;
        pc->backing_idx = (uint32_t)backing_idx;
        pc->len = (uint32_t)piece_len;
        pc->parent_off = parent_off;
        pc->pv_byte = pv_byte;
        pc->logical = logical;

        uint64_t mask = (uint64_t)logical - 1;
        if ((pv_byte & mask) == 0 && (piece_len & mask) == 0) {
            pc->bounce = 0;
            pc->sub_offset = pv_byte;
            pc->sub_len = (uint32_t)piece_len;
        } else {
            uint64_t win_start = pv_byte & ~mask;
            uint64_t win_end;
            if (piece_len > UINT64_MAX - pv_byte) return 0;
            if (mask > UINT64_MAX - (pv_byte + piece_len)) return 0;
            win_end = (pv_byte + piece_len + mask) & ~mask;
            if (win_end - win_start > 0xFFFFFFFFULL) return 0;
            pc->bounce = 1;
            pc->sub_offset = win_start;
            pc->sub_len = (uint32_t)(win_end - win_start);
            pc->bounce_skip = (uint32_t)(pv_byte - win_start);
        }
        if ((uint64_t)pc->parent_off + pc->len > len) return 0;
        parent_off += pc->len;
        n++;
    }
    return n;
}

/* Completion for ONE fan-out sub-IRP. Frees the sub-IRP/MDL/bounce, copies
 * a bounce piece into the parent buffer, and on the LAST in-flight piece
 * completes the parent IRP exactly once and returns the ctx to the pool.
 * Any failure latches the first status (never a partial
 * success: Information = 0 on failure). */
static NTSTATUS WinLvmFanOutCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                                       PVOID Context) {
    UNREFERENCED_PARAMETER(DeviceObject);
    WL_READ_PIECE *pc = (WL_READ_PIECE *)Context;
    WL_READ_CTX *wctx = pc->ctx;
    NTSTATUS st = Irp->IoStatus.Status;

    if (!NT_SUCCESS(st)) {
        InterlockedCompareExchange((volatile LONG *)&wctx->status, (LONG)st,
                                   (LONG)STATUS_SUCCESS);
    } else if (pc->bounce && pc->iosb.Information > pc->bounce_skip) {
        ULONG_PTR avail = pc->iosb.Information - pc->bounce_skip;
        if (avail > pc->len) avail = pc->len;
        RtlCopyMemory(wctx->parent_base + pc->parent_off,
                      pc->bounce_buf + pc->bounce_skip, (size_t)avail);
    }

    if (Irp->MdlAddress) IoFreeMdl(Irp->MdlAddress);
    IoFreeIrp(Irp);
    if (pc->bounce_buf) ExFreePoolWithTag(pc->bounce_buf, WINLVM_WIN_TAG);

    if (InterlockedDecrement(&wctx->remaining) == 0) {
        PIRP parent = wctx->parent;
        NTSTATUS final = (NTSTATUS)wctx->status;
        parent->IoStatus.Status = final;
        parent->IoStatus.Information = NT_SUCCESS(final) ? wctx->total_len : 0;
        IoCompleteRequest(parent, IO_NO_INCREMENT);
        WinLvmIoGateLeave(wctx->gate);
        ExFreePoolWithTag(wctx, WINLVM_WIN_TAG);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Read fan-out: map the request, build a sub-IRP per piece (each sized for
 * ITS backing's stack — ISSUE-21: multi-PV means the deepest member governs
 * the child PDO, each sub-IRP the piece's own target), then send. Aligned
 * pieces MDL directly over the parent buffer slice; unaligned pieces read
 * an aligned bounce window and copy back in completion. */
static NTSTATUS LvFanOutRead(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

    uint64_t lv_byte = (uint64_t)irpSp->Parameters.Read.ByteOffset.QuadPart;
    uint32_t len = irpSp->Parameters.Read.Length;

    const lvmcore_volume_group *view = ext->record->view;
    if (!view || !g_bus_ext) {
        Irp->IoStatus.Status = STATUS_DEVICE_REMOVED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_REMOVED;
    }
    if (!Irp->MdlAddress) { /* DO_DIRECT_IO child: must have a locked MDL */
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }

    WL_READ_CTX *wctx = (WL_READ_CTX *)ExAllocatePoolWithTag(
        NonPagedPool, sizeof(WL_READ_CTX), WINLVM_WIN_TAG);
    if (!wctx) {
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(wctx, sizeof(*wctx));
    wctx->parent = Irp;
    wctx->parent_dev = DeviceObject;
    wctx->gate = &ext->io_gate;
    wctx->parent_base = (PUCHAR)MmGetSystemAddressForMdlSafe(
        Irp->MdlAddress, NormalPagePriority);
    wctx->total_len = len;
    wctx->status = STATUS_SUCCESS;
    if (!wctx->parent_base) {
        ExFreePoolWithTag(wctx, WINLVM_WIN_TAG);
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }

    size_t n = LvMapToPieces(ext, view, lv_byte, len, wctx->pieces,
                             WL_MAX_READ_PIECES);
    if (n == 0) {
        ExFreePoolWithTag(wctx, WINLVM_WIN_TAG);
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }

    /* Build ALL sub-IRPs first: any failed build fails the WHOLE request
     * (never a partial fan-out) — undo what was built, complete the parent
     * with the failure, return the ctx. */
    size_t built = 0;
    NTSTATUS fail = STATUS_SUCCESS;
    for (size_t i = 0; i < n && fail == STATUS_SUCCESS; i++) {
        WL_READ_PIECE *pc = &wctx->pieces[i];
        pc->ctx = wctx;
        PDEVICE_OBJECT backing = ext->backing_pdo[pc->backing_idx];
        if (!backing || !ext->backing_file[pc->backing_idx]) {
            fail = STATUS_DEVICE_REMOVED;
            break;
        }
        /* fresh IRP: one stack location consumed by us, the rest by the
         * backing's whole stack — ISSUE-21 */
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
            /* parent buffer slice: partial MDL over the already-locked
             * parent MDL — no new locking, exactly the aligned request.
             * IoAllocateMdl allocates the target MDL; IoBuildPartialMdl
             * (VOID return, WDK contract) maps the source slice into it. */
            mdl = IoAllocateMdl(NULL, pc->len, FALSE, FALSE, NULL);
            if (mdl)
                IoBuildPartialMdl(Irp->MdlAddress, mdl,
                                  (PUCHAR)wctx->parent_base + pc->parent_off,
                                  pc->len);
        }
        if (!mdl) {
            if (pc->bounce_buf) ExFreePoolWithTag(pc->bounce_buf, WINLVM_WIN_TAG);
            IoFreeIrp(sub);
            fail = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        sub->MdlAddress = mdl;

        PIO_STACK_LOCATION nextSp = IoGetNextIrpStackLocation(sub);
        nextSp->MajorFunction = IRP_MJ_READ;
        nextSp->FileObject = ext->backing_file[pc->backing_idx];
        nextSp->Parameters.Read.ByteOffset.QuadPart = (LONGLONG)pc->sub_offset;
        nextSp->Parameters.Read.Length = pc->sub_len;
        sub->UserIosb = &pc->iosb;
        IoSetCompletionRoutine(sub, WinLvmFanOutCompletion, pc, TRUE, TRUE, TRUE);
        pc->sub_irp = sub;
        built++;
    }

    if (fail != STATUS_SUCCESS) {
        for (size_t i = 0; i < built; i++) {
            WL_READ_PIECE *pc = &wctx->pieces[i];
            if (pc->sub_irp && pc->sub_irp->MdlAddress)
                IoFreeMdl(pc->sub_irp->MdlAddress);
            if (pc->bounce_buf) ExFreePoolWithTag(pc->bounce_buf, WINLVM_WIN_TAG);
            if (pc->sub_irp) IoFreeIrp(pc->sub_irp);
        }
        ExFreePoolWithTag(wctx, WINLVM_WIN_TAG);
        Irp->IoStatus.Status = fail;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return fail;
    }

    InterlockedExchange(&wctx->remaining, (LONG)built);
    for (size_t i = 0; i < built; i++) {
        WL_READ_PIECE *pc = &wctx->pieces[i];
        IoCallDriver(ext->backing_pdo[pc->backing_idx], pc->sub_irp);
    }
    /* Parent IRP is completed by the last fan-out completion; the ctx is
     * freed there too — never touch wctx after this point. */
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Fast path + fan-out dispatch. One fully-mapped aligned piece → the
 * existing VM-verified forwarded-IRP path; anything else → LvFanOutRead. */
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
    int r = wl_map_next(&it, &pv_slot, &pv_byte, &piece_len);
    if (r < 0) {
        /* unmappable: fail whole — a partial forward would silently serve
         * corrupt data. */
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }

    if (r == 1 && piece_len == len) {
        /* Single piece. The backing slice exists only when it is aligned to
         * that member's logical sector: forwarded IRPs cannot bounce. */
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
            if (logical && (pv_byte & mask) == 0 && (piece_len & mask) == 0) {
                /* Child-PDO-owned reference (ObReferenceObject at create) —
                 * unlike a pv_tag deref, removal cannot free it while this
                 * child lives. */
                PDEVICE_OBJECT backing = ext->backing_pdo[backing_idx];

                IoCopyCurrentIrpStackLocationToNext(Irp);
                PIO_STACK_LOCATION nextSp = IoGetNextIrpStackLocation(Irp);
                nextSp->Parameters.Read.ByteOffset.QuadPart = (LONGLONG)pv_byte;

                IoSetCompletionRoutine(Irp, WinLvmGateCompletion, ext,
                                       TRUE, TRUE, TRUE);
                return IoCallDriver(backing, Irp);
            }
        }
    }

    /* Multi-piece (cross-segment/PV/stripe), or a single piece that needs
     * bouncing: fan out with per-piece sub-IRPs. */
    return LvFanOutRead(DeviceObject, Irp);
}

NTSTATUS WinLvmRead(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    if (!WinLvmIoGateEnter(&ext->io_gate)) {
        NTSTATUS q = WinLvmIoGateQueue(&ext->io_gate, Irp);
        if (q == STATUS_SUCCESS) return STATUS_PENDING;
        Irp->IoStatus.Status = q == STATUS_RETRY ? STATUS_DEVICE_BUSY : q;
        Irp->IoStatus.Information = 0; IoCompleteRequest(Irp, IO_NO_INCREMENT); return Irp->IoStatus.Status;
    }
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
    NTSTATUS status = LvForwardIo(DeviceObject, Irp);
    if (status != STATUS_MORE_PROCESSING_REQUIRED) WinLvmIoGateLeave(&ext->io_gate);
    return status;
}

NTSTATUS WinLvmWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    if (ext->kind != WL_EXT_LV || ext->record->dead) {
        Irp->IoStatus.Status = STATUS_DEVICE_REMOVED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        WinLvmIoGateLeave(&ext->io_gate);
        return STATUS_DEVICE_REMOVED;
    }
    /* Write fan-out is W3 — a multi-PV LV fails closed rather than write
     * one stripe and corrupt the rest. Single-backing LVs (backing_count
     * == 1) keep the full write path. */
    if (ext->backing_count > 1) {
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
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
    NTSTATUS status = LvForwardIo(DeviceObject, Irp);
    if (status != STATUS_MORE_PROCESSING_REQUIRED) WinLvmIoGateLeave(&ext->io_gate);
    return status;
}

NTSTATUS WinLvmFlush(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    if (!WinLvmIoGateEnter(&ext->io_gate)) {
        NTSTATUS q = WinLvmIoGateQueue(&ext->io_gate, Irp);
        if (q == STATUS_SUCCESS) return STATUS_PENDING;
        Irp->IoStatus.Status = q == STATUS_RETRY ? STATUS_DEVICE_BUSY : q;
        Irp->IoStatus.Information = 0; IoCompleteRequest(Irp, IO_NO_INCREMENT); return Irp->IoStatus.Status;
    }
    if (ext->kind != WL_EXT_LV) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        WinLvmIoGateLeave(&ext->io_gate);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    /* Surprise-removed backing disk: fail closed like WinLvmRead/Write —
     * forwarding into the removed disk stack (or through the freed record)
     * was the delayed-wedge path (2026-09-14 NMI repro). */
    if (ext->record->dead) {
        Irp->IoStatus.Status = STATUS_DEVICE_REMOVED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        WinLvmIoGateLeave(&ext->io_gate);
        return STATUS_DEVICE_REMOVED;
    }
    /* Forward the flush to the first backing member. A multi-PV LV has no
     * data writes (blocked above), so flushing one member is sufficient for
     * the read-path phase; W3's write fan-out adds per-member flushes. */
    IoSkipCurrentIrpStackLocation(Irp);
    IoSetCompletionRoutine(Irp, WinLvmGateCompletion, ext, TRUE, TRUE, TRUE);
    return IoCallDriver(ext->backing_pdo[0], Irp);
}

NTSTATUS WinLvmShutdown(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    return WinLvmFlush(DeviceObject, Irp);
}
