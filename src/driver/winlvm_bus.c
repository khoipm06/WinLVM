/* winlvm_bus.c — winlvm.sys bus FDO: AddDevice, disk-arrival probing,
 * BusRelations reporting, and the bus half of PnP dispatch.
 *
 * Moved verbatim from winlvm.c (2026-09-12 module split); bodies are
 * byte-identical, only the PnP bus half moved behind WinLvmBusPnp.
 */
#include "winlvm.h"
/* ntstrsafe must come after the WDK headers: it needs the STATUS_* constants
 * that ntddk.h defines (including it first breaks the build with
 * 'STATUS_INVALID_PARAMETER: undeclared identifier' inside ntstrsafe.h). */
#include <ntstrsafe.h>

static const WCHAR kBusDeviceName[] = L"\\Device\\WinLvmBus";
static const WCHAR kBusSymLink[] = L"\\DosDevices\\WinLvmBus";

/* Per-call metadata scratch for lvm_read.c's kernel path: probe workers
 * for several disks run concurrently, so no shared statics. */
void *winlvm_probe_scratch_alloc(void) {
    return ExAllocatePoolZero(NonPagedPool, WINLVM_MAX_METADATA_BYTES + 1,
                             WINLVM_WIN_TAG);
}

void winlvm_probe_scratch_free(void *p) {
    if (p) ExFreePoolWithTag(p, WINLVM_WIN_TAG);
}

static NTSTATUS WinLvmRegisterDiskNotify(PDRIVER_OBJECT DriverObject) {
    if (g_disk_notify_cookie) return STATUS_SUCCESS;
    return IoRegisterPlugPlayNotification(
        EventCategoryDeviceInterfaceChange,
        PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
        (PVOID)&GUID_DEVINTERFACE_DISK,
        DriverObject,
        WinLvmPnpDiskNotify,
        NULL,
        &g_disk_notify_cookie);
}

typedef struct {
    WORK_QUEUE_ITEM item;
    PDRIVER_OBJECT  driver;
} WINLVM_REGISTER_WORK;

