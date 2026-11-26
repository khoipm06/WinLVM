/*
 * winlvm_notify.c — Reference-counted target-device PnP notifications,
 *                   backing-handle release, and MountMgr arrival worker.
 */
#include "winlvm.h"

struct _WINLVM_TARGET_NOTIFY {
    LIST_ENTRY     link;
    PVOID          cookie;
    UNICODE_STRING disk_ifname;
    CHAR           pv_uuid[WL_VG_UUID_LEN];
    PIO_WORKITEM   cleanup_work;
    volatile LONG  refs;
    volatile LONG  unregistered;
    BOOLEAN        async_queued;
    BOOLEAN        linked;
};

typedef struct {
    PIO_WORKITEM   io_work;
    PDEVICE_OBJECT lv_pdo;
    BOOLEAN        dismount_first;
} WINLVM_MM_NOTIFY_WORK;

static KSPIN_LOCK g_target_notify_lock;
static LIST_ENTRY g_target_notify_list;
static BOOLEAN    g_target_notify_init = FALSE;

static NTSTATUS WinLvmTargetDeviceNotify(PVOID NotificationStructure, PVOID Context);

void WinLvmTargetNotifyInit(void) {
    KeInitializeSpinLock(&g_target_notify_lock);
    InitializeListHead(&g_target_notify_list);
    g_target_notify_init = TRUE;
}

static void WinLvmTargetNotifyAddRef(WINLVM_TARGET_NOTIFY *tn) {
    if (tn) InterlockedIncrement(&tn->refs);
}

void WinLvmTargetNotifyRelease(WINLVM_TARGET_NOTIFY *tn) {
    if (!tn) return;
    if (InterlockedDecrement(&tn->refs) == 0) {
        if (tn->disk_ifname.Buffer) {
            ExFreePoolWithTag(tn->disk_ifname.Buffer, WINLVM_WIN_TAG);
        }
        ExFreePoolWithTag(tn, WINLVM_WIN_TAG);
    }
}

/* Atomically claim and unregister tn->cookie at most once, dropping reg_ref. */
static void WinLvmTargetNotifyUnregisterCookieOnce(WINLVM_TARGET_NOTIFY *tn) {
    PVOID cookie = InterlockedExchangePointer(&tn->cookie, NULL);
    if (cookie) {
        IoUnregisterPlugPlayNotificationEx(cookie);
        WinLvmTargetNotifyRelease(tn);
    }
}

static void WinLvmTargetNotifyCleanupWorker(PDEVICE_OBJECT DeviceObject, PVOID Context) {
    UNREFERENCED_PARAMETER(DeviceObject);
    WINLVM_TARGET_NOTIFY *tn = (WINLVM_TARGET_NOTIFY *)Context;
    if (tn) {
        PIO_WORKITEM wi = tn->cleanup_work;
        tn->cleanup_work = NULL;
        WinLvmTargetNotifyUnregisterCookieOnce(tn);
        WinLvmTargetNotifyRelease(tn); /* drop worker_ref */
        if (wi) IoFreeWorkItem(wi);
    }
    WinLvmWorkRelease();
}

/*
 * Unregister from inside WinLvmTargetDeviceNotify via an IoWorkItem.
 * Calling IoUnregisterPlugPlayNotificationEx synchronously inside its own
 * callback deadlocks the PnP notification rundown lock.
 */
static void WinLvmTargetNotifyUnregisterAsync(WINLVM_TARGET_NOTIFY *tn) {
    if (!tn || !g_target_notify_init) return;
    if (!WinLvmWorkAcquire()) {
        InterlockedExchange(&tn->unregistered, 1);
        return;
    }

    PIO_WORKITEM wi = WinLvmAllocBusWorkItem();
    if (!wi) {
        InterlockedExchange(&tn->unregistered, 1);
        WinLvmWorkRelease();
        return;
    }

    KIRQL irql;
    BOOLEAN was_linked = FALSE;
    BOOLEAN should_queue = FALSE;
    KeAcquireSpinLock(&g_target_notify_lock, &irql);
    InterlockedExchange(&tn->unregistered, 1);
    if (tn->cookie != NULL && !tn->async_queued) {
        tn->async_queued = TRUE;
        tn->cleanup_work = wi;
        if (tn->linked) {
            RemoveEntryList(&tn->link);
            tn->linked = FALSE;
            was_linked = TRUE;
        }
        WinLvmTargetNotifyAddRef(tn); /* worker_ref */
        should_queue = TRUE;
    }
    KeReleaseSpinLock(&g_target_notify_lock, irql);

    if (was_linked) WinLvmTargetNotifyRelease(tn); /* list_ref */
    if (!should_queue) {
        IoFreeWorkItem(wi);
        WinLvmWorkRelease();
        return;
    }
    IoQueueWorkItem(wi, WinLvmTargetNotifyCleanupWorker, DelayedWorkQueue, tn);
}

