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

static void WinLvmTeardownLvPdoResources(PDEVICE_OBJECT LvPdo) {
    WINLVM_LV_EXT *lvext = (WINLVM_LV_EXT *)LvPdo->DeviceExtension;

    WinLvmWmiDeregister(LvPdo);
    if (lvext->ifname.Buffer) {
        IoSetDeviceInterfaceState(&lvext->ifname, FALSE);
        RtlFreeUnicodeString(&lvext->ifname);
        RtlZeroMemory(&lvext->ifname, sizeof(lvext->ifname));
    }

    WinLvmIoGateRemove(&lvext->io_gate);

    if (g_bus_ext) {
        KIRQL old;
        KeAcquireSpinLock(&g_bus_ext->lock, &old);
        WinLvmUnlinkLvPdoLocked(g_bus_ext, LvPdo);
        KeReleaseSpinLock(&g_bus_ext->lock, old);
    } else if (lvext->record) {
        WinLvmUnlinkLvPdoLocked(NULL, LvPdo);
    }

    for (size_t i = 0; i < lvext->backing_count && i < WL_VG_MAX_PVS; i++) {
        if (lvext->backing_pdo[i]) {
            ObDereferenceObject(lvext->backing_pdo[i]);
            lvext->backing_pdo[i] = NULL;
        }
        if (lvext->backing_file[i]) {
            ObDereferenceObject(lvext->backing_file[i]);
            lvext->backing_file[i] = NULL;
        }
    }
    lvext->backing_count = 0;
}

void WinLvmDeleteLvPdo(PDEVICE_OBJECT LvPdo) {
    if (!LvPdo) return;
    WinLvmTeardownLvPdoResources(LvPdo);
    IoDeleteDevice(LvPdo);
}

NTSTATUS WinLvmChildPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    if (kind != WL_EXT_LV) {
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
    }

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    WINLVM_LV_EXT *lvext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    WL_TRACE_1("child pnp minor 0x%x", (unsigned)irpSp->MinorFunction);

    switch (irpSp->MinorFunction) {
    case IRP_MN_QUERY_ID: {
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        BUS_QUERY_ID_TYPE type = irpSp->Parameters.QueryId.IdType;

        if (type == BusQueryHardwareIDs) {
            PWCHAR ids = (PWCHAR)ExAllocatePoolZero(
                NonPagedPool, sizeof(WinLvmHwId), WINLVM_WIN_TAG);
            if (!ids) break;
            RtlCopyMemory(ids, WinLvmHwId, sizeof(WinLvmHwId));
            Irp->IoStatus.Information = (ULONG_PTR)ids;
            Irp->IoStatus.Status = STATUS_SUCCESS;
        } else if (type == BusQueryDeviceID) {
            PWCHAR id = (PWCHAR)ExAllocatePoolZero(
                NonPagedPool, sizeof(WinLvmDeviceId), WINLVM_WIN_TAG);
            if (!id) break;
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
                NonPagedPool, (i + 2) * sizeof(WCHAR), WINLVM_WIN_TAG);
            if (!id) break;
            RtlCopyMemory(id, inst, (i + 1) * sizeof(WCHAR));
            Irp->IoStatus.Information = (ULONG_PTR)id;
            Irp->IoStatus.Status = STATUS_SUCCESS;
        }
        break;
    }

    case IRP_MN_QUERY_CAPABILITIES: {
        PDEVICE_CAPABILITIES caps = irpSp->Parameters.DeviceCapabilities.Capabilities;
        caps->RawDeviceOK = TRUE;
        caps->SurpriseRemovalOK = TRUE;
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    }

    case IRP_MN_QUERY_RESOURCES:
    case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_START_DEVICE:
        if (!lvext->ifname.Buffer) {
            UNICODE_STRING ifname;
            NTSTATUS ifst = IoRegisterDeviceInterface(
                DeviceObject, &GUID_DEVINTERFACE_VOLUME, NULL, &ifname);
            WL_TRACE_1("child start: reg ifname 0x%08x", (unsigned)ifst);
            if (NT_SUCCESS(ifst)) lvext->ifname = ifname;
        }
        if (lvext->ifname.Buffer) {
            NTSTATUS set_st = IoSetDeviceInterfaceState(&lvext->ifname, TRUE);
            WL_TRACE_1("child start: set ifstate 0x%08x", (unsigned)set_st);
        }
        WinLvmWmiInit(DeviceObject);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_REMOVE_DEVICE: {
        WinLvmWmiDeregister(DeviceObject);
        if (lvext->ifname.Buffer) IoSetDeviceInterfaceState(&lvext->ifname, FALSE);

        BOOLEAN physically_removed = TRUE;
        if (g_bus_ext) {
            KIRQL old;
            KeAcquireSpinLock(&g_bus_ext->lock, &old);
            if (lvext->record && !lvext->record->dead &&
                !lvext->unavailable && !lvext->deactivating &&
                wl_vg_lv_activated(lvext->record, lvext->lv_index) &&
                wl_vg_lv_pv_complete(lvext->record, lvext->lv_index)) {
                physically_removed = FALSE;
            } else {
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
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoDeleteDevice(DeviceObject);
        return STATUS_SUCCESS;
    }

    case IRP_MN_SURPRISE_REMOVAL:
        if (lvext->ifname.Buffer) IoSetDeviceInterfaceState(&lvext->ifname, FALSE);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    default:
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        break;
    }

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}
