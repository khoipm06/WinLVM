/*
 * winlvm_control.c — \Device\WinLvmControl IOCTL dispatch plane.
 */
#include "winlvm.h"

NTSTATUS WinLvmBusActivateLvByUuid(WINLVM_BUS_EXT *bus, const char *lv_uuid) {
    char norm[WL_VG_UUID_LEN];
    if (!bus || !winlvm_normalize_uuid(lv_uuid, WINLVM_LV_UUID_WIRE_LEN, norm)) {
        return STATUS_INVALID_PARAMETER;
    }

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
                    ce->deactivating = FALSE;
                    ce->unavailable = FALSE;
                    wl_vg_lv_set_activated(r, k, 1);
                    KeReleaseSpinLock(&bus->lock, old);
                    if (ce->ifname.Buffer) IoSetDeviceInterfaceState(&ce->ifname, TRUE);
                    ObDereferenceObject(child);
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

NTSTATUS WinLvmBusDeactivateLvByUuid(WINLVM_BUS_EXT *bus, const char *lv_uuid) {
    char norm[WL_VG_UUID_LEN];
    if (!bus || !winlvm_normalize_uuid(lv_uuid, WINLVM_LV_UUID_WIRE_LEN, norm)) {
        return STATUS_INVALID_PARAMETER;
    }

    KIRQL old;
    PDEVICE_OBJECT target_pdo = NULL;
    KeAcquireSpinLock(&bus->lock, &old);
    for (size_t i = 0; i < bus->child_count; i++) {
        WINLVM_LV_EXT *lv = (WINLVM_LV_EXT *)bus->children[i]->DeviceExtension;
        if (RtlEqualMemory(lv->lv_uuid, norm, 32)) {
            target_pdo = bus->children[i];
            ObReferenceObject(target_pdo);
            lv->deactivating = TRUE;
            if (lv->record) wl_vg_lv_set_activated(lv->record, lv->lv_index, 0);
            break;
        }
    }
    KeReleaseSpinLock(&bus->lock, old);
    if (!target_pdo) return STATUS_OBJECT_NAME_NOT_FOUND;

    WINLVM_LV_EXT *target = (WINLVM_LV_EXT *)target_pdo->DeviceExtension;
    WinLvmIoGateBeginQuiesce(&target->io_gate);
    if (target->ifname.Buffer) IoSetDeviceInterfaceState(&target->ifname, FALSE);
    WinLvmIoGateResume(&target->io_gate, target_pdo);
    ObDereferenceObject(target_pdo);
    IoInvalidateDeviceRelations(bus->pdo, BusRelations);
    return STATUS_SUCCESS;
}

static NTSTATUS WinLvmHandleUpdateLv(const WINLVM_UPDATE_LV_REQUEST *req, ULONG input_length) {
    char norm[WL_VG_UUID_LEN];
    if (!req || input_length < sizeof(*req) ||
        req->header.version != WINLVM_ABI_VERSION ||
        req->header.size < sizeof(*req) ||
        !winlvm_normalize_uuid(req->lv_uuid, WINLVM_LV_UUID_WIRE_LEN, norm)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (req->byte_size == 0 || req->logical_sector < 512 ||
        req->physical_sector < req->logical_sector ||
        (req->logical_sector & (req->logical_sector - 1)) != 0 ||
        (req->physical_sector & (req->physical_sector - 1)) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    WINLVM_BUS_EXT *bus = g_bus_ext;
    if (!bus) return STATUS_DEVICE_NOT_READY;

    PDEVICE_OBJECT pv_pdos[WL_VG_MAX_PVS];
    PFILE_OBJECT pv_files[WL_VG_MAX_PVS];
    ULONG pv_sectors[WL_VG_MAX_PVS];
    size_t pv_count = 0;
    size_t rec_slot = SIZE_MAX;
    char vg_uuid[WL_VG_UUID_LEN];
    RtlZeroMemory(vg_uuid, sizeof(vg_uuid));

    KIRQL old;
    KeAcquireSpinLock(&bus->lock, &old);
    for (size_t i = 0; i < bus->child_count; i++) {
        WINLVM_LV_EXT *lv = (WINLVM_LV_EXT *)bus->children[i]->DeviceExtension;
        if (RtlEqualMemory(lv->lv_uuid, norm, 32) && lv->record) {
            rec_slot = (size_t)(lv->record - bus->vg_records);
            if (rec_slot >= WL_VG_MAX) { rec_slot = SIZE_MAX; break; }
            RtlCopyMemory(vg_uuid, lv->record->vg_uuid, WL_VG_UUID_LEN);
            for (size_t p = 0; p < lv->record->pv_count && p < WL_VG_MAX_PVS; p++) {
                const WINLVM_PV_TAG *tag = (const WINLVM_PV_TAG *)lv->record->pv_tag[p];
                if (lv->record->pv_state[p] == WL_PV_PRESENT && tag &&
                    tag->backing_pdo && tag->backing_file) {
                    ObReferenceObject(tag->backing_pdo);
                    ObReferenceObject(tag->backing_file);
                    pv_pdos[pv_count] = tag->backing_pdo;
                    pv_files[pv_count] = tag->backing_file;
                    pv_sectors[pv_count] = tag->logical_sector;
                    pv_count++;
                }
            }
            break;
        }
    }
    KeReleaseSpinLock(&bus->lock, old);
    if (rec_slot == SIZE_MAX) return STATUS_OBJECT_NAME_NOT_FOUND;

    WINLVM_VIEW_BLOCK *best = NULL;
    for (size_t p = 0; p < pv_count; p++) {
        WINLVM_VIEW_BLOCK *cand = NULL;
        if (NT_SUCCESS(WinLvmReadVgFromDevice(pv_pdos[p], pv_files[p],
                                              pv_sectors[p], &cand, NULL))) {
            if (RtlEqualMemory(cand->vg.id, vg_uuid, 32) &&
                (!best || cand->vg.seqno > best->vg.seqno)) {
                if (best) ExFreePoolWithTag(best, WINLVM_WIN_TAG);
                best = cand;
            } else {
                ExFreePoolWithTag(cand, WINLVM_WIN_TAG);
            }
        }
        ObDereferenceObject(pv_pdos[p]);
        ObDereferenceObject(pv_files[p]);
    }

    if (!best) return STATUS_DATA_ERROR;

    WINLVM_RECONCILE_DROPS *drops = (WINLVM_RECONCILE_DROPS *)ExAllocatePoolZero(
        NonPagedPool, sizeof(WINLVM_RECONCILE_DROPS), WINLVM_WIN_TAG);
    if (!drops) {
        ExFreePoolWithTag(best, WINLVM_WIN_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PDEVICE_OBJECT quiesced[WL_MAX_CHILDREN];
    size_t quiesced_count = 0;
    if (!WinLvmQuiesceRecordChildren(bus, &bus->vg_records[rec_slot],
                                     quiesced, &quiesced_count)) {
        ExFreePoolWithTag(drops, WINLVM_WIN_TAG);
        ExFreePoolWithTag(best, WINLVM_WIN_TAG);
        return STATUS_TIMEOUT;
    }

    WINLVM_VIEW_BLOCK *old_view = NULL;
    WINLVM_PV_TAG *dropped_tags[WL_VG_MAX_PVS];
    size_t dropped_tag_count = 0;
    NTSTATUS status = STATUS_OBJECT_NAME_NOT_FOUND;
    ULONGLONG updated_size = 0;

    KeAcquireSpinLock(&bus->lock, &old);
    wl_vg_record *r = &bus->vg_records[rec_slot];
    if (!r->vg_uuid[0] || !RtlEqualMemory(r->vg_uuid, vg_uuid, 32) ||
        best->vg.seqno < r->seqno || best->vg.pv_count == 0 ||
        best->vg.pv_count > WL_VG_MAX_PVS || best->vg.lv_count > WL_VG_MAX_LVS) {
        status = STATUS_DATA_ERROR;
    } else if (!WinLvmAllRecordChildrenQuiescedLocked(bus, r)) {
        status = STATUS_DEVICE_BUSY;
    } else {
        WinLvmSwapRecordViewLocked(r, &bus->vg_views[rec_slot], best,
                                   &old_view, dropped_tags, &dropped_tag_count);
        best = NULL;
        WinLvmReconcileChildrenLocked(bus, r, drops);

        for (size_t i = 0; i < bus->child_count; i++) {
            WINLVM_LV_EXT *lv = (WINLVM_LV_EXT *)bus->children[i]->DeviceExtension;
            if (RtlEqualMemory(lv->lv_uuid, norm, 32)) {
                if (!lv->unavailable) {
                    if (req->logical_sector > lv->logical_sector) {
                        lv->logical_sector = req->logical_sector;
                        lv->pdo->SectorSize = (USHORT)req->logical_sector;
                    }
                    if (req->physical_sector > lv->physical_sector &&
                        req->physical_sector >= lv->logical_sector) {
                        lv->physical_sector = req->physical_sector;
                    }
                    updated_size = lv->byte_size;
                    status = STATUS_SUCCESS;
                } else {
                    status = STATUS_DEVICE_NOT_CONNECTED;
                }
                break;
            }
        }
    }
    KeReleaseSpinLock(&bus->lock, old);

    if (status == STATUS_SUCCESS) {
        WL_TRACE_2("update lv: uuid %s size %I64u", norm, updated_size);
    }

    if (old_view) ExFreePoolWithTag(old_view, WINLVM_WIN_TAG);
    if (best) ExFreePoolWithTag(best, WINLVM_WIN_TAG);
    for (size_t i = 0; i < dropped_tag_count; i++) WinLvmFreePvTag(dropped_tags[i]);
    WinLvmFreeReconcileDrops(drops);
    WinLvmResumeRecordChildren(quiesced, quiesced_count);
    IoInvalidateDeviceRelations(bus->pdo, BusRelations);
    return status;
}

static NTSTATUS WinLvmHandleInventory(PVOID sysBuf, ULONG output_length, ULONG_PTR *info_out) {
    if (output_length < sizeof(WINLVM_INVENTORY)) {
        *info_out = sizeof(WINLVM_INVENTORY);
        return STATUS_BUFFER_TOO_SMALL;
    }
    WINLVM_INVENTORY *inv = (WINLVM_INVENTORY *)sysBuf;
    ULONG cap = (ULONG)((output_length - sizeof(WINLVM_INVENTORY)) /
                        sizeof(WINLVM_INVENTORY_LV_ENTRY));
    WINLVM_INVENTORY_LV_ENTRY *entries = (WINLVM_INVENTORY_LV_ENTRY *)(inv + 1);
    ULONG rec_count = 0;
    ULONG written = 0;

    WINLVM_BUS_EXT *bus = g_bus_ext;
    if (bus) {
        KIRQL old;
        KeAcquireSpinLock(&bus->lock, &old);
        for (size_t i = 0; i < WL_VG_MAX; i++) {
            wl_vg_record *r = &bus->vg_records[i];
            if (!r->vg_uuid[0] || r->dead || !r->view || !r->view->lvs) continue;
            rec_count++;
            const lvmcore_volume_group *v = r->view;
            for (size_t k = 0; k < v->lv_count && written < cap; k++) {
                const lvmcore_logical_volume *lv = &v->lvs[k];
                if (!wl_vg_lv_child_eligible(lv)) continue;
                WINLVM_INVENTORY_LV_ENTRY *e = &entries[written++];
                RtlZeroMemory(e, sizeof(*e));
                RtlCopyMemory(e->vg_name, r->vg_name, sizeof(e->vg_name) - 1);
                RtlCopyMemory(e->lv_name, lv->name, sizeof(e->lv_name) - 1);
                RtlCopyMemory(e->lv_uuid, lv->id, 32);
                e->byte_size = wl_lv_byte_size(v, k);
                e->logical_sector = 512;
                ULONG flags = 0;
                if (v->read_only || lv->read_only || v->system_id[0] != '\0') {
                    flags |= WINLVM_INV_LV_READONLY;
                }
                for (size_t s = 0; s < lv->segment_count; s++) {
                    if (lv->segments[s].stripe_count > 1) { flags |= WINLVM_INV_LV_STRIPED; break; }
                }
                for (size_t c = 0; c < bus->child_count; c++) {
                    WINLVM_LV_EXT *ce = (WINLVM_LV_EXT *)bus->children[c]->DeviceExtension;
                    if (RtlEqualMemory(ce->lv_uuid, lv->id, 32)) {
                        e->byte_size = ce->byte_size;
                        e->logical_sector = ce->logical_sector;
                        if (!ce->unavailable && !ce->deactivating && wl_vg_lv_activated(r, k)) {
                            flags |= WINLVM_INV_LV_ACTIVE;
                        }
                        if (ce->read_only) flags |= WINLVM_INV_LV_READONLY;
                        break;
                    }
                }
                e->flags = flags;
            }
        }
        KeReleaseSpinLock(&bus->lock, old);
    }

    inv->version = WINLVM_ABI_VERSION;
    inv->size = (ULONG)(sizeof(WINLVM_INVENTORY) + written * sizeof(WINLVM_INVENTORY_LV_ENTRY));
    inv->record_count = rec_count;
    inv->lv_entry_count = written;
    *info_out = inv->size;
    return STATUS_SUCCESS;
}

static NTSTATUS WinLvmHandleVgGateControl(PVOID sysBuf, ULONG input_length, BOOLEAN quiesce) {
    WINLVM_BUS_EXT *bus = g_bus_ext;
    if (!bus) return STATUS_SUCCESS;

    const char *filter_vg = NULL;
    if (input_length >= sizeof(WINLVM_VG_REQUEST)) {
        const WINLVM_VG_REQUEST *vreq = (const WINLVM_VG_REQUEST *)sysBuf;
        BOOLEAN nul_ok = FALSE;
        for (size_t i = 0; i < WINLVM_NAME_WIRE_LEN; i++) {
            if (vreq->vg_name[i] == '\0') { nul_ok = TRUE; break; }
        }
        if (!nul_ok) return STATUS_INVALID_PARAMETER;
        if (vreq->vg_name[0] != '\0') filter_vg = vreq->vg_name;
    }

    PDEVICE_OBJECT targets[WL_MAX_CHILDREN];
    size_t count = 0;
    KIRQL old;
    KeAcquireSpinLock(&bus->lock, &old);
    for (size_t i = 0; i < bus->child_count && count < WL_MAX_CHILDREN; i++) {
        PDEVICE_OBJECT child = bus->children[i];
        WINLVM_LV_EXT *lv = (WINLVM_LV_EXT *)child->DeviceExtension;
        if (filter_vg && (!lv->record || strcmp(lv->record->vg_name, filter_vg) != 0)) {
            continue;
        }
        ObReferenceObject(child);
        targets[count++] = child;
    }
    KeReleaseSpinLock(&bus->lock, old);

    NTSTATUS status = STATUS_SUCCESS;
    for (size_t i = 0; i < count; i++) {
        WINLVM_LV_EXT *lv = (WINLVM_LV_EXT *)targets[i]->DeviceExtension;
        if (quiesce) {
            NTSTATUS gs = WinLvmIoGateBeginQuiesce(&lv->io_gate);
            if (gs != STATUS_SUCCESS && gs != STATUS_PENDING) {
                for (size_t j = 0; j < i; j++) {
                    WINLVM_LV_EXT *prev = (WINLVM_LV_EXT *)targets[j]->DeviceExtension;
                    WinLvmIoGateResume(&prev->io_gate, targets[j]);
                }
                status = gs;
                break;
            }
        } else {
            WinLvmIoGateResume(&lv->io_gate, targets[i]);
        }
    }
    for (size_t i = 0; i < count; i++) ObDereferenceObject(targets[i]);
    return status;
}

static NTSTATUS WinLvmHandleRefresh(void) {
    WINLVM_BUS_EXT *bus = g_bus_ext;
    if (!bus) return STATUS_SUCCESS;

    WinLvmBusResetInactiveRecords(bus);

    UNICODE_STRING reprobe[WL_VG_MAX * WL_VG_MAX_PVS];
    size_t reprobe_count = 0;
    KIRQL old;
    KeAcquireSpinLock(&bus->lock, &old);
    for (size_t i = 0; i < WL_VG_MAX; i++) {
        wl_vg_record *r = &bus->vg_records[i];
        if (!r->vg_uuid[0]) continue;
        for (size_t p = 0; p < r->pv_count && p < WL_VG_MAX_PVS; p++) {
            const WINLVM_PV_TAG *tag = (const WINLVM_PV_TAG *)r->pv_tag[p];
            if (r->pv_state[p] == WL_PV_PRESENT && tag && tag->disk_ifname.Buffer &&
                tag->disk_ifname.Length > 0 && reprobe_count < WL_VG_MAX * WL_VG_MAX_PVS) {
                USHORT len = tag->disk_ifname.Length;
                PWCHAR copy = (PWCHAR)ExAllocatePoolZero(
                    NonPagedPool, (size_t)len + sizeof(WCHAR), WINLVM_WIN_TAG);
                if (copy) {
                    RtlCopyMemory(copy, tag->disk_ifname.Buffer, len);
                    reprobe[reprobe_count].Buffer = copy;
                    reprobe[reprobe_count].Length = len;
                    reprobe[reprobe_count].MaximumLength = (USHORT)(len + sizeof(WCHAR));
                    reprobe_count++;
                }
            }
        }
    }
    KeReleaseSpinLock(&bus->lock, old);

    for (size_t i = 0; i < reprobe_count; i++) {
        (void)WinLvmProbeDevicePath(&reprobe[i]);
        ExFreePoolWithTag(reprobe[i].Buffer, WINLVM_WIN_TAG);
    }

    WinLvmUnregisterDiskNotify();
    return WinLvmRegisterDiskNotify(bus->fdo->DriverObject);
}

static NTSTATUS WinLvmHandleProbeDevice(const WINLVM_PROBE_DEVICE_REQUEST *req, ULONG input_length) {
    if (!req || input_length < sizeof(*req) ||
        req->header.version != WINLVM_ABI_VERSION ||
        req->header.size < sizeof(*req)) {
        return STATUS_INVALID_PARAMETER;
    }
    size_t wlen = 0;
    while (wlen < WINLVM_PATH_WIRE_LEN && req->device_path[wlen] != 0) wlen++;
    if (wlen == 0 || wlen >= WINLVM_PATH_WIRE_LEN) return STATUS_INVALID_PARAMETER;

    UNICODE_STRING path;
    path.Buffer = (PWCH)req->device_path;
    path.Length = (USHORT)(wlen * sizeof(WCHAR));
    path.MaximumLength = (USHORT)((wlen + 1) * sizeof(WCHAR));
    return WinLvmProbeDevicePath(&path);
}

NTSTATUS WinLvmControlDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;
    ULONG input_length = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG output_length = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID sysBuf = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;
    UNREFERENCED_PARAMETER(DeviceObject);

    if (code == IOCTL_WINLVM_GET_LOG) {
        if (output_length > 0 && sysBuf) information = WinLvmLogDrain((char *)sysBuf, output_length);
        status = STATUS_SUCCESS;
    } else if (code == IOCTL_WINLVM_UPDATE_LV) {
        status = sysBuf ? WinLvmHandleUpdateLv((const WINLVM_UPDATE_LV_REQUEST *)sysBuf, input_length)
                        : STATUS_BUFFER_TOO_SMALL;
    } else if (code == IOCTL_WINLVM_PROBE_DEVICE) {
        status = sysBuf ? WinLvmHandleProbeDevice((const WINLVM_PROBE_DEVICE_REQUEST *)sysBuf, input_length)
                        : STATUS_BUFFER_TOO_SMALL;
    } else {
        const BOOLEAN known = (code == IOCTL_WINLVM_INVENTORY || code == IOCTL_WINLVM_REFRESH ||
                               code == IOCTL_WINLVM_QUIESCE_VG || code == IOCTL_WINLVM_RESUME_VG ||
                               code == IOCTL_WINLVM_ACTIVATE_LV || code == IOCTL_WINLVM_DEACTIVATE_LV);
        if (known && (!sysBuf || input_length < sizeof(WINLVM_CONTROL_REQUEST))) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else if (known) {
            const WINLVM_CONTROL_REQUEST *req = (const WINLVM_CONTROL_REQUEST *)sysBuf;
            if (req->version != WINLVM_ABI_VERSION || req->size < sizeof(*req)) {
                status = STATUS_INVALID_PARAMETER;
            } else if (code == IOCTL_WINLVM_INVENTORY) {
                status = WinLvmHandleInventory(sysBuf, output_length, &information);
            } else if (code == IOCTL_WINLVM_QUIESCE_VG) {
                status = WinLvmHandleVgGateControl(sysBuf, input_length, TRUE);
            } else if (code == IOCTL_WINLVM_RESUME_VG) {
                status = WinLvmHandleVgGateControl(sysBuf, input_length, FALSE);
            } else if (code == IOCTL_WINLVM_REFRESH) {
                status = WinLvmHandleRefresh();
            } else if (code == IOCTL_WINLVM_ACTIVATE_LV) {
                if (input_length < sizeof(WINLVM_LV_REQUEST) || req->size < sizeof(WINLVM_LV_REQUEST)) {
                    status = STATUS_BUFFER_TOO_SMALL;
                } else {
                    const WINLVM_LV_REQUEST *lreq = (const WINLVM_LV_REQUEST *)sysBuf;
                    status = g_bus_ext ? WinLvmBusActivateLvByUuid(g_bus_ext, lreq->lv_uuid)
                                       : STATUS_DEVICE_NOT_READY;
                }
            } else if (code == IOCTL_WINLVM_DEACTIVATE_LV) {
                if (input_length < sizeof(WINLVM_LV_REQUEST) || req->size < sizeof(WINLVM_LV_REQUEST)) {
                    status = STATUS_BUFFER_TOO_SMALL;
                } else {
                    const WINLVM_LV_REQUEST *lreq = (const WINLVM_LV_REQUEST *)sysBuf;
                    status = g_bus_ext ? WinLvmBusDeactivateLvByUuid(g_bus_ext, lreq->lv_uuid)
                                       : STATUS_DEVICE_NOT_READY;
                }
            }
        }
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
