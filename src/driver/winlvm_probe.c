/*
 * winlvm_probe.c — PnP interface notification lifecycle, link filtering,
 *                  and unified PV probing for PnP and non-PnP devices.
 */
#include "winlvm.h"

typedef struct {
    KTIMER          timer;
    KDPC            dpc;
    WORK_QUEUE_ITEM item;
    PDRIVER_OBJECT  driver;
    BOOLEAN         active;
} WINLVM_REGISTER_TIMER;

typedef struct {
    WORK_QUEUE_ITEM item;
    UNICODE_STRING  ifname;
} WINLVM_PROBE_WORK;

static KSPIN_LOCK            g_work_lock;
static LONG                  g_pending_work = 0;
static KEVENT                g_work_drained;
static BOOLEAN               g_stopping = FALSE;
static BOOLEAN               g_work_init = FALSE;
static WINLVM_REGISTER_TIMER g_reg_timer;

static void WinLvmProbeWorker(PVOID context);

void WinLvmProbeSubsystemInit(void) {
    KeInitializeSpinLock(&g_work_lock);
    KeInitializeEvent(&g_work_drained, NotificationEvent, TRUE);
    g_pending_work = 0;
    g_stopping = FALSE;
    RtlZeroMemory(&g_reg_timer, sizeof(g_reg_timer));
    g_work_init = TRUE;
}

void WinLvmProbeSubsystemResume(void) {
    if (!g_work_init) return;
    KIRQL irql;
    KeAcquireSpinLock(&g_work_lock, &irql);
    g_stopping = FALSE;
    KeReleaseSpinLock(&g_work_lock, irql);
}