void WinLvmTargetNotifyUnregisterSync(WINLVM_TARGET_NOTIFY *tn) {
    if (!tn || !g_target_notify_init) return;

    KIRQL irql;
    BOOLEAN was_linked = FALSE;
    KeAcquireSpinLock(&g_target_notify_lock, &irql);
    InterlockedExchange(&tn->unregistered, 1);
    if (tn->linked) {
        RemoveEntryList(&tn->link);
        tn->linked = FALSE;
        was_linked = TRUE;
    }
    KeReleaseSpinLock(&g_target_notify_lock, irql);

    if (was_linked) WinLvmTargetNotifyRelease(tn); /* list_ref */
    WinLvmTargetNotifyUnregisterCookieOnce(tn);
}

void WinLvmUnregisterAllTargetNotifies(void) {
    if (!g_target_notify_init) return;
    for (;;) {
        KIRQL irql;
        WINLVM_TARGET_NOTIFY *tn = NULL;
        KeAcquireSpinLock(&g_target_notify_lock, &irql);
        if (!IsListEmpty(&g_target_notify_list)) {
            PLIST_ENTRY entry = RemoveHeadList(&g_target_notify_list);
            tn = CONTAINING_RECORD(entry, WINLVM_TARGET_NOTIFY, link);
            tn->linked = FALSE;
            InterlockedExchange(&tn->unregistered, 1);
        }
        KeReleaseSpinLock(&g_target_notify_lock, irql);

        if (!tn) break;
        WinLvmTargetNotifyUnregisterCookieOnce(tn);
        WinLvmTargetNotifyRelease(tn); /* list_ref */
    }
}

void WinLvmTargetNotifyRegisterForTag(WINLVM_BUS_EXT *bus, size_t rec_slot,
                                      WINLVM_PV_TAG *tag,
                                      PFILE_OBJECT file_ref,
                                      const UNICODE_STRING *path_copy,
                                      const char pv_uuid[WL_VG_UUID_LEN]) {
    if (!g_target_notify_init || !bus || rec_slot >= WL_VG_MAX || !tag ||
        !file_ref || !path_copy || !path_copy->Buffer || path_copy->Length == 0 ||
        !pv_uuid || WinLvmIsProbeStopping()) {
        return;
    }

    WINLVM_TARGET_NOTIFY *tn = (WINLVM_TARGET_NOTIFY *)ExAllocatePoolZero(
        NonPagedPool, sizeof(WINLVM_TARGET_NOTIFY), WINLVM_WIN_TAG);
    if (!tn) return;

    USHORT len = path_copy->Length;
    PWCHAR buf = (PWCHAR)ExAllocatePoolZero(
        NonPagedPool, (size_t)len + sizeof(WCHAR), WINLVM_WIN_TAG);
    if (!buf) {
        ExFreePoolWithTag(tn, WINLVM_WIN_TAG);
        return;
    }
    RtlCopyMemory(buf, path_copy->Buffer, len);
    tn->disk_ifname.Buffer = buf;
    tn->disk_ifname.Length = len;
    tn->disk_ifname.MaximumLength = (USHORT)(len + sizeof(WCHAR));
    RtlCopyMemory(tn->pv_uuid, pv_uuid, WL_VG_UUID_LEN);
    tn->pv_uuid[WL_VG_UUID_LEN - 1] = '\0';
    /* caller_ref (+1) + reg_ref (+1) pinned before exposing tn to PnP. */
    tn->refs = 2;

    PVOID reg_cookie = NULL;
    NTSTATUS st = IoRegisterPlugPlayNotification(
        EventCategoryTargetDeviceChange,
        0,
        file_ref,
        bus->fdo->DriverObject,
        WinLvmTargetDeviceNotify,
        tn,
        &reg_cookie);
    if (!NT_SUCCESS(st) || !reg_cookie) {
        WL_TRACE_2("target notify register skipped for %wZ (0x%08x)",
                   &tn->disk_ifname, (unsigned)st);
        ExFreePoolWithTag(buf, WINLVM_WIN_TAG);
        ExFreePoolWithTag(tn, WINLVM_WIN_TAG);
        return;
    }

    KIRQL irql;
    BOOLEAN abort_reg = FALSE;
    KeAcquireSpinLock(&g_target_notify_lock, &irql);
    if (tn->unregistered || WinLvmIsProbeStopping()) {
        abort_reg = TRUE;
    } else {
        tn->cookie = reg_cookie;
        InsertTailList(&g_target_notify_list, &tn->link);
        tn->linked = TRUE;
        WinLvmTargetNotifyAddRef(tn); /* list_ref */
    }
    KeReleaseSpinLock(&g_target_notify_lock, irql);

    if (abort_reg) {
        IoUnregisterPlugPlayNotificationEx(reg_cookie);
        WinLvmTargetNotifyRelease(tn); /* reg_ref */
        WinLvmTargetNotifyRelease(tn); /* caller_ref */
        return;
    }

    BOOLEAN attached = FALSE;
    KeAcquireSpinLock(&bus->lock, &irql);
    wl_vg_record *r = &bus->vg_records[rec_slot];
    for (size_t p = 0; p < r->pv_count && p < WL_VG_MAX_PVS; p++) {
        if (r->pv_state[p] == WL_PV_PRESENT &&
            RtlEqualMemory(r->pv_uuid[p], pv_uuid, 32) &&
            r->pv_tag[p] == tag && tag->target_notify == NULL) {
            WinLvmTargetNotifyAddRef(tn); /* tag_ref */
            tag->target_notify = tn;
            attached = TRUE;
            break;
        }
    }
    KeReleaseSpinLock(&bus->lock, irql);

    if (!attached) {
        WinLvmTargetNotifyUnregisterSync(tn);
    }
    WinLvmTargetNotifyRelease(tn); /* caller_ref */
}

