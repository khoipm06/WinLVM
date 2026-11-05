/*
 * winlvm_pnp.c — Bus FDO PnP lifecycle, BusRelations enumeration, PV tag
 *                cleanup, and live child geometry reconciliation.
 */
#include "winlvm.h"

void WinLvmFreePvTag(WINLVM_PV_TAG *tag) {
    if (!tag) return;
    if (tag->disk_ifname.Buffer) {
        ExFreePoolWithTag(tag->disk_ifname.Buffer, WINLVM_WIN_TAG);
    }
    if (tag->backing_pdo) ObDereferenceObject(tag->backing_pdo);
    if (tag->backing_file) ObDereferenceObject(tag->backing_file);
    ExFreePoolWithTag(tag, WINLVM_WIN_TAG);
}

void WinLvmBusResetInactiveRecords(WINLVM_BUS_EXT *bus) {
    if (!bus) return;
    WINLVM_PV_TAG *tags[WL_VG_MAX * WL_VG_MAX_PVS];
    size_t tag_count = 0;
    WINLVM_VIEW_BLOCK *views[WL_VG_MAX];
    size_t view_count = 0;

    KIRQL old;
    KeAcquireSpinLock(&bus->lock, &old);
    for (size_t i = 0; i < WL_VG_MAX; i++) {
        wl_vg_record *r = &bus->vg_records[i];
        if (r->vg_uuid[0] != '\0' && r->refs == 0) {
            for (size_t p = 0; p < r->pv_count && p < WL_VG_MAX_PVS; p++) {
                if (r->pv_tag[p] && tag_count < WL_VG_MAX * WL_VG_MAX_PVS) {
                    tags[tag_count++] = (WINLVM_PV_TAG *)r->pv_tag[p];
                    r->pv_tag[p] = NULL;
                }
            }
            if (bus->vg_views[i] && view_count < WL_VG_MAX) {
                views[view_count++] = bus->vg_views[i];
                bus->vg_views[i] = NULL;
            }
            RtlZeroMemory(r, sizeof(*r));
        }
    }
    KeReleaseSpinLock(&bus->lock, old);

    for (size_t i = 0; i < tag_count; i++) WinLvmFreePvTag(tags[i]);
    for (size_t i = 0; i < view_count; i++) ExFreePoolWithTag(views[i], WINLVM_WIN_TAG);
}

void WinLvmFreeReconcileDrops(WINLVM_RECONCILE_DROPS *drops) {
    if (!drops) return;
    for (size_t i = 0; i < drops->count && i < WL_MAX_DROPPED_REFS; i++) {
        if (drops->pdos[i]) ObDereferenceObject(drops->pdos[i]);
        if (drops->files[i]) ObDereferenceObject(drops->files[i]);
    }
    ExFreePoolWithTag(drops, WINLVM_WIN_TAG);
}

BOOLEAN WinLvmQuiesceRecordChildren(WINLVM_BUS_EXT *bus, const wl_vg_record *r,
                                    PDEVICE_OBJECT *out_pdos, size_t *out_count) {
    *out_count = 0;
    if (!bus || !r) return FALSE;

    KIRQL old;
    size_t n = 0;
    KeAcquireSpinLock(&bus->lock, &old);
    for (size_t i = 0; i < bus->child_count && n < WL_MAX_CHILDREN; i++) {
        PDEVICE_OBJECT child = bus->children[i];
        WINLVM_LV_EXT *lext = (WINLVM_LV_EXT *)child->DeviceExtension;
        if (lext->record == r) {
            ObReferenceObject(child);
            out_pdos[n++] = child;
        }
    }
    KeReleaseSpinLock(&bus->lock, old);

    for (size_t i = 0; i < n; i++) {
        WINLVM_LV_EXT *lext = (WINLVM_LV_EXT *)out_pdos[i]->DeviceExtension;
        if (WinLvmIoGateBeginQuiesce(&lext->io_gate) != STATUS_SUCCESS) {
            for (size_t j = 0; j < i; j++) {
                WINLVM_LV_EXT *prev = (WINLVM_LV_EXT *)out_pdos[j]->DeviceExtension;
                WinLvmIoGateResume(&prev->io_gate, out_pdos[j]);
            }
            for (size_t j = 0; j < n; j++) ObDereferenceObject(out_pdos[j]);
            return FALSE;
        }
    }
    *out_count = n;
    return TRUE;
}

