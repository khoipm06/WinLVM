/* winlvm_child.c — winlvm.sys LV PDO PnP dispatch: QUERY_ID,
 * QUERY_CAPABILITIES, START (volume-interface registration), REMOVE
 * (self-removal from the bus children array).
 *
 * Moved verbatim from winlvm.c (2026-09-12 module split) behind
 * WinLvmChildPnp; bodies are byte-identical.
 */
#include "winlvm.h"

NTSTATUS WinLvmChildPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    WINLVM_LV_EXT *lvext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    WL_TRACE_1("child pnp minor 0x%x", (unsigned)irpSp->MinorFunction);
    switch (irpSp->MinorFunction) {
    case IRP_MN_QUERY_ID: {
        /* Verifier 0x20E: every PnP IRP must be initialized to
         * STATUS_NOT_SUPPORTED before dispatch logic. Alloc-failure breaks
         * and unhandled IdTypes (CompatibleIDs, ContainerID, LegacyBusType)
         * would otherwise complete with the I/O manager's sentinel status. */
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = 0;
        BUS_QUERY_ID_TYPE type = irpSp->Parameters.QueryId.IdType;
        WL_TRACE_1("child query id type %d", (int)type);
        if (type == BusQueryHardwareIDs) {
            /* must match the INF's WinLvmVolume model entry */
            PWCHAR ids = (PWCHAR)ExAllocatePoolZero(NonPagedPool,
                sizeof(WinLvmHwId), WINLVM_WIN_TAG);
            if (!ids) break;
            RtlCopyMemory(ids, WinLvmHwId, sizeof(WinLvmHwId));
            Irp->IoStatus.Information = (ULONG_PTR)ids;
            Irp->IoStatus.Status = STATUS_SUCCESS;
        } else if (type == BusQueryDeviceID) {
            /* WDM contract: DeviceID MUST have <enumerator>\<device-id> form */
            PWCHAR id = (PWCHAR)ExAllocatePoolZero(NonPagedPool,
                sizeof(WinLvmDeviceId), WINLVM_WIN_TAG);
            if (!id) break;
            RtlCopyMemory(id, WinLvmDeviceId, sizeof(WinLvmDeviceId));
            Irp->IoStatus.Information = (ULONG_PTR)id;
            Irp->IoStatus.Status = STATUS_SUCCESS;
        } else if (type == BusQueryInstanceID) {
            const lvmcore_volume_group *view = lvext->record->view;
            if (!view || lvext->lv_index >= view->lv_count) break;
            const lvmcore_logical_volume *lv = &view->lvs[lvext->lv_index];
            WCHAR inst[40];
            /* instance = LV UUID, guaranteed unique per device */
            size_t i = 0;
            for (; i < 32 && lv->id[i]; i++) inst[i] = (WCHAR)lv->id[i];
            inst[i] = 0;
            PWCHAR id = (PWCHAR)ExAllocatePoolZero(NonPagedPool,
                (i + 2) * sizeof(WCHAR), WINLVM_WIN_TAG);
            if (!id) break;
            RtlCopyMemory(id, inst, (i + 1) * sizeof(WCHAR));
            Irp->IoStatus.Information = (ULONG_PTR)id;
            Irp->IoStatus.Status = STATUS_SUCCESS;
        }
        break;
    }
    case IRP_MN_QUERY_CAPABILITIES: {
        /* Raw-PDO contract (Oracle-verified): with no function driver the
         * child devnode only starts if the PDO marks itself raw-capable. */
        WL_TRACE("child query caps");
        PDEVICE_CAPABILITIES caps = irpSp->Parameters.DeviceCapabilities.Capabilities;
        caps->RawDeviceOK = TRUE;
        caps->SurpriseRemovalOK = TRUE;
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_RESOURCES:
    case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
        /* No hardware resources behind an LV; PnP requires the explicit
         * success-with-NULL or the raw devnode never starts. */
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    case IRP_MN_START_DEVICE:
        WL_TRACE("child START");
        /* The devnode exists by the time START arrives: register the volume
         * interface NOW (create-time registration fails — no instance path
         * yet), then activate so mountmgr begins its queries. */
        if (!lvext->ifname.Buffer) {
            UNICODE_STRING ifname;
            NTSTATUS ifst = IoRegisterDeviceInterface(DeviceObject,
                &GUID_DEVINTERFACE_VOLUME, NULL, &ifname);
            WL_TRACE_1("child start: reg ifname 0x%08x", (unsigned)ifst);
            if (NT_SUCCESS(ifst)) lvext->ifname = ifname;
        }
        if (lvext->ifname.Buffer) {
            NTSTATUS set_st = IoSetDeviceInterfaceState(&lvext->ifname, TRUE);
            WL_TRACE_1("child start: set ifstate 0x%08x", (unsigned)set_st);
        }
        /* Oracle H5: zero-GUID wmilib provider per LV PDO — the verifier
         * probe's ProviderId IS this self-started PDO; registration makes
         * this stack the legitimate completer, ending the 0x220 cycle. */
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
        /* WDK contract for bus driver child PDO removal:
         * If the device is still physically present on the bus (record != NULL && !record->dead),
         * the bus driver must NOT call IoDeleteDevice! It retains the PDO in its children list,
         * deactivates the interface, and completes with STATUS_SUCCESS.
         * Only when physically removed (record == NULL || record->dead) does it delete the PDO. */
        IoWMIRegistrationControl(DeviceObject, WMIREG_ACTION_DEREGISTER);
        if (lvext->ifname.Buffer) {
            IoSetDeviceInterfaceState(&lvext->ifname, FALSE);
        }

        BOOLEAN physically_removed = (lvext->record == NULL || lvext->record->dead);
        if (!physically_removed) {
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;
        }

        WinLvmIoGateRemove(&lvext->io_gate);
        if (lvext->ifname.Buffer) {
            RtlFreeUnicodeString(&lvext->ifname);
        }
        if (g_bus_ext) {
            KIRQL old;
            KeAcquireSpinLock(&g_bus_ext->lock, &old);
            for (size_t i = 0; i < g_bus_ext->child_count; i++) {
                if (g_bus_ext->children[i] == DeviceObject) {
                    g_bus_ext->children[i] =
                        g_bus_ext->children[g_bus_ext->child_count - 1];
                    g_bus_ext->child_count--;
                    break;
                }
            }
            if (lvext->record) {
                wl_vg_lv_set_activated(lvext->record, lvext->lv_index, 0);
                wl_vg_unref(lvext->record);
                lvext->record = NULL;
            }
            KeReleaseSpinLock(&g_bus_ext->lock, old);
        }
        /* Balance the per-PV backing refs from CreateLvPdo. */
        for (size_t i = 0; i < lvext->backing_count; i++) {
            if (lvext->backing_pdo[i]) ObDereferenceObject(lvext->backing_pdo[i]);
            if (lvext->backing_file[i]) ObDereferenceObject(lvext->backing_file[i]);
            lvext->backing_pdo[i] = NULL;
            lvext->backing_file[i] = NULL;
        }
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoDeleteDevice(DeviceObject);
        return STATUS_SUCCESS;
    }
    case IRP_MN_SURPRISE_REMOVAL:
        /* WDM contract (irp-mn-surprise-removal.md): all PnP drivers MUST
         * answer this IRP with STATUS_SUCCESS — "a driver must not fail
         * this IRP". The old default (STATUS_NOT_SUPPORTED) stalled the
         * surprise-removal processing of the mounted volume stack after a
         * backing-disk yank. Teardown stays in REMOVE_DEVICE; SURPRISE only
         * acknowledges (PnpSurpriseRemove rule: no IoDeleteDevice until
         * the surprise IRP succeeds and all open handles close). */
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    default:
        /* Raw-PDO contract: unknown PnP requests (QUERY_INTERFACE,
         * QUERY_DEVICE_TEXT, ...) must NOT complete with success —
         * STATUS_SUCCESS on IRP_MN_QUERY_INTERFACE makes the querying
         * driver call through its uninitialized output struct
         * (privileged-instruction BSOD 0x7E/c0000096, 2026-09-11). */
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        break;
    }

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}
