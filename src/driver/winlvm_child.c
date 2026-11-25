/*
 * winlvm_child.c — Child LV raw-PDO creation, PnP dispatch, and teardown.
 */
#include "winlvm.h"
#include <ntstrsafe.h>

BOOLEAN WinLvmComputeLvReadOnly(const wl_vg_record *record, size_t lv_index,
                                const size_t *slots, size_t slot_count) {
    const lvmcore_volume_group *view = record ? record->view : NULL;
    if (!view || !view->lvs || lv_index >= view->lv_count) return TRUE;
    if (view->read_only || view->system_id[0] != '\0') return TRUE;
    if (view->lvs[lv_index].read_only) return TRUE;
    for (size_t i = 0; i < slot_count; i++) {
        const WINLVM_PV_TAG *tag = (const WINLVM_PV_TAG *)record->pv_tag[slots[i]];
        if (!tag || tag->read_only) return TRUE;
    }
    return FALSE;
}

BOOLEAN WinLvmCollectLvBackingSlotsLocked(const wl_vg_record *record, size_t lv_index,
                                          size_t *slots_out, size_t *slot_count_out,
                                          ULONG *max_stack_out, ULONG *max_logical_out,
                                          ULONG *max_physical_out) {
    *slot_count_out = 0;
    *max_stack_out = 0;
    *max_logical_out = 0;
    *max_physical_out = 0;

    const lvmcore_volume_group *view = record ? record->view : NULL;
    if (!view || !view->lvs || lv_index >= view->lv_count) return FALSE;
    const lvmcore_logical_volume *lv0 = &view->lvs[lv_index];
    if (lv0->segment_count == 0 || !lv0->segments) return FALSE;

    size_t count = 0;
    ULONG max_stack = 0, max_logical = 0, max_physical = 0;
    for (size_t s = 0; s < lv0->segment_count; s++) {
        const lvmcore_segment *seg = &lv0->segments[s];
        if (!seg->stripes || seg->stripe_count_used == 0) return FALSE;
        for (size_t t = 0; t < seg->stripe_count_used; t++) {
            const lvmcore_physical_volume *pv = lvmcore_vg_find_pv(view, seg->stripes[t].pv_key);
            if (!pv) return FALSE;
            size_t slot = SIZE_MAX;
            for (size_t k = 0; k < record->pv_count; k++) {
                if (RtlEqualMemory(record->pv_uuid[k], pv->id, 32)) { slot = k; break; }
            }
            if (slot == SIZE_MAX) return FALSE;

            size_t known = 0;
            for (; known < count; known++) if (slots_out[known] == slot) break;
            if (known == count) {
                const WINLVM_PV_TAG *tag = (const WINLVM_PV_TAG *)record->pv_tag[slot];
                if (!tag || !tag->backing_pdo || !tag->backing_file || count >= WL_VG_MAX_PVS) {
                    return FALSE;
                }
                slots_out[count++] = slot;
                if ((ULONG)tag->backing_pdo->StackSize > max_stack) {
                    max_stack = (ULONG)tag->backing_pdo->StackSize;
                }
                if (tag->logical_sector > max_logical) max_logical = tag->logical_sector;
                if (tag->physical_sector > max_physical) max_physical = tag->physical_sector;
            }
        }
    }
    if (count == 0) return FALSE;
    if (max_logical == 0) max_logical = 512;
    if (max_physical < max_logical) max_physical = max_logical;

    *slot_count_out = count;
    *max_stack_out = max_stack;
    *max_logical_out = max_logical;
    *max_physical_out = max_physical;
    return TRUE;
}