void WinLvmResumeRecordChildren(PDEVICE_OBJECT *pdos, size_t count) {
    for (size_t i = 0; i < count; i++) {
        WINLVM_LV_EXT *lext = (WINLVM_LV_EXT *)pdos[i]->DeviceExtension;
        WinLvmIoGateResume(&lext->io_gate, pdos[i]);
        ObDereferenceObject(pdos[i]);
    }
}

BOOLEAN WinLvmAllRecordChildrenQuiescedLocked(WINLVM_BUS_EXT *bus, const wl_vg_record *r) {
    if (!bus || !r) return FALSE;
    for (size_t i = 0; i < bus->child_count; i++) {
        WINLVM_LV_EXT *lext = (WINLVM_LV_EXT *)bus->children[i]->DeviceExtension;
        if (lext->record == r && !WinLvmIoGateIsQuiesced(&lext->io_gate)) return FALSE;
    }
    return TRUE;
}

/* Non-blocking seam callback invoked under bus->lock by wl_vg_arrival. */
wl_update_result WinLvmUpdateLvSeam(void *ctx, const char *lv_uuid) {
    WINLVM_BUS_EXT *bus = (WINLVM_BUS_EXT *)ctx;
    if (!bus) bus = g_bus_ext;
    if (!bus || !lv_uuid) return WL_UPDATE_UNAVAILABLE;

    for (size_t i = 0; i < bus->child_count; i++) {
        WINLVM_LV_EXT *lv = (WINLVM_LV_EXT *)bus->children[i]->DeviceExtension;
        if (RtlEqualMemory(lv->lv_uuid, lv_uuid, 32)) {
            return WinLvmIoGateIsQuiesced(&lv->io_gate) ? WL_UPDATE_OK : WL_UPDATE_UNAVAILABLE;
        }
    }
    return WL_UPDATE_OK;
}

void WinLvmSwapRecordViewLocked(wl_vg_record *r,
                                WINLVM_VIEW_BLOCK **view_slot,
                                WINLVM_VIEW_BLOCK *next_block,
                                WINLVM_VIEW_BLOCK **old_view_out,
                                WINLVM_PV_TAG **dropped_tags,
                                size_t *dropped_tag_count) {
    unsigned char next_act[(WL_VG_MAX_LVS + 7) / 8];
    RtlZeroMemory(next_act, sizeof(next_act));
    if (r->view && r->view->lvs) {
        for (size_t i = 0; i < r->view->lv_count && i < WL_VG_MAX_LVS; i++) {
            if (!wl_vg_lv_activated(r, i)) continue;
            for (size_t k = 0; k < next_block->vg.lv_count && k < WL_VG_MAX_LVS; k++) {
                if (RtlEqualMemory(r->view->lvs[i].id, next_block->vg.lvs[k].id, 32)) {
                    next_act[k >> 3] |= (unsigned char)(1u << (k & 7));
                    break;
                }
            }
        }
    }
    RtlCopyMemory(r->lv_activated, next_act, sizeof(next_act));

    char new_uuid[WL_VG_MAX_PVS][WL_VG_UUID_LEN];
    wl_pv_state new_state[WL_VG_MAX_PVS];
    const void *new_tag[WL_VG_MAX_PVS];
    for (size_t i = 0; i < next_block->vg.pv_count; i++) {
        RtlCopyMemory(new_uuid[i], next_block->vg.pvs[i].id, WL_VG_UUID_LEN);
        new_uuid[i][WL_VG_UUID_LEN - 1] = '\0';
        new_state[i] = WL_PV_MISSING;
        new_tag[i] = NULL;
        for (size_t old_p = 0; old_p < r->pv_count; old_p++) {
            if (RtlEqualMemory(r->pv_uuid[old_p], new_uuid[i], 32)) {
                new_state[i] = r->pv_state[old_p];
                new_tag[i] = r->pv_tag[old_p];
                break;
            }
        }
    }
    for (size_t old_p = 0; old_p < r->pv_count; old_p++) {
        if (!r->pv_tag[old_p]) continue;
        BOOLEAN kept = FALSE;
        for (size_t i = 0; i < next_block->vg.pv_count; i++) {
            if (new_tag[i] == r->pv_tag[old_p]) { kept = TRUE; break; }
        }
        if (!kept && *dropped_tag_count < WL_VG_MAX_PVS) {
            dropped_tags[(*dropped_tag_count)++] = (WINLVM_PV_TAG *)r->pv_tag[old_p];
        }
    }
    r->pv_count = next_block->vg.pv_count;
    size_t present = 0, gone = 0;
    for (size_t i = 0; i < r->pv_count; i++) {
        RtlCopyMemory(r->pv_uuid[i], new_uuid[i], WL_VG_UUID_LEN);
        r->pv_state[i] = new_state[i];
        r->pv_tag[i] = new_tag[i];
        if (new_state[i] == WL_PV_PRESENT) present++;
        else if (new_state[i] == WL_PV_GONE) gone++;
    }
    r->state = (present == r->pv_count) ? WL_VG_COMPLETE : WL_VG_INCOMPLETE;
    r->dead = (r->pv_count > 0 && present == 0 && gone == r->pv_count) ? 1 : 0;
    RtlZeroMemory(r->vg_name, sizeof(r->vg_name));
    RtlCopyMemory(r->vg_name, next_block->vg.name, sizeof(r->vg_name) - 1);

    *old_view_out = *view_slot;
    *view_slot = next_block;
    r->view = &next_block->vg;
    r->seqno = next_block->vg.seqno;
    r->stale = 0;
}

