/*
 * winlvm_probe.c — PnP interface notification lifecycle, link filtering,
 *                  and unified PV probing for PnP and non-PnP devices.
 */
#include "winlvm.h"

typedef struct {
    KTIMER         timer;
    KDPC           dpc;
    PIO_WORKITEM   io_work;
    PDRIVER_OBJECT driver;
    BOOLEAN        active;
} WINLVM_REGISTER_TIMER;

typedef struct {
    PIO_WORKITEM   io_work;
    UNICODE_STRING ifname;
    BOOLEAN        explicit_probe;
} WINLVM_PROBE_WORK;

static KSPIN_LOCK            g_work_lock;
static LONG                  g_pending_work = 0;
static KEVENT                g_work_drained;
static BOOLEAN               g_stopping = FALSE;
static BOOLEAN               g_work_init = FALSE;
static PDEVICE_OBJECT        g_work_device = NULL;
static WINLVM_REGISTER_TIMER g_reg_timer;

static void WinLvmProbeWorker(PDEVICE_OBJECT DeviceObject, PVOID context);

void WinLvmProbeSubsystemInit(void) {
    KeInitializeSpinLock(&g_work_lock);
    KeInitializeEvent(&g_work_drained, NotificationEvent, TRUE);
    g_pending_work = 0;
    g_stopping = FALSE;
    g_work_device = NULL;
    RtlZeroMemory(&g_reg_timer, sizeof(g_reg_timer));
    g_work_init = TRUE;
}

void WinLvmProbeSubsystemResume(PDEVICE_OBJECT work_dev) {
    if (!g_work_init) return;
    KIRQL irql;
    KeAcquireSpinLock(&g_work_lock, &irql);
    g_stopping = FALSE;
    if (work_dev) g_work_device = work_dev;
    KeReleaseSpinLock(&g_work_lock, irql);
}

BOOLEAN WinLvmWorkAcquire(void) {
    if (!g_work_init) return FALSE;
    KIRQL irql;
    BOOLEAN ok = FALSE;
    KeAcquireSpinLock(&g_work_lock, &irql);
    if (!g_stopping) {
        g_pending_work++;
        KeClearEvent(&g_work_drained);
        ok = TRUE;
    }
    KeReleaseSpinLock(&g_work_lock, irql);
    return ok;
}

void WinLvmWorkRelease(void) {
    if (!g_work_init) return;
    KIRQL irql;
    KeAcquireSpinLock(&g_work_lock, &irql);
    if (g_pending_work > 0 && --g_pending_work == 0) {
        KeSetEvent(&g_work_drained, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&g_work_lock, irql);
}

PIO_WORKITEM WinLvmAllocBusWorkItem(void) {
    if (!g_work_init) return NULL;
    KIRQL irql;
    PDEVICE_OBJECT dev = NULL;
    KeAcquireSpinLock(&g_work_lock, &irql);
    if (!g_stopping) dev = g_work_device;
    if (dev) ObReferenceObject(dev);
    KeReleaseSpinLock(&g_work_lock, irql);
    if (!dev) return NULL;
    PIO_WORKITEM wi = IoAllocateWorkItem(dev);
    ObDereferenceObject(dev);
    return wi;
}

static WCHAR WinLvmAsciiLowerW(WCHAR c) {
    return (c >= L'A' && c <= L'Z') ? (WCHAR)(c + (L'a' - L'A')) : c;
}

/* Non-paged ASCII case-insensitive equality safe at DISPATCH_LEVEL. */
BOOLEAN WinLvmUnicodeEqualI(const UNICODE_STRING *a, const UNICODE_STRING *b) {
    if (!a || !b || !a->Buffer || !b->Buffer || a->Length != b->Length) return FALSE;
    size_t n = a->Length / sizeof(WCHAR);
    for (size_t i = 0; i < n; i++) {
        if (WinLvmAsciiLowerW(a->Buffer[i]) != WinLvmAsciiLowerW(b->Buffer[i])) return FALSE;
    }
    return TRUE;
}

static BOOLEAN WinLvmUnicodeContainsI(const UNICODE_STRING *haystack, const WCHAR *needle) {
    if (!haystack || !haystack->Buffer || !needle) return FALSE;
    size_t hlen = haystack->Length / sizeof(WCHAR), nlen = 0;
    while (needle[nlen]) nlen++;
    if (nlen == 0 || hlen < nlen) return FALSE;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen && WinLvmAsciiLowerW(haystack->Buffer[i + j]) == WinLvmAsciiLowerW(needle[j])) j++;
        if (j == nlen) return TRUE;
    }
    return FALSE;
}

