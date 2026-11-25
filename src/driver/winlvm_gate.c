/*
 * winlvm_gate.c — Cancel-safe I/O admission and quiesce gate.
 *
 * All transitions of state, active count, drained event, and the CSQ list
 * run under gate->lock so admission, quiesce drain, resume, and removal
 * cannot race or strand queued IRPs.
 */
#include "winlvm.h"

static VOID GateRemove(PIO_CSQ Csq, PIRP Irp) {
    WL_IO_GATE *gate = CONTAINING_RECORD(Csq, WL_IO_GATE, csq);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    if (gate->queued_count > 0) gate->queued_count--;
}

static PIRP GatePeek(PIO_CSQ Csq, PIRP Irp, PVOID Context) {
    WL_IO_GATE *gate = CONTAINING_RECORD(Csq, WL_IO_GATE, csq);
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(Context);
    if (IsListEmpty(&gate->queued)) return NULL;
    return CONTAINING_RECORD(gate->queued.Flink, IRP, Tail.Overlay.ListEntry);
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
    case IRP_MJ_READ:
        return WinLvmRead(DeviceObject, Irp);
    case IRP_MJ_WRITE:
        return WinLvmWrite(DeviceObject, Irp);
    case IRP_MJ_FLUSH_BUFFERS:
        return WinLvmFlush(DeviceObject, Irp);
    case IRP_MJ_SHUTDOWN:
        return WinLvmShutdown(DeviceObject, Irp);
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

/* Runs under gate->lock via IoCsqInsertIrpEx: state check, IoMarkIrpPending,
 * and list insertion are atomic with respect to Resume and Remove. */
static NTSTATUS GateInsertEx(PIO_CSQ Csq, PIRP Irp, PVOID Context) {
    WL_IO_GATE *gate = CONTAINING_RECORD(Csq, WL_IO_GATE, csq);
    UNREFERENCED_PARAMETER(Context);
    if (gate->state == WL_IO_REMOVING) return STATUS_DEVICE_REMOVED;
    if (gate->state == WL_IO_RUNNING) return STATUS_RETRY;
    if (gate->queued_count >= WINLVM_IO_QUEUE_LIMIT) return STATUS_DEVICE_BUSY;
    IoMarkIrpPending(Irp);
    InsertTailList(&gate->queued, &Irp->Tail.Overlay.ListEntry);
    gate->queued_count++;
    return STATUS_SUCCESS;
}

NTSTATUS WinLvmIoGateInit(WL_IO_GATE *gate) {
    KeInitializeSpinLock(&gate->lock);
    InitializeListHead(&gate->queued);
    KeInitializeEvent(&gate->drained, NotificationEvent, TRUE);
    gate->state = WL_IO_RUNNING;
    gate->active = 0;
    gate->queued_count = 0;
    gate->device = NULL;
    gate->dispatch = GateDispatch;
    return IoCsqInitializeEx(&gate->csq, GateInsertEx, GateRemove, GatePeek,
                             GateAcquire, GateRelease, GateCancelled);
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
    KIRQL irql;
    KeAcquireSpinLock(&gate->lock, &irql);
    if (gate->active > 0) {
        gate->active--;
        if (gate->active == 0) {
            KeSetEvent(&gate->drained, IO_NO_INCREMENT, FALSE);
        }
    }
    KeReleaseSpinLock(&gate->lock, irql);
}

NTSTATUS WinLvmIoGateQueue(WL_IO_GATE *gate, PIRP Irp) {
    return IoCsqInsertIrpEx(&gate->csq, Irp, NULL, NULL);
}

NTSTATUS WinLvmIoGateBeginQuiesce(WL_IO_GATE *gate) {
    KIRQL irql;
    KeAcquireSpinLock(&gate->lock, &irql);
    if (gate->state == WL_IO_QUIESCED) {
        KeReleaseSpinLock(&gate->lock, irql);
        return STATUS_SUCCESS;
    }
    if (gate->state != WL_IO_RUNNING) {
        KeReleaseSpinLock(&gate->lock, irql);
        return STATUS_INVALID_DEVICE_STATE;
    }
    gate->state = WL_IO_QUIESCING;
    if (gate->active == 0) {
        gate->state = WL_IO_QUIESCED;
        KeReleaseSpinLock(&gate->lock, irql);
        return STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&gate->lock, irql);

    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        LARGE_INTEGER timeout;
        timeout.QuadPart = -50000000LL; /* 5 seconds relative */
        NTSTATUS wait_status = KeWaitForSingleObject(
            &gate->drained, Executive, KernelMode, FALSE, &timeout);
        BOOLEAN need_resume = FALSE;
        KeAcquireSpinLock(&gate->lock, &irql);
        if (gate->state == WL_IO_QUIESCING && gate->active == 0) {
            gate->state = WL_IO_QUIESCED;
            KeReleaseSpinLock(&gate->lock, irql);
            return STATUS_SUCCESS;
        }
        if (gate->state == WL_IO_QUIESCING) {
            gate->state = WL_IO_RUNNING;
            need_resume = TRUE;
        }
        KeReleaseSpinLock(&gate->lock, irql);
        if (need_resume && gate->device) {
            WinLvmIoGateResume(gate, gate->device);
        }
        return (wait_status == STATUS_TIMEOUT) ? STATUS_TIMEOUT : STATUS_UNSUCCESSFUL;
    }

    return STATUS_PENDING;
}