void WinLvmReconcileChildrenLocked(WINLVM_BUS_EXT *bus, wl_vg_record *r,
                                   WINLVM_RECONCILE_DROPS *drops) {
    const lvmcore_volume_group *v = r ? r->view : NULL;
    if (!bus || !r || !v || !v->lvs) return;

    for (size_t c = 0; c < bus->child_count; c++) {
        WINLVM_LV_EXT *lext = (WINLVM_LV_EXT *)bus->children[c]->DeviceExtension;
        if (lext->record != r) continue;

        size_t match = SIZE_MAX;
        for (size_t k = 0; k < v->lv_count; k++) {
            if (RtlEqualMemory(v->lvs[k].id, lext->lv_uuid, 32)) { match = k; break; }
        }
        size_t slots[WL_VG_MAX_PVS];
        size_t slot_count = 0;
        ULONG max_stack = 0, max_logical = 512, max_physical = 512;
        uint64_t new_size = (match != SIZE_MAX) ? wl_lv_byte_size(v, match) : 0;

        if (match == SIZE_MAX || new_size == 0 ||
            !wl_vg_lv_child_eligible(&v->lvs[match]) ||
            !wl_vg_lv_pv_complete(r, match) ||
            !WinLvmCollectLvBackingSlotsLocked(r, match, slots, &slot_count,
                                               &max_stack, &max_logical, &max_physical)) {
            lext->unavailable = TRUE;
            continue;
        }

        PDEVICE_OBJECT new_pdo[WL_VG_MAX_PVS];
        PFILE_OBJECT new_file[WL_VG_MAX_PVS];
        ULONG new_logical[WL_VG_MAX_PVS];
        ULONG new_physical[WL_VG_MAX_PVS];
        uint32_t new_slot[WL_VG_MAX_PVS];

        for (size_t i = 0; i < lext->backing_count; i++) {
            BOOLEAN still_used = FALSE;
            for (size_t j = 0; j < slot_count; j++) {
                const WINLVM_PV_TAG *tag = (const WINLVM_PV_TAG *)r->pv_tag[slots[j]];
                if (lext->backing_pdo[i] == tag->backing_pdo &&
                    lext->backing_file[i] == tag->backing_file) {
                    still_used = TRUE;
                    break;
                }
            }
            if (!still_used && drops && drops->count < WL_MAX_DROPPED_REFS) {
                drops->pdos[drops->count] = lext->backing_pdo[i];
                drops->files[drops->count] = lext->backing_file[i];
                drops->count++;
            }
        }

        for (size_t j = 0; j < slot_count; j++) {
            const WINLVM_PV_TAG *tag = (const WINLVM_PV_TAG *)r->pv_tag[slots[j]];
            BOOLEAN was_used = FALSE;
            for (size_t i = 0; i < lext->backing_count; i++) {
                if (lext->backing_pdo[i] == tag->backing_pdo &&
                    lext->backing_file[i] == tag->backing_file) {
                    was_used = TRUE;
                    break;
                }
            }
            if (!was_used) {
                ObReferenceObject(tag->backing_pdo);
                ObReferenceObject(tag->backing_file);
            }
            new_pdo[j] = tag->backing_pdo;
            new_file[j] = tag->backing_file;
            new_logical[j] = tag->logical_sector;
            new_physical[j] = tag->physical_sector;
            new_slot[j] = (uint32_t)slots[j];
        }

        for (size_t j = 0; j < slot_count; j++) {
            lext->backing_pdo[j] = new_pdo[j];
            lext->backing_file[j] = new_file[j];
            lext->backing_logical[j] = new_logical[j];
            lext->backing_physical[j] = new_physical[j];
            lext->pv_slot[j] = new_slot[j];
        }
        lext->backing_count = slot_count;
        lext->lv_index = match;
        lext->byte_size = new_size;
        lext->logical_sector = max_logical;
        lext->physical_sector = max_physical;
        lext->read_only = WinLvmComputeLvReadOnly(r, match, slots, slot_count);
        lext->unavailable = FALSE;
        lext->pdo->SectorSize = (USHORT)max_logical;
        if (max_stack > 0) lext->pdo->StackSize = (UCHAR)(max_stack + 1);
    }
}

