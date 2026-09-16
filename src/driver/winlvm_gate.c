/* winlvm_gate.c — bounded cancel-safe quiesce gate. */
#include "winlvm.h"

static VOID GateInsert(PIO_CSQ Csq, PIRP Irp, PVOID Context) {
    WL_IO_GATE *gate = CONTAINING_RECORD(Csq, WL_IO_GATE, csq);
    UNREFERENCED_PARAMETER(Context);
    InsertTailList(&gate->queued, &Irp->Tail.Overlay.ListEntry);
    gate->queued_count++;
}
static VOID GateRemove(PIO_CSQ Csq, PIRP Irp) {
    WL_IO_GATE *gate = CONTAINING_RECORD(Csq, WL_IO_GATE, csq);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    gate->queued_count--;
}
static PIRP GatePeek(PIO_CSQ Csq, PIRP Irp, PVOID Context) {
    WL_IO_GATE *gate = CONTAINING_RECORD(Csq, WL_IO_GATE, csq);
    PLIST_ENTRY e;
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(Context);
    if (IsListEmpty(&gate->queued)) return NULL;
    e = gate->queued.Flink;
    return CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
}
static VOID GateAcquire(PIO_CSQ Csq, PKIRQL Irql) {
    WL_IO_GATE *gate = CONTAINING_RECORD(Csq, WL_IO_GATE, csq);
    KeAcquireSpinLock(&gate->lock, Irql);
}
static VOID GateRelease(PIO_CSQ Csq, KIRQL Irql) {
    WL_IO_GATE *gate = CONTAINING_RECORD(Csq, WL_IO_GATE, csq);
    KeReleaseSpinLock(&gate->lock, Irql);
}
static NTSTATUS GateDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    switch (sp->MajorFunction) {
    case IRP_MJ_READ: return WinLvmRead(DeviceObject, Irp);
    case IRP_MJ_WRITE: return WinLvmWrite(DeviceObject, Irp);
    case IRP_MJ_FLUSH_BUFFERS: return WinLvmFlush(DeviceObject, Irp);
    case IRP_MJ_SHUTDOWN: return WinLvmShutdown(DeviceObject, Irp);
    default:
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}
static VOID GateCancelled(PIO_CSQ Csq, PIRP Irp) {
    UNREFERENCED_PARAMETER(Csq);
    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}
static NTSTATUS GateInsertEx(PIO_CSQ Csq, PIRP Irp, PVOID Context) {
    WL_IO_GATE *gate = CONTAINING_RECORD(Csq, WL_IO_GATE, csq);
    UNREFERENCED_PARAMETER(Context);
    if (gate->queued_count >= WINLVM_IO_QUEUE_LIMIT) return STATUS_DEVICE_BUSY;
    InsertTailList(&gate->queued, &Irp->Tail.Overlay.ListEntry);
    gate->queued_count++;
    return STATUS_SUCCESS;
}

NTSTATUS WinLvmIoGateInit(WL_IO_GATE *gate) {
    NTSTATUS status;
    KeInitializeSpinLock(&gate->lock);
    InitializeListHead(&gate->queued);
    KeInitializeEvent(&gate->drained, NotificationEvent, TRUE);
    gate->state = WL_IO_RUNNING;
    gate->active = 0;
    gate->device = NULL;
    gate->dispatch = GateDispatch;
    gate->queued_count = 0;
    status = IoCsqInitializeEx(&gate->csq, GateInsertEx, GateRemove, GatePeek,
                               GateAcquire, GateRelease, GateCancelled);
    return status;
}

BOOLEAN WinLvmIoGateEnter(WL_IO_GATE *gate) {
    KIRQL irql;
    BOOLEAN admitted = FALSE;
    KeAcquireSpinLock(&gate->lock, &irql);
    if (gate->state == WL_IO_RUNNING) {
        gate->active++;
        KeClearEvent(&gate->drained);
        admitted = TRUE;
    }
    KeReleaseSpinLock(&gate->lock, irql);
    return admitted;
}

void WinLvmIoGateLeave(WL_IO_GATE *gate) {
    if (InterlockedDecrement(&gate->active) == 0) {
        KeSetEvent(&gate->drained, IO_NO_INCREMENT, FALSE);
    }
}

NTSTATUS WinLvmIoGateQueue(WL_IO_GATE *gate, PIRP Irp) {
    KIRQL irql;
    LONG state;
    KeAcquireSpinLock(&gate->lock, &irql);
    state = gate->state;
    KeReleaseSpinLock(&gate->lock, irql);
    if (state == WL_IO_REMOVING) return STATUS_DEVICE_REMOVED;
    if (state == WL_IO_RUNNING) return STATUS_RETRY;
    return IoCsqInsertIrpEx(&gate->csq, Irp, NULL, NULL);
}

NTSTATUS WinLvmIoGateBeginQuiesce(WL_IO_GATE *gate) {
    KIRQL irql;
    KeAcquireSpinLock(&gate->lock, &irql);
    if (gate->state != WL_IO_RUNNING) {
        KeReleaseSpinLock(&gate->lock, irql);
        return STATUS_INVALID_DEVICE_STATE;
    }
    gate->state = WL_IO_QUIESCING;
    KeReleaseSpinLock(&gate->lock, irql);
    if (gate->active != 0) return STATUS_PENDING;
    KeAcquireSpinLock(&gate->lock, &irql);
    gate->state = WL_IO_QUIESCED;
    KeReleaseSpinLock(&gate->lock, irql);
    return STATUS_SUCCESS;
}

void WinLvmIoGateResume(WL_IO_GATE *gate, PDEVICE_OBJECT DeviceObject) {
    PIRP irp;
    KIRQL irql;
    KeAcquireSpinLock(&gate->lock, &irql);
    if (gate->state == WL_IO_QUIESCED) gate->state = WL_IO_RUNNING;
    KeReleaseSpinLock(&gate->lock, irql);
    while ((irp = IoCsqRemoveNextIrp(&gate->csq, NULL)) != NULL) {
        if (gate->dispatch) gate->dispatch(DeviceObject, irp);
        else {
            irp->IoStatus.Status = STATUS_INVALID_DEVICE_STATE;
            irp->IoStatus.Information = 0;
            IoCompleteRequest(irp, IO_NO_INCREMENT);
        }
    }
}

void WinLvmIoGateRemove(WL_IO_GATE *gate) {
    PIRP irp;
    KIRQL irql;
    KeAcquireSpinLock(&gate->lock, &irql);
    gate->state = WL_IO_REMOVING;
    KeReleaseSpinLock(&gate->lock, irql);
    while ((irp = IoCsqRemoveNextIrp(&gate->csq, NULL)) != NULL) {
        irp->IoStatus.Status = STATUS_DEVICE_REMOVED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}