BOOLEAN WinLvmShouldIgnoreLinkName(const UNICODE_STRING *name) {
    return WinLvmUnicodeContainsI(name, L"winlvm") ||
           WinLvmUnicodeContainsI(name, L"cdrom") ||
           WinLvmUnicodeContainsI(name, L"volumesnapshot");
}

BOOLEAN WinLvmIsProbeStopping(void) {
    if (!g_work_init) return FALSE;
    KIRQL irql;
    KeAcquireSpinLock(&g_work_lock, &irql);
    BOOLEAN stop = g_stopping;
    KeReleaseSpinLock(&g_work_lock, irql);
    return stop;
}

void WinLvmUnregisterDiskNotify(void) {
    PVOID disk_cookie = InterlockedExchangePointer(&g_disk_notify_cookie, NULL);
    if (disk_cookie) IoUnregisterPlugPlayNotificationEx(disk_cookie);
    PVOID vol_cookie = InterlockedExchangePointer(&g_volume_notify_cookie, NULL);
    if (vol_cookie) IoUnregisterPlugPlayNotificationEx(vol_cookie);
}

NTSTATUS WinLvmRegisterDiskNotify(PDRIVER_OBJECT DriverObject) {
    if (WinLvmIsProbeStopping()) return STATUS_DEVICE_REMOVED;
    NTSTATUS status = STATUS_SUCCESS;
    const GUID *guids[2] = { &GUID_DEVINTERFACE_DISK, &GUID_DEVINTERFACE_VOLUME };
    PVOID *cookies[2] = { &g_disk_notify_cookie, &g_volume_notify_cookie };
    for (int i = 0; i < 2; i++) {
        if (*cookies[i]) continue;
        PVOID cookie = NULL;
        NTSTATUS st = IoRegisterPlugPlayNotification(
            EventCategoryDeviceInterfaceChange,
            PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
            (PVOID)guids[i], DriverObject, WinLvmPnpDiskNotify, NULL, &cookie);
        if (!NT_SUCCESS(st)) {
            WL_TRACE_2("register: notify failed idx %d 0x%x", i, (unsigned)st);
            if (NT_SUCCESS(status)) status = st;
        } else if (InterlockedCompareExchangePointer(cookies[i], cookie, NULL) != NULL) {
            IoUnregisterPlugPlayNotificationEx(cookie);
        }
    }
    if (WinLvmIsProbeStopping()) {
        WinLvmUnregisterDiskNotify();
        return STATUS_DEVICE_REMOVED;
    }
    return status;
}

static VOID WinLvmRegisterWorker(PDEVICE_OBJECT DeviceObject, PVOID context) {
    UNREFERENCED_PARAMETER(DeviceObject);
    WINLVM_REGISTER_TIMER *rt = (WINLVM_REGISTER_TIMER *)context;
    KIRQL irql;
    KeAcquireSpinLock(&g_work_lock, &irql);
    BOOLEAN stop = g_stopping;
    PDRIVER_OBJECT driver = rt->driver;
    PIO_WORKITEM wi = rt->io_work;
    rt->io_work = NULL;
    rt->active = FALSE;
    KeReleaseSpinLock(&g_work_lock, irql);

    if (!stop && g_bus_ext && driver) {
        WinLvmUnregisterDiskNotify();
        (void)WinLvmRegisterDiskNotify(driver);
    }
    if (wi) IoFreeWorkItem(wi);
    WinLvmWorkRelease();
}

static VOID WinLvmRegisterDpcRoutine(PKDPC Dpc, PVOID DeferredContext,
                                     PVOID SystemArgument1, PVOID SystemArgument2) {
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    WINLVM_REGISTER_TIMER *rt = (WINLVM_REGISTER_TIMER *)DeferredContext;
    if (rt->io_work) {
        IoQueueWorkItem(rt->io_work, WinLvmRegisterWorker, DelayedWorkQueue, rt);
    } else {
        WinLvmWorkRelease();
    }
}