static NTSTATUS WinLvmSyncCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static BOOLEAN WinLvmChildIsReportable(const WINLVM_LV_EXT *lext) {
    if (!lext || !lext->record || lext->record->dead) return FALSE;
    if (lext->unavailable || lext->deactivating) return FALSE;
    if (!wl_vg_lv_activated(lext->record, lext->lv_index)) return FALSE;
    if (!wl_vg_lv_pv_complete(lext->record, lext->lv_index)) return FALSE;
    return TRUE;
}

static NTSTATUS WinLvmQueryBusRelations(WINLVM_BUS_EXT *bus, PIRP Irp) {
    PDEVICE_OBJECT orphans[WL_MAX_CHILDREN];
    size_t orphan_count = 0;
    KIRQL old;

    KeAcquireSpinLock(&bus->lock, &old);
    size_t kept = 0;
    for (size_t i = 0; i < bus->child_count; i++) {
        PDEVICE_OBJECT child = bus->children[i];
        WINLVM_LV_EXT *lext = (WINLVM_LV_EXT *)child->DeviceExtension;
        if (!lext->reported_to_pnp && !WinLvmChildIsReportable(lext) &&
            orphan_count < WL_MAX_CHILDREN) {
            orphans[orphan_count++] = child;
            continue;
        }
        bus->children[kept] = child;
        if (kept != i) bus->children[i] = NULL;
        kept++;
    }
    bus->child_count = kept;
    KeReleaseSpinLock(&bus->lock, old);

    for (size_t i = 0; i < orphan_count; i++) WinLvmDeleteLvPdo(orphans[i]);

    PDEVICE_RELATIONS rel = NULL;
    size_t alloc_cap = 0;
    for (;;) {
        KeAcquireSpinLock(&bus->lock, &old);
        size_t active_count = 0;
        for (size_t i = 0; i < bus->child_count; i++) {
            WINLVM_LV_EXT *lext = (WINLVM_LV_EXT *)bus->children[i]->DeviceExtension;
            if (WinLvmChildIsReportable(lext)) active_count++;
        }
        if (rel && active_count <= alloc_cap) {
            rel->Count = 0;
            for (size_t i = 0; i < bus->child_count; i++) {
                PDEVICE_OBJECT child = bus->children[i];
                WINLVM_LV_EXT *lext = (WINLVM_LV_EXT *)child->DeviceExtension;
                if (WinLvmChildIsReportable(lext)) {
                    lext->reported_to_pnp = TRUE;
                    rel->Objects[rel->Count++] = child;
                    ObReferenceObject(child);
                }
            }
            size_t total = bus->child_count;
            KeReleaseSpinLock(&bus->lock, old);
            WL_TRACE_2("QDR bus relations: %u active of %u total children",
                       (unsigned)rel->Count, (unsigned)total);
            break;
        }
        KeReleaseSpinLock(&bus->lock, old);

        if (rel) { ExFreePoolWithTag(rel, WINLVM_WIN_TAG); rel = NULL; }
        alloc_cap = active_count;
        size_t bytes = sizeof(DEVICE_RELATIONS) + alloc_cap * sizeof(PDEVICE_OBJECT);
        rel = (PDEVICE_RELATIONS)ExAllocatePoolZero(NonPagedPool, bytes, WINLVM_WIN_TAG);
        if (!rel) return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Irp->IoStatus.Information) ExFreePool((PVOID)Irp->IoStatus.Information);
    Irp->IoStatus.Information = (ULONG_PTR)rel;
    Irp->IoStatus.Status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

NTSTATUS WinLvmBusPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    if (kind != WL_EXT_BUS) {
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
    }

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    WINLVM_BUS_EXT *ext = (WINLVM_BUS_EXT *)DeviceObject->DeviceExtension;

    switch (irpSp->MinorFunction) {
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (irpSp->Parameters.QueryDeviceRelations.Type == BusRelations) {
            NTSTATUS st = WinLvmQueryBusRelations(ext, Irp);
            Irp->IoStatus.Status = st;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return st;
        }
        break;

    case IRP_MN_START_DEVICE: {
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
            WinLvmWmiInit(DeviceObject);
            WinLvmProbeSubsystemResume();
            WinLvmRegisterDiskNotifyDeferred(DeviceObject->DriverObject);
        }
        Irp->IoStatus.Status = st;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return st;
    }

    case IRP_MN_REMOVE_DEVICE: {
        WinLvmStopAndDrainProbeWork();

        PDEVICE_OBJECT remaining_children[WL_MAX_CHILDREN];
        size_t remaining_count = 0;
        WINLVM_PV_TAG *tags[WL_VG_MAX * WL_VG_MAX_PVS];
        size_t tag_count = 0;
        WINLVM_VIEW_BLOCK *views[WL_VG_MAX];
        size_t view_count = 0;

        KIRQL old;
        KeAcquireSpinLock(&ext->lock, &old);
        for (size_t i = 0; i < ext->child_count && remaining_count < WL_MAX_CHILDREN; i++) {
            remaining_children[remaining_count++] = ext->children[i];
            ext->children[i] = NULL;
        }
        ext->child_count = 0;
        for (size_t i = 0; i < WL_VG_MAX; i++) {
            wl_vg_record *r = &ext->vg_records[i];
            for (size_t p = 0; p < r->pv_count && p < WL_VG_MAX_PVS; p++) {
                if (r->pv_tag[p] && tag_count < WL_VG_MAX * WL_VG_MAX_PVS) {
                    tags[tag_count++] = (WINLVM_PV_TAG *)r->pv_tag[p];
                    r->pv_tag[p] = NULL;
                }
            }
            if (ext->vg_views[i]) {
                views[view_count++] = ext->vg_views[i];
                ext->vg_views[i] = NULL;
            }
            RtlZeroMemory(r, sizeof(*r));
        }
        if (g_bus_ext == ext) g_bus_ext = NULL;
        KeReleaseSpinLock(&ext->lock, old);

        for (size_t i = 0; i < remaining_count; i++) WinLvmDeleteLvPdo(remaining_children[i]);
        for (size_t i = 0; i < tag_count; i++) WinLvmFreePvTag(tags[i]);
        for (size_t i = 0; i < view_count; i++) ExFreePoolWithTag(views[i], WINLVM_WIN_TAG);

        WinLvmWmiDeregister(DeviceObject);
        PDEVICE_OBJECT lower = ext->bus_pdo;
        IoDetachDevice(lower);
        IoDeleteDevice(DeviceObject);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(lower, Irp);
    }

    default:
        break;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->bus_pdo, Irp);
}

NTSTATUS WinLvmAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject) {
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
    ext->kind = WL_EXT_BUS;
    ext->fdo = fdo;
    ext->pdo = PhysicalDeviceObject;
    KeInitializeSpinLock(&ext->lock);
    wl_vg_registry_init(ext->vg_records, WL_VG_MAX);

    ext->bus_pdo = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
    if (!ext->bus_pdo) {
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }

    g_bus_ext = ext;
    fdo->Flags |= DO_BUFFERED_IO;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