static BOOLEAN WinLvmWorkAcquire(void) {
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

static void WinLvmWorkRelease(void) {
    if (!g_work_init) return;
    KIRQL irql;
    KeAcquireSpinLock(&g_work_lock, &irql);
    if (g_pending_work > 0 && --g_pending_work == 0) {
        KeSetEvent(&g_work_drained, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&g_work_lock, irql);
}

static WCHAR WinLvmAsciiLowerW(WCHAR c) {
    return (c >= L'A' && c <= L'Z') ? (WCHAR)(c + (L'a' - L'A')) : c;
}

/* Non-paged ASCII case-insensitive equality safe at DISPATCH_LEVEL. */
static BOOLEAN WinLvmUnicodeEqualI(const UNICODE_STRING *a, const UNICODE_STRING *b) {
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

static BOOLEAN WinLvmShouldIgnoreLinkName(const UNICODE_STRING *name) {
    return WinLvmUnicodeContainsI(name, L"winlvm") ||
           WinLvmUnicodeContainsI(name, L"cdrom") ||
           WinLvmUnicodeContainsI(name, L"volumesnapshot");
}

static BOOLEAN WinLvmIsStopping(void) {
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
    if (WinLvmIsStopping()) return STATUS_DEVICE_REMOVED;
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
    if (WinLvmIsStopping()) {
        WinLvmUnregisterDiskNotify();
        return STATUS_DEVICE_REMOVED;
    }
    return status;
}

static VOID WinLvmRegisterDpcRoutine(PKDPC Dpc, PVOID DeferredContext,
                                     PVOID SystemArgument1, PVOID SystemArgument2) {
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
    ExQueueWorkItem(&((WINLVM_REGISTER_TIMER *)DeferredContext)->item, DelayedWorkQueue);
}

static VOID WinLvmRegisterWorker(PVOID context) {
    WINLVM_REGISTER_TIMER *rt = (WINLVM_REGISTER_TIMER *)context;
    KIRQL irql;
    KeAcquireSpinLock(&g_work_lock, &irql);
    BOOLEAN stop = g_stopping;
    PDRIVER_OBJECT driver = rt->driver;
    rt->active = FALSE;
    KeReleaseSpinLock(&g_work_lock, irql);

    if (!stop && g_bus_ext && driver) {
        WinLvmUnregisterDiskNotify();
        (void)WinLvmRegisterDiskNotify(driver);
    }
    WinLvmWorkRelease();
}

void WinLvmRegisterDiskNotifyDeferred(PDRIVER_OBJECT DriverObject) {
    if (!g_work_init) return;
    KIRQL irql;
    KeAcquireSpinLock(&g_work_lock, &irql);
    if (!g_stopping && !g_reg_timer.active) {
        g_reg_timer.driver = DriverObject;
        g_reg_timer.active = TRUE;
        g_pending_work++;
        KeClearEvent(&g_work_drained);
        KeInitializeTimer(&g_reg_timer.timer);
        KeInitializeDpc(&g_reg_timer.dpc, WinLvmRegisterDpcRoutine, &g_reg_timer);
        ExInitializeWorkItem(&g_reg_timer.item, WinLvmRegisterWorker, &g_reg_timer);
        LARGE_INTEGER delay;
        delay.QuadPart = -20000000LL; /* 2 seconds relative */
        KeSetTimer(&g_reg_timer.timer, delay, &g_reg_timer.dpc);
    }
    KeReleaseSpinLock(&g_work_lock, irql);
}

void WinLvmStopAndDrainProbeWork(void) {
    if (!g_work_init) {
        WinLvmUnregisterDiskNotify();
        return;
    }
    KIRQL irql;
    KeAcquireSpinLock(&g_work_lock, &irql);
    g_stopping = TRUE;
    if (g_reg_timer.active && KeCancelTimer(&g_reg_timer.timer)) {
        g_reg_timer.active = FALSE;
        if (g_pending_work > 0 && --g_pending_work == 0) {
            KeSetEvent(&g_work_drained, IO_NO_INCREMENT, FALSE);
        }
    }
    KeReleaseSpinLock(&g_work_lock, irql);

    KeFlushQueuedDpcs();
    WinLvmUnregisterDiskNotify();
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        KeWaitForSingleObject(&g_work_drained, Executive, KernelMode, FALSE, NULL);
    }
    /* Unregister again after all workers exit so late WinLvmRegisterWorker calls
     * cannot leave live PnP notifications across unload. */
    WinLvmUnregisterDiskNotify();
}

static NTSTATUS WinLvmQueueProbeForInterface(const UNICODE_STRING *ifname) {
    if (!ifname || !ifname->Buffer || ifname->Length < sizeof(WCHAR) ||
        (ifname->Length % sizeof(WCHAR)) != 0 ||
        ifname->Length > (USHORT)(0xFFFEu - sizeof(WCHAR))) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!WinLvmWorkAcquire()) return STATUS_DEVICE_REMOVED;

    WINLVM_PROBE_WORK *work = (WINLVM_PROBE_WORK *)ExAllocatePoolZero(
        NonPagedPool, sizeof(WINLVM_PROBE_WORK), WINLVM_WIN_TAG);
    if (!work) {
        WinLvmWorkRelease();
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    USHORT len = ifname->Length;
    PWCHAR buf = (PWCHAR)ExAllocatePoolZero(
        NonPagedPool, (size_t)len + sizeof(WCHAR), WINLVM_WIN_TAG);
    if (!buf) {
        ExFreePoolWithTag(work, WINLVM_WIN_TAG);
        WinLvmWorkRelease();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(buf, ifname->Buffer, len);
    work->ifname.Buffer = buf;
    work->ifname.Length = len;
    work->ifname.MaximumLength = (USHORT)(len + sizeof(WCHAR));

    ExInitializeWorkItem(&work->item, WinLvmProbeWorker, work);
    ExQueueWorkItem(&work->item, DelayedWorkQueue);
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

        WINLVM_PV_TAG *removed_tag = NULL;
        BOOLEAN found = FALSE;
        KIRQL old;
        KeAcquireSpinLock(&bus->lock, &old);
        for (size_t i = 0; i < WL_VG_MAX && !found; i++) {
            wl_vg_record *r = &bus->vg_records[i];
            if (!r->vg_uuid[0]) continue;
            for (size_t p = 0; p < r->pv_count && p < WL_VG_MAX_PVS; p++) {
                WINLVM_PV_TAG *tag = (WINLVM_PV_TAG *)r->pv_tag[p];
                if (tag && tag->disk_ifname.Buffer &&
                    WinLvmUnicodeEqualI(&tag->disk_ifname, change->SymbolicLinkName)) {
                    char pv_uuid[WL_VG_UUID_LEN];
                    RtlCopyMemory(pv_uuid, r->pv_uuid[p], WL_VG_UUID_LEN);
                    removed_tag = tag;
                    r->pv_tag[p] = NULL;
                    wl_vg_pv_removed(bus->vg_records, WL_VG_MAX, pv_uuid);

                    for (size_t c = 0; c < bus->child_count; c++) {
                        WINLVM_LV_EXT *lext = (WINLVM_LV_EXT *)bus->children[c]->DeviceExtension;
                        if (lext->record == r && wl_vg_lv_any_pv_gone(r, lext->lv_index)) {
                            lext->unavailable = TRUE;
                        }
                    }
                    found = TRUE;
                    break;
                }
            }
        }
        KeReleaseSpinLock(&bus->lock, old);

        if (removed_tag) WinLvmFreePvTag(removed_tag);
        if (found) IoInvalidateDeviceRelations(bus->pdo, BusRelations);
        return STATUS_SUCCESS;
    }

    if (!RtlEqualMemory(&change->Event, &GUID_DEVICE_INTERFACE_ARRIVAL, sizeof(GUID))) {
        return STATUS_SUCCESS;
    }
    return WinLvmQueueProbeForInterface(change->SymbolicLinkName);
}

NTSTATUS WinLvmProbeDevicePath(const UNICODE_STRING *symLinkOrNtPath) {
    WINLVM_BUS_EXT *bus = g_bus_ext;
    if (!bus) return STATUS_DEVICE_NOT_READY;
    if (!symLinkOrNtPath || !symLinkOrNtPath->Buffer ||
        symLinkOrNtPath->Length < sizeof(WCHAR) ||
        (symLinkOrNtPath->Length % sizeof(WCHAR)) != 0 ||
        symLinkOrNtPath->Length > (USHORT)(0xFFFEu - sizeof(WCHAR))) {
        return STATUS_INVALID_PARAMETER;
    }

    USHORT len = symLinkOrNtPath->Length;
    PWCHAR path_buf = (PWCHAR)ExAllocatePoolZero(
        NonPagedPool, (size_t)len + sizeof(WCHAR), WINLVM_WIN_TAG);
    if (!path_buf) return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(path_buf, symLinkOrNtPath->Buffer, len);
    if (len >= 4 * sizeof(WCHAR) &&
        path_buf[0] == L'\\' && path_buf[1] == L'\\' &&
        path_buf[2] == L'.' && path_buf[3] == L'\\') {
        path_buf[1] = L'?';
        path_buf[2] = L'?';
    }

    UNICODE_STRING nt_path;
    nt_path.Buffer = path_buf;
    nt_path.Length = len;
    nt_path.MaximumLength = (USHORT)(len + sizeof(WCHAR));
    if (WinLvmShouldIgnoreLinkName(&nt_path)) {
        ExFreePoolWithTag(path_buf, WINLVM_WIN_TAG);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    PFILE_OBJECT disk_file = NULL;
    PDEVICE_OBJECT disk_pdo = NULL;
    BOOLEAN pv_read_only = FALSE;
    NTSTATUS status = IoGetDeviceObjectPointer(
        &nt_path, FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES,
        &disk_file, &disk_pdo);
    if (status == STATUS_ACCESS_DENIED || status == STATUS_MEDIA_WRITE_PROTECTED) {
        status = IoGetDeviceObjectPointer(
            &nt_path, FILE_READ_DATA | FILE_READ_ATTRIBUTES,
            &disk_file, &disk_pdo);
        if (NT_SUCCESS(status)) pv_read_only = TRUE;
    }
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(path_buf, WINLVM_WIN_TAG);
        return status;
    }
    if (disk_pdo->DriverObject == bus->fdo->DriverObject) {
        ObDereferenceObject(disk_file);
        ExFreePoolWithTag(path_buf, WINLVM_WIN_TAG);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (!pv_read_only && !WinLvmQueryDiskWritable(disk_pdo, disk_file)) {
        pv_read_only = TRUE;
    }
    ULONG logical_sector = 512, physical_sector = 512;
    WinLvmQuerySectorSize(disk_pdo, disk_file, &logical_sector, &physical_sector);

    WINLVM_VIEW_BLOCK *block = NULL;
    char pv_uuid[WL_VG_UUID_LEN];
    status = WinLvmReadVgFromDevice(disk_pdo, disk_file, logical_sector, &block, pv_uuid);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(disk_file);
        ExFreePoolWithTag(path_buf, WINLVM_WIN_TAG);
        return status;
    }

    WINLVM_PV_TAG *tag = (WINLVM_PV_TAG *)ExAllocatePoolZero(
        NonPagedPool, sizeof(WINLVM_PV_TAG), WINLVM_WIN_TAG);
    if (!tag) {
        ExFreePoolWithTag(block, WINLVM_WIN_TAG);
        ObDereferenceObject(disk_file);
        ExFreePoolWithTag(path_buf, WINLVM_WIN_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    tag->backing_pdo = disk_pdo;
    tag->backing_file = disk_file;
    tag->disk_ifname = nt_path;
    tag->logical_sector = logical_sector;
    tag->physical_sector = physical_sector;
    tag->read_only = pv_read_only;
    ObReferenceObject(disk_pdo);
    ObReferenceObject(disk_file);
    ObDereferenceObject(disk_file);

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

    WINLVM_PV_TAG *old_tags[WL_VG_MAX_PVS];
    size_t old_tag_count = 0;
    WINLVM_PV_TAG *tags_to_free[WL_VG_MAX_PVS + 1];
    size_t free_tag_count = 0;
    WINLVM_VIEW_BLOCK *old_view = NULL;
    size_t slot = 0;
    size_t newly[WL_VG_MAX_LVS];
    size_t newly_count = 0;

    KeAcquireSpinLock(&bus->lock, &old);
    int existed_idx = wl_vg_find(bus->vg_records, WL_VG_MAX, block->vg.id);
    BOOLEAN all_quiesced = TRUE;
    if (existed_idx >= 0) {
        wl_vg_record *er = &bus->vg_records[existed_idx];
        for (size_t p = 0; p < er->pv_count && p < WL_VG_MAX_PVS; p++) {
            if (er->pv_tag[p]) old_tags[old_tag_count++] = (WINLVM_PV_TAG *)er->pv_tag[p];
        }
        all_quiesced = WinLvmAllRecordChildrenQuiescedLocked(bus, er);
    }
    wl_lv_update_fn seam = (quiesce_ok && all_quiesced) ? WinLvmUpdateLvSeam : NULL;
    wl_arrival_result res = wl_vg_arrival(
        bus->vg_records, WL_VG_MAX, &slot, &block->vg,
        pv_uuid, tag, seam, bus, newly, WL_VG_MAX_LVS, &newly_count);

    if ((res == WL_ARR_RECONCILED) ||
        (existed_idx < 0 && (res == WL_ARR_COMPLETED || res == WL_ARR_PENDING))) {
        if (res == WL_ARR_RECONCILED) old_view = bus->vg_views[slot];
        bus->vg_views[slot] = block;
        block = NULL;
    }
    if (res == WL_ARR_RECONCILED) {
        WinLvmReconcileChildrenLocked(bus, &bus->vg_records[slot], drops);
    }
    if (res != WL_ARR_REJECTED && res != WL_ARR_DUPLICATE) {
        wl_vg_record *r = &bus->vg_records[slot];
        for (size_t oi = 0; oi < old_tag_count; oi++) {
            BOOLEAN still_present = FALSE;
            for (size_t p = 0; p < r->pv_count; p++) {
                if (r->pv_tag[p] == old_tags[oi]) { still_present = TRUE; break; }
            }
            if (!still_present) tags_to_free[free_tag_count++] = old_tags[oi];
        }
        BOOLEAN tag_kept = FALSE;
        for (size_t p = 0; p < r->pv_count; p++) {
            if (r->pv_tag[p] == tag) { tag_kept = TRUE; break; }
        }
        if (!tag_kept) tags_to_free[free_tag_count++] = tag;
        for (size_t i = 0; i < newly_count; i++) wl_vg_lv_set_activated(r, newly[i], 1);
    } else {
        tags_to_free[free_tag_count++] = tag;
    }
    KeReleaseSpinLock(&bus->lock, old);

    if (old_view) ExFreePoolWithTag(old_view, WINLVM_WIN_TAG);
    if (drops) WinLvmFreeReconcileDrops(drops);
    for (size_t i = 0; i < free_tag_count; i++) WinLvmFreePvTag(tags_to_free[i]);
    if (quiesced_count > 0) WinLvmResumeRecordChildren(quiesced, quiesced_count);
    for (size_t i = 0; i < newly_count; i++) {
        wl_vg_record *r = &bus->vg_records[slot];
        if (!WinLvmProbeWorkerCreateLvPdo(bus, r, newly[i])) {
            KeAcquireSpinLock(&bus->lock, &old);
            wl_vg_lv_set_activated(r, newly[i], 0);
            KeReleaseSpinLock(&bus->lock, old);
        }
    }
    if (newly_count > 0 || res == WL_ARR_RECONCILED) {
        IoInvalidateDeviceRelations(bus->pdo, BusRelations);
    }
    if (block) ExFreePoolWithTag(block, WINLVM_WIN_TAG);
    return (res == WL_ARR_REJECTED) ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

static void WinLvmProbeWorker(PVOID context) {
    WINLVM_PROBE_WORK *work = (WINLVM_PROBE_WORK *)context;
    WL_TRACE_1("probe worker: ifname %wZ", &work->ifname);
    (void)WinLvmProbeDevicePath(&work->ifname);
    ExFreePoolWithTag(work->ifname.Buffer, WINLVM_WIN_TAG);
    ExFreePoolWithTag(work, WINLVM_WIN_TAG);
    WinLvmWorkRelease();
}
