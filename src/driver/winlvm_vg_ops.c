/*
 * winlvm_vg_ops.c — Recent PV path tracking, probe arrival commit,
 *                   child PDO rebinding, and LV activation/deactivation.
 */
#include "winlvm.h"

void WinLvmRecordRecentPvPathLocked(WINLVM_BUS_EXT *bus,
                                    const UNICODE_STRING *path) {
    if (!bus || !path || !path->Buffer || path->Length == 0) return;
    for (size_t i = 0; i < bus->recent_pv_count; i++) {
        if (WinLvmUnicodeEqualI(&bus->recent_pv_paths[i], path)) {
            return;
        }
    }
    USHORT len = path->Length;
    PWCHAR copy = (PWCHAR)ExAllocatePoolZero(
        NonPagedPool, (size_t)len + sizeof(WCHAR), WINLVM_WIN_TAG);
    if (!copy) return;
    RtlCopyMemory(copy, path->Buffer, len);

    if (bus->recent_pv_count >= WL_MAX_RECENT_PV_PATHS) {
        if (bus->recent_pv_paths[0].Buffer) {
            ExFreePoolWithTag(bus->recent_pv_paths[0].Buffer, WINLVM_WIN_TAG);
        }
        for (size_t i = 1; i < WL_MAX_RECENT_PV_PATHS; i++) {
            bus->recent_pv_paths[i - 1] = bus->recent_pv_paths[i];
        }
        bus->recent_pv_count = WL_MAX_RECENT_PV_PATHS - 1;
    }
    bus->recent_pv_paths[bus->recent_pv_count].Buffer = copy;
    bus->recent_pv_paths[bus->recent_pv_count].Length = len;
    bus->recent_pv_paths[bus->recent_pv_count].MaximumLength =
        (USHORT)(len + sizeof(WCHAR));
    bus->recent_pv_count++;
}

size_t WinLvmCopyRecentPvPaths(WINLVM_BUS_EXT *bus,
                               UNICODE_STRING *out, size_t cap) {
    if (!bus || !out || cap == 0) return 0;
    size_t copied = 0;
    KIRQL old;
    KeAcquireSpinLock(&bus->lock, &old);
    for (size_t i = 0; i < bus->recent_pv_count && copied < cap; i++) {
        const UNICODE_STRING *src = &bus->recent_pv_paths[i];
        if (!src->Buffer || src->Length == 0) continue;
        PWCHAR copy = (PWCHAR)ExAllocatePoolZero(
            NonPagedPool, (size_t)src->Length + sizeof(WCHAR), WINLVM_WIN_TAG);
        if (!copy) continue;
        RtlCopyMemory(copy, src->Buffer, src->Length);
        out[copied].Buffer = copy;
        out[copied].Length = src->Length;
        out[copied].MaximumLength = (USHORT)(src->Length + sizeof(WCHAR));
        copied++;
    }
    KeReleaseSpinLock(&bus->lock, old);
    return copied;
}

void WinLvmFreeRecentPvPathsLocked(WINLVM_BUS_EXT *bus,
                                   PWCHAR *bufs_out, size_t *count_out) {
    *count_out = 0;
    if (!bus) return;
    for (size_t i = 0; i < bus->recent_pv_count && i < WL_MAX_RECENT_PV_PATHS; i++) {
        if (bus->recent_pv_paths[i].Buffer) {
            bufs_out[(*count_out)++] = bus->recent_pv_paths[i].Buffer;
            RtlZeroMemory(&bus->recent_pv_paths[i], sizeof(UNICODE_STRING));
        }
    }
    bus->recent_pv_count = 0;
}