void WinLvmRegisterDiskNotifyDeferred(PDEVICE_OBJECT BusFdo) {
    if (!g_work_init || !BusFdo) return;
    PIO_WORKITEM wi = IoAllocateWorkItem(BusFdo);
    if (!wi) return;

    KIRQL irql;
    BOOLEAN armed = FALSE;
    KeAcquireSpinLock(&g_work_lock, &irql);
    if (!g_stopping && !g_reg_timer.active) {
        g_reg_timer.driver = BusFdo->DriverObject;
        g_reg_timer.io_work = wi;
        g_reg_timer.active = TRUE;
        g_pending_work++;
        KeClearEvent(&g_work_drained);
        KeInitializeTimer(&g_reg_timer.timer);
        KeInitializeDpc(&g_reg_timer.dpc, WinLvmRegisterDpcRoutine, &g_reg_timer);
        LARGE_INTEGER delay;
        delay.QuadPart = -20000000LL; /* 2 seconds relative */
        KeSetTimer(&g_reg_timer.timer, delay, &g_reg_timer.dpc);
        armed = TRUE;
    }
    KeReleaseSpinLock(&g_work_lock, irql);
    if (!armed) IoFreeWorkItem(wi);
}

void WinLvmStopAndDrainProbeWork(void) {
    if (!g_work_init) {
        WinLvmUnregisterDiskNotify();
        WinLvmUnregisterAllTargetNotifies();
        return;
    }
    KIRQL irql;
    PIO_WORKITEM cancelled_wi = NULL;
    KeAcquireSpinLock(&g_work_lock, &irql);
    g_stopping = TRUE;
    if (g_reg_timer.active && KeCancelTimer(&g_reg_timer.timer)) {
        g_reg_timer.active = FALSE;
        cancelled_wi = g_reg_timer.io_work;
        g_reg_timer.io_work = NULL;
        if (g_pending_work > 0 && --g_pending_work == 0) {
            KeSetEvent(&g_work_drained, IO_NO_INCREMENT, FALSE);
        }
    }
    KeReleaseSpinLock(&g_work_lock, irql);
    if (cancelled_wi) IoFreeWorkItem(cancelled_wi);

    KeFlushQueuedDpcs();
    WinLvmUnregisterDiskNotify();
    WinLvmUnregisterAllTargetNotifies();
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        KeWaitForSingleObject(&g_work_drained, Executive, KernelMode, FALSE, NULL);
        /* Barrier: wait for the last WinLvmWorkRelease to release g_work_lock. */
        KeAcquireSpinLock(&g_work_lock, &irql);
        KeReleaseSpinLock(&g_work_lock, irql);
    }
    WinLvmUnregisterDiskNotify();
    WinLvmUnregisterAllTargetNotifies();

    KeAcquireSpinLock(&g_work_lock, &irql);
    g_work_device = NULL;
    KeReleaseSpinLock(&g_work_lock, irql);
}