BOOLEAN WinLvmProbeWorkerCreateLvPdo(WINLVM_BUS_EXT *bus,
                                     wl_vg_record *record,
                                     size_t lv_index) {
    size_t slots[WL_VG_MAX_PVS];
    PDEVICE_OBJECT backing_pdo[WL_VG_MAX_PVS];
    PFILE_OBJECT backing_file[WL_VG_MAX_PVS];
    ULONG backing_logical[WL_VG_MAX_PVS];
    ULONG backing_physical[WL_VG_MAX_PVS];
    size_t slot_count = 0;
    ULONG max_stack = 0, max_logical = 512, max_physical = 512;
    uint64_t byte_size = 0;
    BOOLEAN read_only = FALSE;
    BOOLEAN ok = FALSE;
    CHAR lv_uuid[WL_VG_UUID_LEN];
    RtlZeroMemory(lv_uuid, sizeof(lv_uuid));

    KIRQL old;
    KeAcquireSpinLock(&bus->lock, &old);
    const lvmcore_volume_group *view = record ? record->view : NULL;
    if (view && view->lvs && lv_index < view->lv_count &&
        WinLvmCollectLvBackingSlotsLocked(record, lv_index, slots, &slot_count,
                                          &max_stack, &max_logical, &max_physical)) {
        RtlCopyMemory(lv_uuid, view->lvs[lv_index].id, 32);
        lv_uuid[32] = '\0';
        byte_size = wl_lv_byte_size(view, lv_index);
        read_only = WinLvmComputeLvReadOnly(record, lv_index, slots, slot_count);
        for (size_t i = 0; i < slot_count; i++) {
            const WINLVM_PV_TAG *tag = (const WINLVM_PV_TAG *)record->pv_tag[slots[i]];
            backing_pdo[i] = tag->backing_pdo;
            backing_file[i] = tag->backing_file;
            backing_logical[i] = tag->logical_sector;
            backing_physical[i] = tag->physical_sector;
            ObReferenceObject(tag->backing_pdo);
            ObReferenceObject(tag->backing_file);
        }
        ok = (byte_size > 0);
    }
    KeReleaseSpinLock(&bus->lock, old);

    if (!ok) {
        for (size_t i = 0; i < slot_count; i++) {
            ObDereferenceObject(backing_pdo[i]);
            ObDereferenceObject(backing_file[i]);
        }
        return FALSE;
    }

    WCHAR namebuf[64];
    RtlZeroMemory(namebuf, sizeof(namebuf));
    NTSTATUS status = RtlStringCchPrintfW(
        namebuf, sizeof(namebuf) / sizeof(WCHAR), L"\\Device\\WinLvm%hs", lv_uuid);
    if (!NT_SUCCESS(status)) goto fail_refs;

    UNICODE_STRING devName;
    RtlInitUnicodeString(&devName, namebuf);
    PDEVICE_OBJECT lv_pdo = NULL;
    status = IoCreateDevice(bus->fdo->DriverObject, sizeof(WINLVM_LV_EXT), &devName,
                            FILE_DEVICE_DISK, FILE_DEVICE_SECURE_OPEN, FALSE, &lv_pdo);
    if (!NT_SUCCESS(status)) goto fail_refs;

    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)lv_pdo->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));
    ext->kind = WL_EXT_LV;
    if (!NT_SUCCESS(WinLvmIoGateInit(&ext->io_gate))) {
        IoDeleteDevice(lv_pdo);
        goto fail_refs;
    }
    ext->io_gate.device = lv_pdo;
    RtlCopyMemory(ext->devname, namebuf, sizeof(namebuf));
    RtlCopyMemory(ext->lv_uuid, lv_uuid, WL_VG_UUID_LEN);
    ext->pdo = lv_pdo;
    ext->record = record;
    ext->lv_index = lv_index;
    ext->backing_count = slot_count;
    for (size_t i = 0; i < slot_count; i++) {
        ext->backing_pdo[i] = backing_pdo[i];
        ext->backing_file[i] = backing_file[i];
        ext->backing_logical[i] = backing_logical[i];
        ext->backing_physical[i] = backing_physical[i];
        ext->pv_slot[i] = (uint32_t)slots[i];
    }
    ext->logical_sector = max_logical;
    ext->physical_sector = max_physical;
    ext->byte_size = byte_size;
    ext->read_only = read_only;
    ext->drive_letter = 0;
    ext->system_power_state = PowerSystemWorking;
    ext->device_power_state = PowerDeviceD0;
    KeInitializeMutex(&ext->ifname_mutex, 0);
    ext->ifname_torn_down = FALSE;

    lv_pdo->SectorSize = (USHORT)ext->logical_sector;
    lv_pdo->StackSize = (UCHAR)(max_stack + 1);
    if (lv_pdo->StackSize < 1) lv_pdo->StackSize = 1;
    lv_pdo->Flags |= DO_DIRECT_IO;
    lv_pdo->Flags &= ~DO_DEVICE_INITIALIZING;

    KeAcquireSpinLock(&bus->lock, &old);
    if (bus->child_count < WL_MAX_CHILDREN) {
        bus->children[bus->child_count++] = lv_pdo;
        wl_vg_ref(record);
        size_t total = bus->child_count;
        KeReleaseSpinLock(&bus->lock, old);
        WL_TRACE_2("create lv pdo: SUCCESS child %wZ total %u", &devName, (unsigned)total);
        return TRUE;
    }
    KeReleaseSpinLock(&bus->lock, old);
    IoDeleteDevice(lv_pdo);