BOOLEAN WinLvmIoGateIsQuiesced(WL_IO_GATE *gate) {
    KIRQL irql;
    BOOLEAN quiesced;
    KeAcquireSpinLock(&gate->lock, &irql);
    quiesced = (BOOLEAN)(gate->state == WL_IO_QUIESCED && gate->active == 0);
    KeReleaseSpinLock(&gate->lock, irql);
    return quiesced;
}

void WinLvmIoGateResume(WL_IO_GATE *gate, PDEVICE_OBJECT DeviceObject) {
    PIRP batch[WINLVM_IO_QUEUE_LIMIT];
    ULONG count = 0;
    PIRP irp;
    KIRQL irql;

    KeAcquireSpinLock(&gate->lock, &irql);
    if (gate->state == WL_IO_QUIESCED || gate->state == WL_IO_QUIESCING) {
        gate->state = WL_IO_RUNNING;
    }
    KeReleaseSpinLock(&gate->lock, irql);

    /* Drain into a bounded local batch before dispatch so a concurrent
     * BeginQuiesce re-queueing an IRP cannot livelock this loop. */
    while (count < WINLVM_IO_QUEUE_LIMIT &&
           (irp = IoCsqRemoveNextIrp(&gate->csq, NULL)) != NULL) {
        batch[count++] = irp;
    }
    for (ULONG i = 0; i < count; i++) {
        if (gate->dispatch && DeviceObject) {
            gate->dispatch(DeviceObject, batch[i]);
        } else {
            batch[i]->IoStatus.Status = STATUS_INVALID_DEVICE_STATE;
            batch[i]->IoStatus.Information = 0;
            IoCompleteRequest(batch[i], IO_NO_INCREMENT);
        }
    }
}

void WinLvmIoGateRemove(WL_IO_GATE *gate) {
    PIRP irp;
    KIRQL irql;
    BOOLEAN wait_drained = FALSE;

    KeAcquireSpinLock(&gate->lock, &irql);
    gate->state = WL_IO_REMOVING;
    if (gate->active > 0) {
        wait_drained = TRUE;
    }
    KeReleaseSpinLock(&gate->lock, irql);

    while ((irp = IoCsqRemoveNextIrp(&gate->csq, NULL)) != NULL) {
        irp->IoStatus.Status = STATUS_DEVICE_REMOVED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }

    /* Drain all in-flight completions before the caller deletes the PDO.
     * Re-acquiring gate->lock after Wait acts as a barrier guaranteeing any
     * WinLvmIoGateLeave that signaled gate->drained has released gate->lock. */
    if (wait_drained && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        KeWaitForSingleObject(&gate->drained, Executive, KernelMode, FALSE, NULL);
        KeAcquireSpinLock(&gate->lock, &irql);
        KeReleaseSpinLock(&gate->lock, irql);
    }
}
