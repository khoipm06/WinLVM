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

/* Release a per-PV backing tag: interface link copy + disk PDO reference. */
static void WinLvmFreePvTag(WINLVM_PV_TAG *tag) {
    if (!tag) return;
    if (tag->disk_ifname.Buffer)
        ExFreePoolWithTag(tag->disk_ifname.Buffer, WINLVM_WIN_TAG);
    if (tag->backing_pdo) ObDereferenceObject(tag->backing_pdo);
    ExFreePoolWithTag(tag, WINLVM_WIN_TAG);
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
    wl_vg_registry_init(ext->vg_records, WL_VG_MAX); /* idempotent after zero */
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
     * (2026-09-13 surprise-removal test). Tear the PV down.
     * W2 Task 8: the registry is multi-PV; match the link against every
     * PV's per-slot tag and mark the PV GONE (record stays — re-insert). */
    if (IsEqualGUID(&data->Event, &GUID_DEVICE_INTERFACE_REMOVAL)) {
        WINLVM_BUS_EXT *bus = g_bus_ext;
        if (!bus) return STATUS_SUCCESS;
        wl_vg_record *hit = NULL;
        size_t hit_pv = 0;
        KIRQL old;
        KeAcquireSpinLock(&bus->lock, &old);
        for (size_t i = 0; i < WL_VG_MAX && !hit; i++) {
            wl_vg_record *r = &bus->vg_records[i];
            if (!r->vg_uuid[0]) continue;
            for (size_t p = 0; p < r->pv_count; p++) {
                const WINLVM_PV_TAG *tag = (const WINLVM_PV_TAG *)r->pv_tag[p];
                if (tag && tag->disk_ifname.Buffer &&
                    RtlEqualUnicodeString(&tag->disk_ifname,
                                          data->SymbolicLinkName, TRUE)) {
                    hit = r;
                    hit_pv = p;
                    break;
                }
            }
        }
        if (hit) {
            /* Mark the PV GONE (dead recomputed, per-LV all-gone set) BEFORE
             * dropping the tag so dispatch fails closed on the next IRP
             * (2026-09-13 lesson). The record itself survives for re-insert. */
            wl_vg_pv_removed(bus->vg_records, WL_VG_MAX, hit->pv_uuid[hit_pv]);
            WINLVM_PV_TAG *tag = (WINLVM_PV_TAG *)hit->pv_tag[hit_pv];
            hit->pv_tag[hit_pv] = NULL;
            KeReleaseSpinLock(&bus->lock, old);
            if (tag) WinLvmFreePvTag(tag);
            WL_TRACE("bus: backing disk removed, invalidating VG");
            /* Re-report relations: children of an all-gone PV complete their
             * own removal path on the next QDR cycle. */
            IoInvalidateDeviceRelations(bus->pdo, BusRelations);
            return STATUS_SUCCESS;
        }
        KeReleaseSpinLock(&bus->lock, old);
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

static BOOLEAN WinLvmProbeWorkerCreateLvPdo(WINLVM_BUS_EXT *bus,
                                            wl_vg_record *record,
                                            size_t lv_index);

/* W3 UPDATE_LV seam, absent: keeps the OLD view serving (fail closed —
 * never free/fake under live children). Returns unavailable, so a
 * higher-seqno late arrival leaves the record stale with the old view. */
wl_update_result WinLvmUpdateLvSeam(void *ctx, const char *lv_uuid) {
    (void)ctx; (void)lv_uuid;
    return WL_UPDATE_UNAVAILABLE;
}

void WinLvmProbeWorker(PVOID context) {
    WINLVM_PROBE_WORK *work = (WINLVM_PROBE_WORK *)context;
    WINLVM_BUS_EXT *bus = g_bus_ext;
    WL_TRACE("probe worker start");

    if (bus) {
        WINLVM_DISK_SOURCE dsrc;
        dsrc.disk_pdo = work->target_disk;
        dsrc.file = work->target_file;
        KeInitializeEvent(&dsrc.done, NotificationEvent, FALSE);

        /* Adapter: the lvmcore byte source has the same read-callback
         * contract as winlvm_byte_source — the disk source IS the impl. */
        lvmcore_byte_source src;
        src.impl = &dsrc;
        src.size = ~0ULL;
        src.read = winlvm_disk_source_read;

        /* Per-arrival view arena: label + parse live here. Consumed by the
         * record only on first-arrival create / refs==0 reconcile; frees
         * here otherwise (core ownership contract). */
        WINLVM_VIEW_BLOCK *block = ExAllocatePoolZero(NonPagedPool,
            sizeof(WINLVM_VIEW_BLOCK), WINLVM_WIN_TAG);
        if (!block) {
            WL_TRACE("probe: view block alloc failed");
            goto out;
        }
        lvmcore_arena_init(&block->arena, block->buf, sizeof(block->buf));

        lvmcore_pv_label label;
        lvmcore_err lerr;
        lvmcore_status st = lvmcore_read_pv_label(&src, &block->arena,
                                                  &label, &lerr);
        if (st != LVMCORE_OK) {
            WL_TRACE_1("probe: label read failed: %s", lerr.message);
            goto out_block;
        }

        lvmcore_volume_group *vg_out = &block->vg;
        lvmcore_err verr;
        st = lvmcore_read_vg(&src, LVMCORE_READ_FLAG_DEFAULT, &block->arena,
                             vg_out, &verr);
        if (st != LVMCORE_OK) {
            WL_TRACE_1("probe: lvmcore read_vg failed: %s", verr.message);
            goto out_block;
        }

        /* Per-PV backing tag: interface link + disk PDO (referenced). */
        WINLVM_PV_TAG *tag = ExAllocatePoolZero(NonPagedPool,
            sizeof(WINLVM_PV_TAG), WINLVM_WIN_TAG);
        if (!tag) {
            WL_TRACE("probe: tag alloc failed");
            goto out_block;
        }
        tag->backing_pdo = work->target_disk;
        tag->disk_ifname = work->ifname;
        work->ifname.Buffer = NULL; /* ownership moved into the tag */
        ObReferenceObject(work->target_disk);

        /* Registry transitions run under the bus lock (the state machine
         * mutates only shared record slots). PDO creation happens after
         * release — PASSIVE_LEVEL, IoCreateDevice. */
        KIRQL old;
        size_t slot = 0;
        size_t newly[WL_VG_MAX_LVS];
        size_t newly_count = 0;
        KeAcquireSpinLock(&bus->lock, &old);
        int existed = wl_vg_find(bus->vg_records, WL_VG_MAX,
                                 vg_out->id) >= 0;
        wl_arrival_result res = wl_vg_arrival(
            bus->vg_records, WL_VG_MAX, &slot, vg_out,
            label.pv_uuid, tag, WinLvmUpdateLvSeam, NULL,
            newly, WL_VG_MAX_LVS, &newly_count);
        /* View arena kept by the record only when it was stored: brand-new
         * record (created now) or a refs==0 reconcile swap. */
        int view_kept = (res == WL_ARR_RECONCILED) ||
                        (!existed && (res == WL_ARR_COMPLETED ||
                                      res == WL_ARR_PENDING));
        if (view_kept) {
            if (res == WL_ARR_RECONCILED && bus->vg_views[slot]) {
                ExFreePoolWithTag(bus->vg_views[slot], WINLVM_WIN_TAG);
            }
            bus->vg_views[slot] = block;
            block = NULL; /* relocated into the bus extension */
        }
        /* Claim the freshly-activatable LVs UNDER THE LOCK: the core leaves
         * lv_activated[] untouched (wl_collect_newly excludes claimed LVs),
         * so the first arrival for this VG wins — a concurrent worker for
         * another PV of the same VG never double-creates a PDO. */
        for (size_t i = 0; i < newly_count; i++)
            wl_vg_lv_set_activated(&bus->vg_records[slot], newly[i], 1);
        KeReleaseSpinLock(&bus->lock, old);

        /* Tag ownership: the record stores it on every accepted result
         * except REJECTED / DUPLICATE, and on STALE_SEQNO only when the
         * serving view has a slot for this PV UUID. */
        if (res == WL_ARR_REJECTED || res == WL_ARR_DUPLICATE) {
            WinLvmFreePvTag(tag);
            tag = NULL;
        } else if (res == WL_ARR_STALE_SEQNO) {
            wl_vg_record *r = &bus->vg_records[slot];
            int kept = 0;
            for (size_t p = 0; p < r->pv_count; p++)
                if (r->pv_tag[p] == tag) { kept = 1; break; }
            if (!kept) { WinLvmFreePvTag(tag); tag = NULL; }
        }

        /* Freshly-activatable LVs become child PDOs (single-PV backing
         * per PDO; multi-PV fan-out is W3). Transient create failures
         * release the claim so a future arrival retries this LV. */
        if (newly_count > 0) {
            wl_vg_record *r = &bus->vg_records[slot];
            for (size_t i = 0; i < newly_count; i++) {
                if (!WinLvmProbeWorkerCreateLvPdo(bus, r, newly[i])) {
                    KIRQL kl;
                    KeAcquireSpinLock(&bus->lock, &kl);
                    wl_vg_lv_set_activated(r, newly[i], 0);
                    KeReleaseSpinLock(&bus->lock, &kl);
                }
            }
            IoInvalidateDeviceRelations(bus->pdo, BusRelations);
        }

out_block:
        if (block) ExFreePoolWithTag(block, WINLVM_WIN_TAG);
    }

out:
    ObDereferenceObject(work->target_file);  /* pair from IoGetDeviceObjectPointer */
    if (work->ifname.Buffer)              /* never stored on a record */
        ExFreePoolWithTag(work->ifname.Buffer, WINLVM_WIN_TAG);
    ExFreePoolWithTag(work, WINLVM_WIN_TAG);
}

static BOOLEAN WinLvmProbeWorkerCreateLvPdo(WINLVM_BUS_EXT *bus,
                                            wl_vg_record *record,
                                            size_t lv_index) {
    /* Named device object: mountmgr requires QUERY_DEVICE_NAME to answer
     * with a \Device namespace name it can open; the volume interface
     * (\??\Volume{guid}) symlink is NOT a valid answer there. */
    const lvmcore_volume_group *view = record->view;
    if (!view || lv_index >= view->lv_count) return FALSE;
    const lvmcore_logical_volume *lv0 = &view->lvs[lv_index];

    /* v22 glue is single-backing pass-through per PDO. Resolve the ONE
     * record PV slot every stripe of this LV lives on; a multi-PV LV
     * (fan-out is W3) must NOT get a PDO — it would read from one disk
     * and silently serve corrupt data for the other PV's extents. */
    size_t pv_slot = SIZE_MAX;
    for (size_t s = 0; s < lv0->segment_count; s++) {
        const lvmcore_segment *seg = &lv0->segments[s];
        for (size_t t = 0; t < seg->stripe_count_used; t++) {
            const lvmcore_physical_volume *pv =
                lvmcore_vg_find_pv(view, seg->stripes[t].pv_key);
            if (!pv) return FALSE;
            size_t slot = SIZE_MAX;
            for (size_t k = 0; k < record->pv_count; k++) {
                if (RtlEqualMemory(record->pv_uuid[k], pv->id, 32)) {
                    slot = k;
                    break;
                }
            }
            if (slot == SIZE_MAX) return FALSE;
            if (pv_slot == SIZE_MAX) pv_slot = slot;
            else if (pv_slot != slot) return FALSE; /* cross-PV LV: W3 fan-out */
        }
    }
    if (pv_slot == SIZE_MAX) return FALSE; /* no stripes (empty LV) */
    const WINLVM_PV_TAG *tag = (const WINLVM_PV_TAG *)record->pv_tag[pv_slot];
    if (!tag || !tag->backing_pdo) return FALSE;
    PDEVICE_OBJECT backing_disk = tag->backing_pdo;

    /* Overflow-safe total size; 0 = overflow or absent — fail closed
     * with no volume rather than a lie of a device. */
    uint64_t byte_size = wl_lv_byte_size(view, lv_index);
    if (byte_size == 0) return FALSE;

    WCHAR namebuf[64];
    RtlZeroMemory(namebuf, sizeof(namebuf));
    UNICODE_STRING devName;
    devName.Buffer = namebuf;
    devName.MaximumLength = sizeof(namebuf);
    NTSTATUS status = RtlUnicodeStringPrintf(&devName, L"\\Device\\WinLvm%hs",
                                             lv0->id);
    if (!NT_SUCCESS(status)) return FALSE;

    PDEVICE_OBJECT lv_pdo = NULL;
    status = IoCreateDevice(bus->fdo->DriverObject,
                                     sizeof(WINLVM_LV_EXT), &devName,
                                     FILE_DEVICE_DISK, FILE_DEVICE_SECURE_OPEN,
                                     FALSE, &lv_pdo);
    if (!NT_SUCCESS(status)) {
        WL_TRACE_1("pdo create failed 0x%x", (unsigned)status);
        return FALSE;
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

    ext->byte_size = byte_size;

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
    if (bus->child_count < WL_MAX_CHILDREN) {
        bus->children[bus->child_count++] = lv_pdo;
        wl_vg_ref(record); /* child-PDO refcount: reconciles refuse to swap
                            * a view under live children (refs == 0 gate) */
    } else {
        /* over cap: undo rather than corrupt the array */
        KeReleaseSpinLock(&bus->lock, old);
        if (ext->ifname.Buffer) {
            IoSetDeviceInterfaceState(&ext->ifname, FALSE);
            RtlFreeUnicodeString(&ext->ifname);
        }
        ObDereferenceObject(backing_disk);
        IoDeleteDevice(lv_pdo);
        return FALSE;
    }
    KeReleaseSpinLock(&bus->lock, old);
    return TRUE;
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
            /* Children sweep deletes directly (never sees REMOVE_DEVICE),
             * so undo the child-PDO ref + activation claim here too. */
            if (ce->record) {
                wl_vg_lv_set_activated(ce->record, ce->lv_index, 0);
                wl_vg_unref(ce->record);
            }
            ObDereferenceObject(ce->backing_pdo);
            IoDeleteDevice(ext->children[i]);
        }
        ext->child_count = 0;
        /* Registry sweep: records are embedded bus-ext slots never freed
         * individually; their arena blocks and per-PV tags return here. */
        for (size_t i = 0; i < WL_VG_MAX; i++) {
            wl_vg_record *r = &ext->vg_records[i];
            for (size_t p = 0; p < r->pv_count; p++) {
                WINLVM_PV_TAG *tag = (WINLVM_PV_TAG *)r->pv_tag[p];
                if (tag) { WinLvmFreePvTag(tag); r->pv_tag[p] = NULL; }
            }
            if (ext->vg_views[i]) {
                ExFreePoolWithTag(ext->vg_views[i], WINLVM_WIN_TAG);
                ext->vg_views[i] = NULL;
            }
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
     * detaches them from the record (surprise-removal teardown). Test is
     * PER-LV: in a multi-PV VG only the children whose own PVs all went
     * away drop (an LV on a surviving PV keeps its backing). */
    for (size_t i = 0; i < bus->child_count; i++) {
        WINLVM_LV_EXT *lext =
            (WINLVM_LV_EXT *)bus->children[i]->DeviceExtension;
        if (lext->record && wl_vg_lv_all_gone(lext->record, lext->lv_index))
            continue;
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