fail_refs:
    for (size_t i = 0; i < slot_count; i++) {
        ObDereferenceObject(backing_pdo[i]);
        ObDereferenceObject(backing_file[i]);
    }
    return FALSE;
}

static void WinLvmUnlinkLvPdoLocked(WINLVM_BUS_EXT *bus, PDEVICE_OBJECT LvPdo) {
    WINLVM_LV_EXT *lvext = (WINLVM_LV_EXT *)LvPdo->DeviceExtension;
    if (bus) {
        for (size_t i = 0; i < bus->child_count; i++) {
            if (bus->children[i] == LvPdo) {
                bus->children[i] = bus->children[bus->child_count - 1];
                bus->children[bus->child_count - 1] = NULL;
                bus->child_count--;
                break;
            }
        }
    }
    if (lvext->record) {
        wl_vg_lv_set_activated(lvext->record, lvext->lv_index, 0);
        wl_vg_unref(lvext->record);
        lvext->record = NULL;
    }
}

void WinLvmLvRegisterAndEnableInterface(PDEVICE_OBJECT LvPdo) {
    if (!LvPdo || KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    WINLVM_LV_EXT *lvext = (WINLVM_LV_EXT *)LvPdo->DeviceExtension;
    KeWaitForSingleObject(&lvext->ifname_mutex, Executive, KernelMode, FALSE, NULL);
    if (!lvext->ifname_torn_down) {
        if (!lvext->ifname.Buffer) {
            UNICODE_STRING ifname;
            NTSTATUS ifst = IoRegisterDeviceInterface(
                LvPdo, &GUID_DEVINTERFACE_VOLUME, NULL, &ifname);
            WL_TRACE_1("child start: reg ifname 0x%08x", (unsigned)ifst);
            if (NT_SUCCESS(ifst)) lvext->ifname = ifname;
        }
        if (lvext->ifname.Buffer) {
            NTSTATUS set_st = IoSetDeviceInterfaceState(&lvext->ifname, TRUE);
            WL_TRACE_1("child start: set ifstate 0x%08x", (unsigned)set_st);
        }
    }
    KeReleaseMutex(&lvext->ifname_mutex, FALSE);
}

void WinLvmLvSetInterfaceEnabled(PDEVICE_OBJECT LvPdo, BOOLEAN enable) {
    if (!LvPdo || KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    WINLVM_LV_EXT *lvext = (WINLVM_LV_EXT *)LvPdo->DeviceExtension;
    KeWaitForSingleObject(&lvext->ifname_mutex, Executive, KernelMode, FALSE, NULL);
    if (lvext->ifname.Buffer && (!enable || !lvext->ifname_torn_down)) {
        IoSetDeviceInterfaceState(&lvext->ifname, enable);
    }
    KeReleaseMutex(&lvext->ifname_mutex, FALSE);
}

void WinLvmLvDestroyInterface(PDEVICE_OBJECT LvPdo) {
    if (!LvPdo || KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    WINLVM_LV_EXT *lvext = (WINLVM_LV_EXT *)LvPdo->DeviceExtension;
    KeWaitForSingleObject(&lvext->ifname_mutex, Executive, KernelMode, FALSE, NULL);
    lvext->ifname_torn_down = TRUE;
    if (lvext->ifname.Buffer) {
        IoSetDeviceInterfaceState(&lvext->ifname, FALSE);
        RtlFreeUnicodeString(&lvext->ifname);
        RtlZeroMemory(&lvext->ifname, sizeof(lvext->ifname));
    }
    KeReleaseMutex(&lvext->ifname_mutex, FALSE);
}

static void WinLvmTeardownLvPdoResources(PDEVICE_OBJECT LvPdo) {
    WINLVM_LV_EXT *lvext = (WINLVM_LV_EXT *)LvPdo->DeviceExtension;
    PDEVICE_OBJECT drop_pdos[WL_VG_MAX_PVS];
    PFILE_OBJECT drop_files[WL_VG_MAX_PVS];
    size_t drop_count = 0;

    WinLvmWmiDeregister(LvPdo);
    WinLvmLvDestroyInterface(LvPdo);

    WinLvmIoGateRemove(&lvext->io_gate);

    if (g_bus_ext) {
        KIRQL old;
        KeAcquireSpinLock(&g_bus_ext->lock, &old);
        WinLvmUnlinkLvPdoLocked(g_bus_ext, LvPdo);
        WinLvmDetachChildBackingLocked(lvext, drop_pdos, drop_files, &drop_count);
        lvext->drive_letter = 0;
        KeReleaseSpinLock(&g_bus_ext->lock, old);
    } else {
        WinLvmUnlinkLvPdoLocked(NULL, LvPdo);
        WinLvmDetachChildBackingLocked(lvext, drop_pdos, drop_files, &drop_count);
        lvext->drive_letter = 0;
    }

    for (size_t i = 0; i < drop_count; i++) {
        if (drop_pdos[i]) ObDereferenceObject(drop_pdos[i]);
        if (drop_files[i]) ObDereferenceObject(drop_files[i]);
    }
}

void WinLvmFinishProbeActivation(WINLVM_BUS_EXT *bus, size_t slot,
                                 const size_t *newly, size_t newly_count,
                                 PDEVICE_OBJECT *revived, size_t revived_count,
                                 BOOLEAN force_invalidate) {
    KIRQL old;
    BOOLEAN any_rebound = FALSE;
    for (size_t i = 0; i < revived_count; i++) {
        if (WinLvmRebindChildPdo(bus, revived[i])) any_rebound = TRUE;
        ObDereferenceObject(revived[i]);
    }
    for (size_t i = 0; i < newly_count; i++) {
        wl_vg_record *r = &bus->vg_records[slot];
        if (!WinLvmProbeWorkerCreateLvPdo(bus, r, newly[i])) {
            KeAcquireSpinLock(&bus->lock, &old);
            BOOLEAN has_child = FALSE;
            if (r->view && r->view->lvs && newly[i] < r->view->lv_count) {
                const char *id = r->view->lvs[newly[i]].id;
                for (size_t c = 0; c < bus->child_count; c++) {
                    WINLVM_LV_EXT *ce = (WINLVM_LV_EXT *)bus->children[c]->DeviceExtension;
                    if (RtlEqualMemory(ce->lv_uuid, id, 32)) {
                        has_child = TRUE;
                        break;
                    }
                }
            }
            if (!has_child) wl_vg_lv_set_activated(r, newly[i], 0);
            KeReleaseSpinLock(&bus->lock, old);
        }
    }
    if (newly_count > 0 || any_rebound || force_invalidate) {
        IoInvalidateDeviceRelations(bus->pdo, BusRelations);
    }
}

void WinLvmDeleteLvPdo(PDEVICE_OBJECT LvPdo) {
    if (!LvPdo) return;
    WinLvmTeardownLvPdoResources(LvPdo);
    IoDeleteDevice(LvPdo);
}

NTSTATUS WinLvmChildPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    if (kind != WL_EXT_LV) {
        NTSTATUS st = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return st;
    }

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    WINLVM_LV_EXT *lvext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    WL_TRACE_1("child pnp minor 0x%x", (unsigned)irpSp->MinorFunction);

    switch (irpSp->MinorFunction) {
    case IRP_MN_QUERY_ID: {
        BUS_QUERY_ID_TYPE type = irpSp->Parameters.QueryId.IdType;

        if (type == BusQueryHardwareIDs) {
            PWCHAR ids = (PWCHAR)ExAllocatePoolZero(
                PagedPool, sizeof(WinLvmHwId), WINLVM_WIN_TAG);
            if (!ids) {
                Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                Irp->IoStatus.Information = 0;
                break;
            }
            RtlCopyMemory(ids, WinLvmHwId, sizeof(WinLvmHwId));
            Irp->IoStatus.Information = (ULONG_PTR)ids;
            Irp->IoStatus.Status = STATUS_SUCCESS;
        } else if (type == BusQueryDeviceID) {
            PWCHAR id = (PWCHAR)ExAllocatePoolZero(
                PagedPool, sizeof(WinLvmDeviceId), WINLVM_WIN_TAG);
            if (!id) {
                Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                Irp->IoStatus.Information = 0;
                break;
            }
            RtlCopyMemory(id, WinLvmDeviceId, sizeof(WinLvmDeviceId));
            Irp->IoStatus.Information = (ULONG_PTR)id;
            Irp->IoStatus.Status = STATUS_SUCCESS;
        } else if (type == BusQueryInstanceID) {
            if (lvext->lv_uuid[0] == '\0') break;
            WCHAR inst[40];
            size_t i = 0;
            for (; i < 32 && lvext->lv_uuid[i]; i++) {
                inst[i] = (WCHAR)(unsigned char)lvext->lv_uuid[i];
            }
            inst[i] = 0;
            PWCHAR id = (PWCHAR)ExAllocatePoolZero(
                PagedPool, (i + 2) * sizeof(WCHAR), WINLVM_WIN_TAG);
            if (!id) {
                Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                Irp->IoStatus.Information = 0;
                break;
            }
            RtlCopyMemory(id, inst, (i + 1) * sizeof(WCHAR));
            Irp->IoStatus.Information = (ULONG_PTR)id;
            Irp->IoStatus.Status = STATUS_SUCCESS;
        }
        break;
    }

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (irpSp->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation) {
            PDEVICE_RELATIONS rel = (PDEVICE_RELATIONS)ExAllocatePoolZero(
                PagedPool, sizeof(DEVICE_RELATIONS), WINLVM_WIN_TAG);
            if (!rel) {
                Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                Irp->IoStatus.Information = 0;
            } else {
                rel->Count = 1;
                rel->Objects[0] = DeviceObject;
                ObReferenceObject(DeviceObject);
                Irp->IoStatus.Information = (ULONG_PTR)rel;
                Irp->IoStatus.Status = STATUS_SUCCESS;
            }
        }
        break;

    case IRP_MN_QUERY_CAPABILITIES: {
        PDEVICE_CAPABILITIES caps = irpSp->Parameters.DeviceCapabilities.Capabilities;
        if (caps) {
            caps->RawDeviceOK = TRUE;
            caps->SurpriseRemovalOK = TRUE;
            caps->DeviceState[PowerSystemWorking] = PowerDeviceD0;
            caps->DeviceState[PowerSystemSleeping1] = PowerDeviceD3;
            caps->DeviceState[PowerSystemSleeping2] = PowerDeviceD3;
            caps->DeviceState[PowerSystemSleeping3] = PowerDeviceD3;
            caps->DeviceState[PowerSystemHibernate] = PowerDeviceD3;
            caps->DeviceState[PowerSystemShutdown] = PowerDeviceD3;
            Irp->IoStatus.Status = STATUS_SUCCESS;
        }
        break;
    }

    case IRP_MN_START_DEVICE:
        WinLvmLvRegisterAndEnableInterface(DeviceObject);
        WinLvmWmiInit(DeviceObject);
        WinLvmQueueMountMgrArrival(DeviceObject);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_STOP_DEVICE:
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_REMOVE_DEVICE: {
        WinLvmWmiDeregister(DeviceObject);
        WinLvmLvSetInterfaceEnabled(DeviceObject, FALSE);

        BOOLEAN physically_removed = TRUE;
        size_t rec_slot = SIZE_MAX;
        if (g_bus_ext) {
            KIRQL old;
            KeAcquireSpinLock(&g_bus_ext->lock, &old);
            if (lvext->record && !lvext->record->dead &&
                !lvext->unavailable && !lvext->deactivating &&
                wl_vg_lv_activated(lvext->record, lvext->lv_index) &&
                wl_vg_lv_pv_complete(lvext->record, lvext->lv_index)) {
                physically_removed = FALSE;
            } else {
                if (lvext->record >= g_bus_ext->vg_records &&
                    lvext->record < g_bus_ext->vg_records + WL_VG_MAX) {
                    rec_slot = (size_t)(lvext->record - g_bus_ext->vg_records);
                }
                /* Atomically unlink before dropping bus->lock so concurrent
                 * activation or QDR cannot resurrect a PDO under deletion. */
                WinLvmUnlinkLvPdoLocked(g_bus_ext, DeviceObject);
            }
            KeReleaseSpinLock(&g_bus_ext->lock, old);
        }
        if (!physically_removed) {
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;
        }

        WinLvmTeardownLvPdoResources(DeviceObject);
        if (g_bus_ext && rec_slot < WL_VG_MAX) {
            WinLvmBusResetRecordIfInactive(g_bus_ext, rec_slot);
        }
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoDeleteDevice(DeviceObject);
        return STATUS_SUCCESS;
    }

    case IRP_MN_SURPRISE_REMOVAL:
        WinLvmLvSetInterfaceEnabled(DeviceObject, FALSE);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    default:
        break;
    }

    NTSTATUS st = Irp->IoStatus.Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}