void WinLvmDetachChildBackingLocked(WINLVM_LV_EXT *lext,
                                    PDEVICE_OBJECT *pdos_out,
                                    PFILE_OBJECT *files_out,
                                    size_t *count_out) {
    *count_out = 0;
    if (!lext) return;
    size_t n = lext->backing_count;
    if (n > WL_VG_MAX_PVS) n = WL_VG_MAX_PVS;
    for (size_t i = 0; i < n; i++) {
        if (lext->backing_pdo[i] || lext->backing_file[i]) {
            pdos_out[*count_out] = lext->backing_pdo[i];
            files_out[*count_out] = lext->backing_file[i];
            (*count_out)++;
            lext->backing_pdo[i] = NULL;
            lext->backing_file[i] = NULL;
        }
    }
    lext->backing_count = 0;
}

void WinLvmReleasePvAndQuiesceChildren(WINLVM_BUS_EXT *bus,
                                       const UNICODE_STRING *np_link,
                                       const char *pv_uuid,
                                       BOOLEAN keep_target_notify) {
    if (!bus || (!np_link && !pv_uuid)) return;

    WINLVM_PV_TAG *removed_tags[WL_VG_MAX * WL_VG_MAX_PVS];
    size_t removed_count = 0;
    WINLVM_TARGET_NOTIFY *kept_tns[WL_VG_MAX * WL_VG_MAX_PVS];
    size_t kept_tn_count = 0;
    WINLVM_VIEW_BLOCK *freed_views[WL_VG_MAX];
    size_t view_count = 0;
    PDEVICE_OBJECT targets[WL_MAX_CHILDREN];
    size_t target_count = 0;
    uint32_t rebind_mask = 0;
    BOOLEAN any_matched = FALSE;

    KIRQL old;
    KeAcquireSpinLock(&bus->lock, &old);
    for (size_t i = 0; i < WL_VG_MAX; i++) {
        wl_vg_record *r = &bus->vg_records[i];
        if (!r->vg_uuid[0]) continue;
        BOOLEAN rec_hit = FALSE;
        for (size_t p = 0; p < r->pv_count && p < WL_VG_MAX_PVS; p++) {
            WINLVM_PV_TAG *tag = (WINLVM_PV_TAG *)r->pv_tag[p];
            BOOLEAN match = FALSE;
            if (np_link) {
                match = (tag && tag->disk_ifname.Buffer &&
                         WinLvmUnicodeEqualI(&tag->disk_ifname, np_link));
            } else if (pv_uuid) {
                match = (RtlEqualMemory(r->pv_uuid[p], pv_uuid, 32) &&
                         (tag != NULL || r->pv_state[p] == WL_PV_PRESENT));
            }
            if (!match) continue;

            char matched_uuid[WL_VG_UUID_LEN];
            RtlCopyMemory(matched_uuid, r->pv_uuid[p], WL_VG_UUID_LEN);
            if (tag) {
                if (keep_target_notify && tag->target_notify) {
                    if (kept_tn_count < WL_VG_MAX * WL_VG_MAX_PVS) {
                        kept_tns[kept_tn_count++] = tag->target_notify;
                    }
                    tag->target_notify = NULL;
                }
                if (removed_count < WL_VG_MAX * WL_VG_MAX_PVS) {
                    removed_tags[removed_count++] = tag;
                }
                r->pv_tag[p] = NULL;
            }
            wl_vg_pv_removed(bus->vg_records, WL_VG_MAX, matched_uuid);
            rec_hit = TRUE;
            any_matched = TRUE;
        }
        if (!rec_hit) continue;

        for (size_t c = 0; c < bus->child_count; c++) {
            PDEVICE_OBJECT child = bus->children[c];
            WINLVM_LV_EXT *lext = (WINLVM_LV_EXT *)child->DeviceExtension;
            if (lext->record != r) continue;
            if (wl_vg_lv_any_pv_gone(r, lext->lv_index)) {
                lext->unavailable = TRUE;
                size_t found = SIZE_MAX;
                for (size_t a = 0; a < target_count; a++) {
                    if (targets[a] == child) { found = a; break; }
                }
                if (found != SIZE_MAX) {
                    rebind_mask &= ~(1u << found);
                } else if (target_count < WL_MAX_CHILDREN) {
                    ObReferenceObject(child);
                    targets[target_count++] = child;
                }
            } else if (!lext->unavailable && !lext->deactivating &&
                       WinLvmChildBindingChangedLocked(r, lext->lv_index, lext)) {
                BOOLEAN seen = FALSE;
                for (size_t a = 0; a < target_count; a++) {
                    if (targets[a] == child) { seen = TRUE; break; }
                }
                if (!seen && target_count < WL_MAX_CHILDREN) {
                    ObReferenceObject(child);
                    rebind_mask |= (1u << target_count);
                    targets[target_count++] = child;
                }
            }
        }
        if (r->refs == 0 && r->dead) {
            for (size_t p = 0; p < r->pv_count && p < WL_VG_MAX_PVS; p++) {
                if (r->pv_tag[p] && removed_count < WL_VG_MAX * WL_VG_MAX_PVS) {
                    removed_tags[removed_count++] = (WINLVM_PV_TAG *)r->pv_tag[p];
                    r->pv_tag[p] = NULL;
                }
            }
            if (bus->vg_views[i] && view_count < WL_VG_MAX) {
                freed_views[view_count++] = bus->vg_views[i];
                bus->vg_views[i] = NULL;
            }
            RtlZeroMemory(r, sizeof(*r));
        }
    }
    KeReleaseSpinLock(&bus->lock, old);

    for (size_t a = 0; a < target_count; a++) {
        if (rebind_mask & (1u << a)) continue;
        PDEVICE_OBJECT child = targets[a];
        WINLVM_LV_EXT *lext = (WINLVM_LV_EXT *)child->DeviceExtension;
        NTSTATUS qst = WinLvmIoGateBeginQuiesce(&lext->io_gate);
        WinLvmLvSetInterfaceEnabled(child, FALSE);
        if (qst == STATUS_SUCCESS) {
            PDEVICE_OBJECT drop_pdos[WL_VG_MAX_PVS];
            PFILE_OBJECT drop_files[WL_VG_MAX_PVS];
            size_t drop_count = 0;
            KeAcquireSpinLock(&bus->lock, &old);
            WinLvmDetachChildBackingLocked(lext, drop_pdos, drop_files, &drop_count);
            lext->drive_letter = 0;
            KeReleaseSpinLock(&bus->lock, old);
            WinLvmIoGateResume(&lext->io_gate, child);
            for (size_t d = 0; d < drop_count; d++) {
                if (drop_pdos[d]) ObDereferenceObject(drop_pdos[d]);
                if (drop_files[d]) ObDereferenceObject(drop_files[d]);
            }
        }
        ObDereferenceObject(child);
    }

    for (size_t a = 0; a < target_count; a++) {
        if (!(rebind_mask & (1u << a))) continue;
        (void)WinLvmRebindChildPdo(bus, targets[a]);
        ObDereferenceObject(targets[a]);
    }

    for (size_t k = 0; k < kept_tn_count; k++) WinLvmTargetNotifyRelease(kept_tns[k]);
    for (size_t k = 0; k < removed_count; k++) WinLvmFreePvTag(removed_tags[k]);
    for (size_t k = 0; k < view_count; k++) ExFreePoolWithTag(freed_views[k], WINLVM_WIN_TAG);
    if (any_matched || target_count > 0) {
        IoInvalidateDeviceRelations(bus->pdo, BusRelations);
    }
}