NTSTATUS WinLvmQueueProbeForInterfaceEx(const UNICODE_STRING *ifname,
                                        BOOLEAN explicit_probe) {
    if (!ifname || !ifname->Buffer || ifname->Length < sizeof(WCHAR) ||
        (ifname->Length % sizeof(WCHAR)) != 0 ||
        ifname->Length > (USHORT)(0xFFFEu - sizeof(WCHAR))) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!WinLvmWorkAcquire()) return STATUS_DEVICE_REMOVED;

    PIO_WORKITEM wi = WinLvmAllocBusWorkItem();
    if (!wi) {
        WinLvmWorkRelease();
        return STATUS_DEVICE_REMOVED;
    }
    WINLVM_PROBE_WORK *work = (WINLVM_PROBE_WORK *)ExAllocatePoolZero(
        NonPagedPool, sizeof(WINLVM_PROBE_WORK), WINLVM_WIN_TAG);
    if (!work) {
        IoFreeWorkItem(wi);
        WinLvmWorkRelease();
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    USHORT len = ifname->Length;
    PWCHAR buf = (PWCHAR)ExAllocatePoolZero(
        NonPagedPool, (size_t)len + sizeof(WCHAR), WINLVM_WIN_TAG);
    if (!buf) {
        ExFreePoolWithTag(work, WINLVM_WIN_TAG);
        IoFreeWorkItem(wi);
        WinLvmWorkRelease();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(buf, ifname->Buffer, len);
    work->io_work = wi;
    work->ifname.Buffer = buf;
    work->ifname.Length = len;
    work->ifname.MaximumLength = (USHORT)(len + sizeof(WCHAR));
    work->explicit_probe = explicit_probe;

    IoQueueWorkItem(wi, WinLvmProbeWorker, DelayedWorkQueue, work);
    return STATUS_SUCCESS;
}

NTSTATUS WinLvmPnpDiskNotify(PVOID NotificationStructure, PVOID Context) {
    UNREFERENCED_PARAMETER(Context);
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION change =
        (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)NotificationStructure;
    if (!change || !change->SymbolicLinkName || !change->SymbolicLinkName->Buffer ||
        change->SymbolicLinkName->Length == 0) {
        return STATUS_SUCCESS;
    }
    if (WinLvmShouldIgnoreLinkName(change->SymbolicLinkName)) {
        return STATUS_SUCCESS;
    }

    if (RtlEqualMemory(&change->Event, &GUID_DEVICE_INTERFACE_REMOVAL, sizeof(GUID))) {
        WINLVM_BUS_EXT *bus = g_bus_ext;
        if (!bus) return STATUS_SUCCESS;

        USHORT len = change->SymbolicLinkName->Length;
        if ((len % sizeof(WCHAR)) != 0 || len > (USHORT)(0xFFFEu - sizeof(WCHAR))) {
            return STATUS_SUCCESS;
        }
        /* Copy SymbolicLinkName out of PagedPool at PASSIVE_LEVEL before acquiring bus->lock. */
        PWCHAR sym_buf = (PWCHAR)ExAllocatePoolZero(
            NonPagedPool, (size_t)len + sizeof(WCHAR), WINLVM_WIN_TAG);
        if (!sym_buf) return STATUS_SUCCESS;
        RtlCopyMemory(sym_buf, change->SymbolicLinkName->Buffer, len);
        if (len >= 4 * sizeof(WCHAR) && sym_buf[0] == L'\\' &&
            (sym_buf[1] == L'\\' || sym_buf[1] == L'?') &&
            (sym_buf[2] == L'.' || sym_buf[2] == L'?') && sym_buf[3] == L'\\') {
            sym_buf[1] = L'?';
            sym_buf[2] = L'?';
        }
        UNICODE_STRING np_link;
        np_link.Length = len;
        np_link.MaximumLength = (USHORT)(len + sizeof(WCHAR));
        np_link.Buffer = sym_buf;

        WinLvmReleasePvAndQuiesceChildren(bus, &np_link, NULL, FALSE);
        ExFreePoolWithTag(sym_buf, WINLVM_WIN_TAG);
        return STATUS_SUCCESS;
    }

    if (!RtlEqualMemory(&change->Event, &GUID_DEVICE_INTERFACE_ARRIVAL, sizeof(GUID))) {
        return STATUS_SUCCESS;
    }
    return WinLvmQueueProbeForInterfaceEx(change->SymbolicLinkName, FALSE);
}

NTSTATUS WinLvmProbeDevicePathEx(const UNICODE_STRING *symLinkOrNtPath,
                                 BOOLEAN allow_part_scan,
                                 BOOLEAN explicit_probe) {
    WINLVM_BUS_EXT *bus = g_bus_ext;
    if (!bus) return STATUS_DEVICE_NOT_READY;

    UNICODE_STRING nt_path;
    PFILE_OBJECT disk_file = NULL;
    PDEVICE_OBJECT disk_pdo = NULL;
    BOOLEAN pv_read_only = FALSE;
    NTSTATUS status = WinLvmOpenBackingDevice(
        symLinkOrNtPath, bus->fdo->DriverObject,
        &nt_path, &disk_file, &disk_pdo, &pv_read_only);
    if (!NT_SUCCESS(status)) return status;

    if (!pv_read_only && !WinLvmQueryDiskWritable(disk_pdo, disk_file)) {
        pv_read_only = TRUE;
    }
    ULONG logical_sector = 512, physical_sector = 512;
    WinLvmQuerySectorSize(disk_pdo, disk_file, &logical_sector, &physical_sector);

    WINLVM_VIEW_BLOCK *block = NULL;
    char pv_uuid[WL_VG_UUID_LEN];
    status = WinLvmReadVgFromDevice(disk_pdo, disk_file, logical_sector, &block, pv_uuid);
    if (!NT_SUCCESS(status)) {
        ULONG dev_num = 0, parts[32];
        size_t part_count = 0;
        BOOLEAN has_parts = (allow_part_scan && status == STATUS_NOT_FOUND &&
                             WinLvmGetDiskPartitions(disk_pdo, disk_file, &dev_num,
                                                     parts, 32, &part_count));
        ObDereferenceObject(disk_pdo);
        ObDereferenceObject(disk_file);
        ExFreePoolWithTag(nt_path.Buffer, WINLVM_WIN_TAG);
        if (has_parts) return WinLvmProbePartitionList(dev_num, parts, part_count, explicit_probe);
        return status;
    }

    WINLVM_PV_TAG *tag = (WINLVM_PV_TAG *)ExAllocatePoolZero(
        NonPagedPool, sizeof(WINLVM_PV_TAG), WINLVM_WIN_TAG);
    if (!tag) {
        ExFreePoolWithTag(block, WINLVM_WIN_TAG);
        ObDereferenceObject(disk_pdo);
        ObDereferenceObject(disk_file);
        ExFreePoolWithTag(nt_path.Buffer, WINLVM_WIN_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    /* Ownership of +1 disk_pdo (IoGetAttachedDeviceReference) and +1 disk_file transfers to tag. */
    tag->backing_pdo = disk_pdo;
    tag->backing_file = disk_file;
    tag->disk_ifname = nt_path;
    tag->logical_sector = logical_sector;
    tag->physical_sector = physical_sector;
    tag->read_only = pv_read_only;

    PDEVICE_OBJECT quiesced[WL_MAX_CHILDREN];
    size_t quiesced_count = 0;
    BOOLEAN quiesce_ok = FALSE;
    WINLVM_RECONCILE_DROPS *drops = NULL;

    KIRQL old;
    KeAcquireSpinLock(&bus->lock, &old);
    int exist_slot = wl_vg_find(bus->vg_records, WL_VG_MAX, block->vg.id);
    BOOLEAN need_quiesce = (exist_slot >= 0 &&
                            bus->vg_records[exist_slot].refs > 0 &&
                            block->vg.seqno > bus->vg_records[exist_slot].seqno);
    KeReleaseSpinLock(&bus->lock, old);

    if (need_quiesce) {
        drops = (WINLVM_RECONCILE_DROPS *)ExAllocatePoolZero(
            NonPagedPool, sizeof(WINLVM_RECONCILE_DROPS), WINLVM_WIN_TAG);
        if (drops) {
            quiesce_ok = WinLvmQuiesceRecordChildren(
                bus, &bus->vg_records[exist_slot], quiesced, &quiesced_count);
        }
    }

    WINLVM_PROBE_COMMIT commit;
    KeAcquireSpinLock(&bus->lock, &old);
    wl_arrival_result res = WinLvmProbeCommitArrivalLocked(
        bus, &block, pv_uuid, tag, quiesce_ok, explicit_probe, drops, &commit);
    KeReleaseSpinLock(&bus->lock, old);

    if (commit.tn_file_ref) {
        WinLvmTargetNotifyRegisterForTag(
            bus, commit.slot, tag, commit.tn_file_ref, &commit.tn_path, pv_uuid);
        ObDereferenceObject(commit.tn_file_ref);
        ExFreePoolWithTag(commit.tn_path.Buffer, WINLVM_WIN_TAG);
    }
    if (commit.old_view) ExFreePoolWithTag(commit.old_view, WINLVM_WIN_TAG);
    if (drops) WinLvmFreeReconcileDrops(drops);
    for (size_t i = 0; i < commit.free_tag_count; i++) {
        WinLvmFreePvTag(commit.tags_to_free[i]);
    }
    if (quiesced_count > 0) WinLvmResumeRecordChildren(quiesced, quiesced_count);
    WinLvmFinishProbeActivation(bus, commit.slot, commit.newly, commit.newly_count,
                                commit.revived, commit.revived_count,
                                (BOOLEAN)(res == WL_ARR_RECONCILED));
    if (block) ExFreePoolWithTag(block, WINLVM_WIN_TAG);
    return (res == WL_ARR_REJECTED) ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

NTSTATUS WinLvmProbeDevicePath(const UNICODE_STRING *symLinkOrNtPath) {
    return WinLvmProbeDevicePathEx(symLinkOrNtPath, TRUE, FALSE);
}

static void WinLvmProbeWorker(PDEVICE_OBJECT DeviceObject, PVOID context) {
    UNREFERENCED_PARAMETER(DeviceObject);
    WINLVM_PROBE_WORK *work = (WINLVM_PROBE_WORK *)context;
    PIO_WORKITEM wi = work->io_work;
    WL_TRACE_1("probe worker: ifname %wZ", &work->ifname);
    (void)WinLvmProbeDevicePathEx(&work->ifname, TRUE, work->explicit_probe);
    ExFreePoolWithTag(work->ifname.Buffer, WINLVM_WIN_TAG);
    ExFreePoolWithTag(work, WINLVM_WIN_TAG);
    if (wi) IoFreeWorkItem(wi);
    WinLvmWorkRelease();
}