BOOLEAN WinLvmRebindChildPdo(WINLVM_BUS_EXT *bus, PDEVICE_OBJECT child) {
    if (!bus || !child) return FALSE;
    WINLVM_RECONCILE_DROPS *drops = (WINLVM_RECONCILE_DROPS *)ExAllocatePoolZero(
        NonPagedPool, sizeof(WINLVM_RECONCILE_DROPS), WINLVM_WIN_TAG);
    if (!drops) return FALSE;

    WINLVM_LV_EXT *ce = (WINLVM_LV_EXT *)child->DeviceExtension;
    if (WinLvmIoGateBeginQuiesce(&ce->io_gate) != STATUS_SUCCESS) {
        ExFreePoolWithTag(drops, WINLVM_WIN_TAG);
        return FALSE;
    }

    BOOLEAN ok = FALSE;
    KIRQL old;
    KeAcquireSpinLock(&bus->lock, &old);
    BOOLEAN present = FALSE;
    for (size_t i = 0; i < bus->child_count; i++) {
        if (bus->children[i] == child) { present = TRUE; break; }
    }
    if (present && ce->record && !ce->record->dead &&
        WinLvmReconcileSingleChildLocked(ce->record, ce, drops)) {
        ce->deactivating = FALSE;
        ce->unavailable = FALSE;
        wl_vg_lv_set_activated(ce->record, ce->lv_index, 1);
        ok = TRUE;
    }
    KeReleaseSpinLock(&bus->lock, old);

    WinLvmFreeReconcileDrops(drops);
    WinLvmIoGateResume(&ce->io_gate, child);
    if (ok) {
        WinLvmLvSetInterfaceEnabled(child, TRUE);
        WinLvmQueueMountMgrArrival(child);
    }
    return ok;
}

void WinLvmCollectExistingChildrenForActivationLocked(WINLVM_BUS_EXT *bus,
                                                      wl_vg_record *r,
                                                      size_t *newly,
                                                      size_t *newly_count,
                                                      BOOLEAN explicit_probe,
                                                      PDEVICE_OBJECT *revived_out,
                                                      size_t *revived_count_out) {
    *revived_count_out = 0;
    if (!bus || !r || r->dead || !r->view || !r->view->lvs) return;

    size_t kept_newly = 0;
    for (size_t n = 0; n < *newly_count; n++) {
        size_t k = newly[n];
        if (k >= r->view->lv_count) continue;
        PDEVICE_OBJECT existing = NULL;
        for (size_t c = 0; c < bus->child_count; c++) {
            WINLVM_LV_EXT *ce = (WINLVM_LV_EXT *)bus->children[c]->DeviceExtension;
            if (RtlEqualMemory(ce->lv_uuid, r->view->lvs[k].id, 32)) {
                existing = bus->children[c];
                break;
            }
        }
        if (existing) {
            if (*revived_count_out < WL_MAX_CHILDREN) {
                ObReferenceObject(existing);
                revived_out[(*revived_count_out)++] = existing;
            }
        } else {
            newly[kept_newly++] = k;
        }
    }
    *newly_count = kept_newly;

    for (size_t k = 0; k < r->view->lv_count && k < WL_VG_MAX_LVS; k++) {
        if (!wl_vg_lv_child_eligible(&r->view->lvs[k]) || !wl_vg_lv_pv_complete(r, k)) {
            continue;
        }
        PDEVICE_OBJECT existing = NULL;
        for (size_t c = 0; c < bus->child_count; c++) {
            WINLVM_LV_EXT *ce = (WINLVM_LV_EXT *)bus->children[c]->DeviceExtension;
            if (RtlEqualMemory(ce->lv_uuid, r->view->lvs[k].id, 32)) {
                existing = bus->children[c];
                break;
            }
        }
        if (existing) {
            WINLVM_LV_EXT *ce = (WINLVM_LV_EXT *)existing->DeviceExtension;
            BOOLEAN need_revive = (ce->unavailable || (explicit_probe && ce->deactivating));
            if (need_revive && *revived_count_out < WL_MAX_CHILDREN) {
                BOOLEAN seen = FALSE;
                for (size_t j = 0; j < *revived_count_out; j++) {
                    if (revived_out[j] == existing) { seen = TRUE; break; }
                }
                if (!seen) {
                    ObReferenceObject(existing);
                    revived_out[(*revived_count_out)++] = existing;
                }
            }
        } else if (explicit_probe && !wl_vg_lv_activated(r, k) &&
                   *newly_count < WL_VG_MAX_LVS) {
            BOOLEAN seen = FALSE;
            for (size_t j = 0; j < *newly_count; j++) {
                if (newly[j] == k) { seen = TRUE; break; }
            }
            if (!seen) newly[(*newly_count)++] = k;
        }
    }
}