static NTSTATUS WinLvmTargetDeviceNotify(PVOID NotificationStructure, PVOID Context) {
    PTARGET_DEVICE_REMOVAL_NOTIFICATION note =
        (PTARGET_DEVICE_REMOVAL_NOTIFICATION)NotificationStructure;
    WINLVM_TARGET_NOTIFY *tn = (WINLVM_TARGET_NOTIFY *)Context;
    if (!note || !tn) return STATUS_SUCCESS;

    if (RtlEqualMemory(&note->Event, &GUID_TARGET_DEVICE_QUERY_REMOVE, sizeof(GUID))) {
        WL_TRACE_1("target notify: query remove %wZ", &tn->disk_ifname);
        if (g_bus_ext) {
            WinLvmReleasePvAndQuiesceChildren(g_bus_ext, NULL, tn->pv_uuid, TRUE);
        }
        return STATUS_SUCCESS;
    }

    if (RtlEqualMemory(&note->Event, &GUID_TARGET_DEVICE_REMOVE_CANCELLED, sizeof(GUID))) {
        WL_TRACE_1("target notify: remove cancelled %wZ", &tn->disk_ifname);
        if (tn->disk_ifname.Buffer && tn->disk_ifname.Length > 0) {
            (void)WinLvmQueueProbeForInterfaceEx(&tn->disk_ifname, TRUE);
        }
        WinLvmTargetNotifyUnregisterAsync(tn);
        return STATUS_SUCCESS;
    }

    if (RtlEqualMemory(&note->Event, &GUID_TARGET_DEVICE_REMOVE_COMPLETE, sizeof(GUID))) {
        WL_TRACE_1("target notify: remove complete %wZ", &tn->disk_ifname);
        if (g_bus_ext) {
            WinLvmReleasePvAndQuiesceChildren(g_bus_ext, NULL, tn->pv_uuid, TRUE);
        }
        WinLvmTargetNotifyUnregisterAsync(tn);
        return STATUS_SUCCESS;
    }

    return STATUS_SUCCESS;
}

