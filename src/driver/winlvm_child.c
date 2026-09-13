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
            winlvm_logical_volume *lv = &lvext->record->vg.lvs[lvext->lv_index];
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
            if (NT_SUCCESS(ifst)) lvext->ifname = ifname;
        }
        if (lvext->ifname.Buffer) {
            IoSetDeviceInterfaceState(&lvext->ifname, TRUE);
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
        /* PnP removes a child directly: deactivate interface, drop self
         * from the bus children array (else next QDR reports a freed PDO),
         * release refs, delete self. */
        IoWMIRegistrationControl(DeviceObject, WMIREG_ACTION_DEREGISTER);
        if (lvext->ifname.Buffer) {
            IoSetDeviceInterfaceState(&lvext->ifname, FALSE);
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
            /* Surprise-removal teardown: last child of a dead record
             * releases the record (backing PDO deref + ifname + struct).
             * In-flight IRPs already saw dead=1 and fail closed. */
            WINLVM_VG_RECORD *rec = lvext->record;
            if (rec && rec->dead && g_bus_ext->child_count == 0 &&
                g_bus_ext->vgs == rec) {
                g_bus_ext->vgs = NULL;
            } else {
                rec = NULL;
            }
            KeReleaseSpinLock(&g_bus_ext->lock, old);
            if (rec) {
                if (rec->disk_ifname.Buffer)
                    ExFreePoolWithTag(rec->disk_ifname.Buffer, WINLVM_WIN_TAG);
                ObDereferenceObject(rec->backing_pdo);
                ExFreePoolWithTag(rec, WINLVM_WIN_TAG);
            }
        }
        ObDereferenceObject(lvext->backing_pdo);
        IoDeleteDevice(DeviceObject);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    }
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