static NTSTATUS WinLvmBusActivateLvOnce(WINLVM_BUS_EXT *bus,
                                        const char norm[WL_VG_UUID_LEN]) {
    KIRQL old;
    KeAcquireSpinLock(&bus->lock, &old);
    for (size_t i = 0; i < WL_VG_MAX; i++) {
        wl_vg_record *r = &bus->vg_records[i];
        const lvmcore_volume_group *view = r->view;
        if (!view || !view->lvs || r->pv_count == 0) continue;
        for (size_t k = 0; k < view->lv_count; k++) {
            if (!RtlEqualMemory(view->lvs[k].id, norm, 32)) continue;
            if (!wl_vg_lv_child_eligible(&view->lvs[k]) || !wl_vg_lv_pv_complete(r, k)) {
                KeReleaseSpinLock(&bus->lock, old);
                return STATUS_DEVICE_NOT_READY;
            }
            for (size_t c = 0; c < bus->child_count; c++) {
                PDEVICE_OBJECT child = bus->children[c];
                WINLVM_LV_EXT *ce = (WINLVM_LV_EXT *)child->DeviceExtension;
                if (RtlEqualMemory(ce->lv_uuid, norm, 32)) {
                    ObReferenceObject(child);
                    KeReleaseSpinLock(&bus->lock, old);
                    BOOLEAN ok = WinLvmRebindChildPdo(bus, child);
                    ObDereferenceObject(child);
                    if (!ok) return STATUS_UNSUCCESSFUL;
                    IoInvalidateDeviceRelations(bus->pdo, BusRelations);
                    return STATUS_SUCCESS;
                }
            }
            wl_vg_lv_set_activated(r, k, 1);
            KeReleaseSpinLock(&bus->lock, old);
            if (WinLvmProbeWorkerCreateLvPdo(bus, r, k)) {
                IoInvalidateDeviceRelations(bus->pdo, BusRelations);
                return STATUS_SUCCESS;
            }
            KeAcquireSpinLock(&bus->lock, &old);
            wl_vg_lv_set_activated(r, k, 0);
            KeReleaseSpinLock(&bus->lock, old);
            return STATUS_UNSUCCESSFUL;
        }
    }
    KeReleaseSpinLock(&bus->lock, old);
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS WinLvmBusActivateLvByUuid(WINLVM_BUS_EXT *bus, const char *lv_uuid) {
    char norm[WL_VG_UUID_LEN];
    if (!bus || !winlvm_normalize_uuid(lv_uuid, WINLVM_LV_UUID_WIRE_LEN, norm)) {
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS st = WinLvmBusActivateLvOnce(bus, norm);
    if (st == STATUS_OBJECT_NAME_NOT_FOUND || st == STATUS_DEVICE_NOT_READY) {
        UNICODE_STRING paths[WL_MAX_RECENT_PV_PATHS];
        size_t n = WinLvmCopyRecentPvPaths(bus, paths, WL_MAX_RECENT_PV_PATHS);
        for (size_t i = 0; i < n; i++) {
            (void)WinLvmProbeDevicePathEx(&paths[i], FALSE, TRUE);
            ExFreePoolWithTag(paths[i].Buffer, WINLVM_WIN_TAG);
        }
        if (n > 0) st = WinLvmBusActivateLvOnce(bus, norm);
    }
    return st;
}

NTSTATUS WinLvmBusDeactivateLvByUuid(WINLVM_BUS_EXT *bus, const char *lv_uuid) {
    char norm[WL_VG_UUID_LEN];
    if (!bus || !winlvm_normalize_uuid(lv_uuid, WINLVM_LV_UUID_WIRE_LEN, norm)) {
        return STATUS_INVALID_PARAMETER;
    }

    KIRQL old;
    PDEVICE_OBJECT target_pdo = NULL;
    size_t rec_slot = SIZE_MAX;
    KeAcquireSpinLock(&bus->lock, &old);
    for (size_t i = 0; i < bus->child_count; i++) {
        WINLVM_LV_EXT *lv = (WINLVM_LV_EXT *)bus->children[i]->DeviceExtension;
        if (RtlEqualMemory(lv->lv_uuid, norm, 32)) {
            target_pdo = bus->children[i];
            ObReferenceObject(target_pdo);
            lv->deactivating = TRUE;
            if (lv->record) {
                wl_vg_lv_set_activated(lv->record, lv->lv_index, 0);
                if (lv->record >= bus->vg_records &&
                    lv->record < bus->vg_records + WL_VG_MAX) {
                    rec_slot = (size_t)(lv->record - bus->vg_records);
                }
            }
            break;
        }
    }
    KeReleaseSpinLock(&bus->lock, old);
    if (!target_pdo) return STATUS_OBJECT_NAME_NOT_FOUND;

    WINLVM_LV_EXT *target = (WINLVM_LV_EXT *)target_pdo->DeviceExtension;
    NTSTATUS qst = WinLvmIoGateBeginQuiesce(&target->io_gate);
    WinLvmLvSetInterfaceEnabled(target_pdo, FALSE);

    PDEVICE_OBJECT drop_pdos[WL_VG_MAX_PVS];
    PFILE_OBJECT drop_files[WL_VG_MAX_PVS];
    size_t drop_count = 0;
    WINLVM_PV_TAG *detached_tags[WL_VG_MAX_PVS];
    size_t detached_count = 0;

    KeAcquireSpinLock(&bus->lock, &old);
    if (qst == STATUS_SUCCESS) {
        WinLvmDetachChildBackingLocked(target, drop_pdos, drop_files, &drop_count);
        target->drive_letter = 0;
    }
    if (rec_slot < WL_VG_MAX) {
        wl_vg_record *rec = &bus->vg_records[rec_slot];
        BOOLEAN any_live = FALSE;
        for (size_t i = 0; i < bus->child_count; i++) {
            WINLVM_LV_EXT *ce = (WINLVM_LV_EXT *)bus->children[i]->DeviceExtension;
            if (ce->record == rec && !ce->deactivating && !ce->unavailable &&
                wl_vg_lv_activated(rec, ce->lv_index)) {
                any_live = TRUE;
                break;
            }
        }
        if (!any_live && rec->vg_uuid[0] != '\0') {
            for (size_t p = 0; p < rec->pv_count && p < WL_VG_MAX_PVS; p++) {
                WINLVM_PV_TAG *tag = (WINLVM_PV_TAG *)rec->pv_tag[p];
                if (tag) {
                    WinLvmRecordRecentPvPathLocked(bus, &tag->disk_ifname);
                    if (detached_count < WL_VG_MAX_PVS) {
                        detached_tags[detached_count++] = tag;
                    }
                    rec->pv_tag[p] = NULL;
                }
                if (rec->pv_state[p] == WL_PV_PRESENT) {
                    wl_vg_pv_removed(bus->vg_records, WL_VG_MAX, rec->pv_uuid[p]);
                }
            }
        }
    }
    KeReleaseSpinLock(&bus->lock, old);

    if (qst == STATUS_SUCCESS) {
        WinLvmIoGateResume(&target->io_gate, target_pdo);
    }
    for (size_t i = 0; i < drop_count; i++) {
        if (drop_pdos[i]) ObDereferenceObject(drop_pdos[i]);
        if (drop_files[i]) ObDereferenceObject(drop_files[i]);
    }
    for (size_t i = 0; i < detached_count; i++) WinLvmFreePvTag(detached_tags[i]);
    ObDereferenceObject(target_pdo);
    if (rec_slot < WL_VG_MAX) WinLvmBusResetRecordIfInactive(bus, rec_slot);
    IoInvalidateDeviceRelations(bus->pdo, BusRelations);
    return STATUS_SUCCESS;
}

wl_arrival_result WinLvmProbeCommitArrivalLocked(WINLVM_BUS_EXT *bus,
                                                 WINLVM_VIEW_BLOCK **block_io,
                                                 const char pv_uuid[WL_VG_UUID_LEN],
                                                 WINLVM_PV_TAG *tag,
                                                 BOOLEAN quiesce_ok,
                                                 BOOLEAN explicit_probe,
                                                 WINLVM_RECONCILE_DROPS *drops,
                                                 WINLVM_PROBE_COMMIT *out) {
    RtlZeroMemory(out, sizeof(*out));
    WINLVM_VIEW_BLOCK *block = *block_io;
    WINLVM_PV_TAG *old_tags[WL_VG_MAX_PVS];
    size_t old_tag_count = 0;
    PDEVICE_OBJECT pre_unavail[WL_MAX_CHILDREN];
    size_t pre_unavail_count = 0;
    BOOLEAN register_tn = FALSE;

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
        bus->vg_records, WL_VG_MAX, &out->slot, &block->vg,
        pv_uuid, tag, seam, bus, out->newly, WL_VG_MAX_LVS, &out->newly_count);

    if ((res == WL_ARR_RECONCILED) ||
        (existed_idx < 0 && (res == WL_ARR_COMPLETED || res == WL_ARR_PENDING))) {
        if (res == WL_ARR_RECONCILED) out->old_view = bus->vg_views[out->slot];
        bus->vg_views[out->slot] = block;
        *block_io = NULL;
    }
    if (res == WL_ARR_RECONCILED) {
        for (size_t c = 0; c < bus->child_count && pre_unavail_count < WL_MAX_CHILDREN; c++) {
            WINLVM_LV_EXT *ce = (WINLVM_LV_EXT *)bus->children[c]->DeviceExtension;
            if (ce->record == &bus->vg_records[out->slot] && ce->unavailable) {
                pre_unavail[pre_unavail_count++] = bus->children[c];
            }
        }
        WinLvmReconcileChildrenLocked(bus, &bus->vg_records[out->slot], drops);
    }

    if (res != WL_ARR_REJECTED && res != WL_ARR_DUPLICATE) {
        wl_vg_record *r = &bus->vg_records[out->slot];
        for (size_t oi = 0; oi < old_tag_count; oi++) {
            BOOLEAN still_present = FALSE;
            for (size_t p = 0; p < r->pv_count; p++) {
                if (r->pv_tag[p] == old_tags[oi]) { still_present = TRUE; break; }
            }
            if (!still_present) out->tags_to_free[out->free_tag_count++] = old_tags[oi];
        }
        BOOLEAN tag_kept = FALSE;
        for (size_t p = 0; p < r->pv_count; p++) {
            if (r->pv_tag[p] == tag) { tag_kept = TRUE; break; }
        }
        if (tag_kept) {
            WinLvmRecordRecentPvPathLocked(bus, &tag->disk_ifname);
            register_tn = TRUE;
        } else {
            out->tags_to_free[out->free_tag_count++] = tag;
        }
        WinLvmCollectExistingChildrenForActivationLocked(
            bus, r, out->newly, &out->newly_count, explicit_probe,
            out->revived, &out->revived_count);
        for (size_t i = 0; i < pre_unavail_count && out->revived_count < WL_MAX_CHILDREN; i++) {
            WINLVM_LV_EXT *ce = (WINLVM_LV_EXT *)pre_unavail[i]->DeviceExtension;
            if (!ce->unavailable && !ce->deactivating) {
                BOOLEAN seen = FALSE;
                for (size_t j = 0; j < out->revived_count; j++) {
                    if (out->revived[j] == pre_unavail[i]) { seen = TRUE; break; }
                }
                if (!seen) {
                    ObReferenceObject(pre_unavail[i]);
                    out->revived[out->revived_count++] = pre_unavail[i];
                }
            }
        }
        for (size_t i = 0; i < out->newly_count; i++) wl_vg_lv_set_activated(r, out->newly[i], 1);
    } else if (res == WL_ARR_DUPLICATE && explicit_probe && existed_idx >= 0) {
        out->slot = (size_t)existed_idx;
        wl_vg_record *r = &bus->vg_records[out->slot];
        BOOLEAN swapped = FALSE;
        if (r->refs == 0) {
            for (size_t p = 0; p < r->pv_count && p < WL_VG_MAX_PVS; p++) {
                if (RtlEqualMemory(r->pv_uuid[p], pv_uuid, 32)) {
                    if (r->pv_tag[p]) {
                        out->tags_to_free[out->free_tag_count++] = (WINLVM_PV_TAG *)r->pv_tag[p];
                    }
                    r->pv_tag[p] = tag;
                    WinLvmRecordRecentPvPathLocked(bus, &tag->disk_ifname);
                    swapped = TRUE;
                    register_tn = TRUE;
                    break;
                }
            }
        }
        if (!swapped) out->tags_to_free[out->free_tag_count++] = tag;
        WinLvmCollectExistingChildrenForActivationLocked(
            bus, r, out->newly, &out->newly_count, TRUE,
            out->revived, &out->revived_count);
        for (size_t i = 0; i < out->newly_count; i++) wl_vg_lv_set_activated(r, out->newly[i], 1);
    } else {
        out->tags_to_free[out->free_tag_count++] = tag;
    }

    if (register_tn && tag->backing_file && tag->disk_ifname.Buffer &&
        tag->disk_ifname.Length > 0) {
        PWCHAR pcopy = (PWCHAR)ExAllocatePoolZero(
            NonPagedPool, (size_t)tag->disk_ifname.Length + sizeof(WCHAR), WINLVM_WIN_TAG);
        if (pcopy) {
            RtlCopyMemory(pcopy, tag->disk_ifname.Buffer, tag->disk_ifname.Length);
            out->tn_path.Buffer = pcopy;
            out->tn_path.Length = tag->disk_ifname.Length;
            out->tn_path.MaximumLength = (USHORT)(tag->disk_ifname.Length + sizeof(WCHAR));
            out->tn_file_ref = tag->backing_file;
            ObReferenceObject(out->tn_file_ref);
        }
    }
    return res;
}