static void WinLvmMountMgrArrivalWorker(PDEVICE_OBJECT DeviceObject, PVOID Context) {
    UNREFERENCED_PARAMETER(DeviceObject);
    WINLVM_MM_NOTIFY_WORK *work = (WINLVM_MM_NOTIFY_WORK *)Context;
    PDEVICE_OBJECT lv_pdo = work ? work->lv_pdo : NULL;
    PIO_WORKITEM io_work = work ? work->io_work : NULL;
    BOOLEAN dismount_first = work ? work->dismount_first : FALSE;
    if (work) ExFreePoolWithTag(work, WINLVM_WIN_TAG);

    if (lv_pdo && !WinLvmIsProbeStopping()) {
        WINLVM_LV_EXT *lvext = (WINLVM_LV_EXT *)lv_pdo->DeviceExtension;
        if (!lvext->unavailable && !lvext->deactivating && lvext->devname[0] != 0) {
            if (dismount_first) WinLvmRemountLvVolumeIfMounted(lv_pdo);
            WinLvmSendMountMgrArrival(lvext->devname);
            if (dismount_first) WinLvmTriggerLvVolumeMount(lv_pdo);
        }
    }

    if (io_work) IoFreeWorkItem(io_work);
    if (lv_pdo) ObDereferenceObject(lv_pdo);
    WinLvmWorkRelease();
}

void WinLvmQueueMountMgrArrivalEx(PDEVICE_OBJECT LvPdo, BOOLEAN dismount_first) {
    if (!LvPdo || !WinLvmWorkAcquire()) return;
    WINLVM_MM_NOTIFY_WORK *work = (WINLVM_MM_NOTIFY_WORK *)ExAllocatePoolZero(
        NonPagedPool, sizeof(WINLVM_MM_NOTIFY_WORK), WINLVM_WIN_TAG);
    if (!work) {
        WinLvmWorkRelease();
        return;
    }
    PIO_WORKITEM io_work = IoAllocateWorkItem(LvPdo);
    if (!io_work) {
        ExFreePoolWithTag(work, WINLVM_WIN_TAG);
        WinLvmWorkRelease();
        return;
    }
    ObReferenceObject(LvPdo);
    work->lv_pdo = LvPdo;
    work->io_work = io_work;
    work->dismount_first = dismount_first;
    IoQueueWorkItem(io_work, WinLvmMountMgrArrivalWorker, DelayedWorkQueue, work);
}

void WinLvmQueueMountMgrArrival(PDEVICE_OBJECT LvPdo) {
    WinLvmQueueMountMgrArrivalEx(LvPdo, FALSE);
}