static VOID WinLvmRegisterWorker(PVOID context) {
    WINLVM_REGISTER_WORK *work = (WINLVM_REGISTER_WORK *)context;
    WL_TRACE("register worker entry");
    /* F10 fix: PnP START transaction holds devnode lock across post-start.
     * DelayedWorkQueue can execute immediately on another core before PnP
     * unwinds, so IoRegisterPlugPlayNotification(INCLUDE_EXISTING) callback's
     * IoGetDeviceObjectPointer deadlocks with the open devnode lock.
     * 2s relative delay lets the START transaction close completely. */
    LARGE_INTEGER delay;
    delay.QuadPart = -20000000LL;  /* 2 seconds relative (100ns units) */
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
    WL_TRACE("register worker delay done");
    /* Bus may have been removed while this worker sat queued: registering
     * then would leave a cookie pointing at a dead bus and block the next
     * devnode cycle from re-registering. */
    if (g_bus_ext) WinLvmRegisterDiskNotify(work->driver);
    ExFreePoolWithTag(work, WINLVM_WIN_TAG);
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

/* --------------------------------------------------------------- */
/* Disk arrival: probe for LVM PVs, cache VG, create LV PDOs.       */
/* --------------------------------------------------------------- */

typedef struct {
    WORK_QUEUE_ITEM item;
    PDEVICE_OBJECT  target_disk;
    PFILE_OBJECT    target_file;
    UNICODE_STRING  ifname;   /* disk interface link (owned): arrival link
                              * stashed for the worker to store on the VG
                              * record — REMOVAL matching (winlvm_bus.c) */
} WINLVM_PROBE_WORK;

NTSTATUS WinLvmPnpDiskNotify(PVOID notificationStruct, PVOID context) {
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION data =
        (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)notificationStruct;
    UNREFERENCED_PARAMETER(context);
    if (!data || !data->SymbolicLinkName) return STATUS_SUCCESS;

    /* Backing-disk surprise removal: the probe only listens to ARRIVALS,
     * so a removed PV left a live VG record pointing at a dead PDO — IRPs
     * forwarded there wedged the VM minutes after the removal
     * (2026-09-13 surprise-removal test). Tear the record down. */
    if (IsEqualGUID(&data->Event, &GUID_DEVICE_INTERFACE_REMOVAL)) {
        WINLVM_BUS_EXT *bus = g_bus_ext;
        if (!bus) return STATUS_SUCCESS;
        KIRQL old;
        KeAcquireSpinLock(&bus->lock, &old);
        WINLVM_VG_RECORD *record = bus->vgs;
        WINLVM_VG_RECORD *hit = NULL;
        if (record && record->disk_ifname.Buffer &&
            RtlEqualUnicodeString(&record->disk_ifname,
                                  data->SymbolicLinkName, TRUE)) {
            hit = record;
        }
        if (hit) {
            /* Mark dead first so in-flight dispatch fails new I/O cleanly;
             * actual record free happens after child PDOs are gone
             * (IRP_MN_REMOVE_DEVICE drops them — winlvm_child.c). */
            InterlockedExchange(&hit->dead, 1);
        }
        KeReleaseSpinLock(&bus->lock, old);
        if (hit) {
            WL_TRACE("bus: backing disk removed, invalidating VG");
            /* Re-report relations: children with a dead record complete
             * their own removal path on the next QDR cycle. */
            IoInvalidateDeviceRelations(bus->pdo, BusRelations);
        }
        return STATUS_SUCCESS;
    }

    if (!IsEqualGUID(&data->Event, &GUID_DEVICE_INTERFACE_ARRIVAL)) return STATUS_SUCCESS;

    WINLVM_PROBE_WORK *work = ExAllocatePoolZero(NonPagedPool,
        sizeof(WINLVM_PROBE_WORK), WINLVM_WIN_TAG);
    if (!work) return STATUS_INSUFFICIENT_RESOURCES;

    /* stash the arrival link: the worker stores it on the VG record so a
     * later REMOVAL notification can match this disk back to the record.
     * (No RtlDuplicateUnicodeString — that's ntifs-only; manual copy.) */
    {
        USHORT n = data->SymbolicLinkName->Length;
        PWCHAR buf = (PWCHAR)ExAllocatePoolZero(NonPagedPool,
            n + sizeof(WCHAR), WINLVM_WIN_TAG);
        if (buf) {
            RtlCopyMemory(buf, data->SymbolicLinkName->Buffer, n);
            work->ifname.Buffer = buf;
            work->ifname.Length = n;
            work->ifname.MaximumLength = (USHORT)(n + sizeof(WCHAR));
        } else {
            RtlZeroMemory(&work->ifname, sizeof(work->ifname));
        }
    }

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

                int created = 0;
                if (!record) {
                    record = ExAllocatePoolZero(NonPagedPool,
                        sizeof(WINLVM_VG_RECORD), WINLVM_WIN_TAG);
                    if (record) {
                        record->vg = *vg;
                        ObReferenceObject(work->target_disk);
                        record->backing_pdo = work->target_disk;
                        /* keep the disk interface link so a later REMOVAL
                         * notification can find this record again */
                        if (work->ifname.Buffer)
                            record->disk_ifname = work->ifname;
                        else
                            RtlZeroMemory(&record->disk_ifname,
                                          sizeof(record->disk_ifname));
                        work->ifname.Buffer = NULL; /* ownership moved */
                        KeAcquireSpinLock(&bus->lock, &old);
                        if (!bus->vgs) { bus->vgs = record; created = 1; }
                        else {
                            /* single-PV groups only for now: a second VG on
                             * another disk is dropped leak-free */
                            WINLVM_VG_RECORD *r = bus->vgs;
                            if (r != record) {
                                KeReleaseSpinLock(&bus->lock, old);
                                ObDereferenceObject(work->target_disk);
                                /* disk_ifname is a MANUAL NonPagedPool copy
                                 * (see ifname stash above) — tagged free,
                                 * never RtlFreeUnicodeString (no header) */
                                if (record->disk_ifname.Buffer)
                                    ExFreePoolWithTag(record->disk_ifname.Buffer,
                                                      WINLVM_WIN_TAG);
                                ExFreePoolWithTag(record, WINLVM_WIN_TAG);
                                record = NULL;
                                KeAcquireSpinLock(&bus->lock, &old);
                            }
                        }
                        KeReleaseSpinLock(&bus->lock, old);
                    }
                } else if (record->backing_pdo != work->target_disk) {
                    /* a different disk carrying (a copy of) the same VG:
                     * single-PV phase — ignore, no duplicate LV devnodes */
                    record = NULL;
                }

                /* re-notifies of the same PV must not re-create LV PDOs:
                 * duplicates would collide on the LV-UUID instance ID */
                if (record && created) {
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
    if (work->ifname.Buffer)              /* never stored on a record */
        ExFreePoolWithTag(work->ifname.Buffer, WINLVM_WIN_TAG);
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

    /* The volume interface is registered at IRP_MN_START_DEVICE, not here:
     * IoRegisterDeviceInterface needs the PDO's devnode instance path, and
     * no devnode exists until PnP processes this PDO via BusRelations —
     * create-time registration fails silently and mountmgr never hears
     * about the volume. */

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
/* Bus FDO PnP dispatch.                                            */
/* --------------------------------------------------------------- */

static NTSTATUS WinLvmQueryBusRelations(WINLVM_BUS_EXT *bus, PIRP Irp);

static NTSTATUS WinLvmSyncCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS WinLvmBusPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    WINLVM_BUS_EXT *ext = (WINLVM_BUS_EXT *)DeviceObject->DeviceExtension;

    switch (irpSp->MinorFunction) {
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (irpSp->Parameters.QueryDeviceRelations.Type == BusRelations) {
            /* The bus FDO owns the BusRelations answer for its children
             * and completes the IRP itself (toaster busenum pattern).
             * Forwarding down lets the root PDO (PnpManager) replace the
             * answer with its own failure/empty relations — enumerated
             * children never materialize as devnodes (2026-09-11 kd -kl
             * finding: child_count=1 in the driver, zero in PnP). */
            NTSTATUS st = WinLvmQueryBusRelations(ext, Irp);
            Irp->IoStatus.Status = st;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return st;
        }
        break;
    case IRP_MN_START_DEVICE: {
        /* WDM FDO contract: an FDO must wait for the lower driver to
         * complete START before starting itself, then complete the IRP.
         * Skipping down without waiting trips Verifier PnpStartDevice rule. */
        KEVENT event;
        KeInitializeEvent(&event, NotificationEvent, FALSE);
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp, WinLvmSyncCompletion, &event, TRUE, TRUE, TRUE);
        NTSTATUS st = IoCallDriver(ext->bus_pdo, Irp);
        if (st == STATUS_PENDING) {
            KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
            st = Irp->IoStatus.Status;
        }
        if (NT_SUCCESS(st)) {
            /* Oracle H5: register a zero-GUID wmilib provider on the bus FDO
             * so the WMI core owns the FDO ↔ ProviderId mapping the verifier
             * probe expects. Nonfatal, PASSIVE_LEVEL (START_DEVICE). */
            WinLvmWmiInit(DeviceObject);

            WINLVM_REGISTER_WORK *work = ExAllocatePoolZero(NonPagedPool,
                sizeof(WINLVM_REGISTER_WORK), WINLVM_WIN_TAG);
            if (work) {
                ExInitializeWorkItem(&work->item, WinLvmRegisterWorker, work);
                work->driver = DeviceObject->DriverObject;
                ExQueueWorkItem(&work->item, DelayedWorkQueue);
            }
        }
        Irp->IoStatus.Status = st;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return st;
    }
    case IRP_MN_REMOVE_DEVICE: {
        IoSkipCurrentIrpStackLocation(Irp);
        NTSTATUS st = IoCallDriver(ext->bus_pdo, Irp);
        /* forward down, then detach + delete children then self */
        for (size_t i = 0; i < ext->child_count; i++) {
            /* Children are deleted directly here (never see their own
             * REMOVE_DEVICE), so undo their per-device state explicitly:
             * the child's WMI provider entry (H5) and volume interface. */
            IoWMIRegistrationControl(ext->children[i], WMIREG_ACTION_DEREGISTER);
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
        /* Drop the disk-interface registration with the bus: leaving the
         * cookie set makes the next devnode cycle's START skip
         * re-registration (WinLvmRegisterDiskNotify early-outs), so a
         * disable/enable would silently stop finding LVs forever. */
        if (g_disk_notify_cookie) {
            IoUnregisterPlugPlayNotification(g_disk_notify_cookie);
            g_disk_notify_cookie = NULL;
        }
        IoWMIRegistrationControl(DeviceObject, WMIREG_ACTION_DEREGISTER);
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

static NTSTATUS WinLvmQueryBusRelations(WINLVM_BUS_EXT *bus, PIRP Irp) {
    KIRQL old;
    KeAcquireSpinLock(&bus->lock, &old);
    size_t n = 0;
    /* dead-VG children are not reported — PnP sends them REMOVE, which
     * detaches them from the record (surprise-removal teardown) */
    for (size_t i = 0; i < bus->child_count; i++) {
        WINLVM_LV_EXT *lext =
            (WINLVM_LV_EXT *)bus->children[i]->DeviceExtension;
        if (lext->record && lext->record->dead) continue;
        bus->children[n] = bus->children[i];
        if (n != i) bus->children[i] = NULL;
        n++;
    }
    bus->child_count = n;
    KeReleaseSpinLock(&bus->lock, old);
    WL_TRACE_1("QDR bus relations: %u children", (unsigned)n);

    /* WDM contract: BusRelations completed with STATUS_SUCCESS must ALWAYS
     * carry a valid PDEVICE_RELATIONS in Information — even with zero
     * children. Returning success with Information = NULL made PnP
     * dereference NULL right after START (2026-09-12 install freeze). */
    size_t bytes = sizeof(DEVICE_RELATIONS) + n * sizeof(PDEVICE_OBJECT);
    PDEVICE_RELATIONS rel = (PDEVICE_RELATIONS)ExAllocatePoolZero(
        NonPagedPool, bytes, WINLVM_WIN_TAG);
    if (!rel) return STATUS_INSUFFICIENT_RESOURCES;

    KeAcquireSpinLock(&bus->lock, &old);
    rel->Count = (ULONG)n;
    for (size_t i = 0; i < n; i++) {
        rel->Objects[i] = bus->children[i];
        /* WDM contract: the bus driver must reference each reported PDO;
         * PnP consumes that reference when it ingests the devnode. Reporting
         * without it leaves the PDO delete-pending (refcount 1->0) and the
         * devnode never materializes (2026-09-12 kd -kl: refcount 0,
         * DO_BUS_ENUMERATED set, zero devnodes). Winbtrfs volume.c:149
         * references the same way. */
        ObReferenceObject(bus->children[i]);
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
