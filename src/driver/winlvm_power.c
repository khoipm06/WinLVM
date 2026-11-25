/*
 * winlvm_power.c — WDM power IRP dispatch for bus FDO, child LV PDOs,
 *                  and the control device object.
 *
 * Dual-compiled for Windows kernel mode (_KERNEL_MODE) and host unit tests
 * via host_wdm_types.h.
 */
#ifdef _KERNEL_MODE
#include "winlvm.h"
#else
#include "host_wdm_types.h"
#endif

static BOOLEAN WinLvmIsValidSystemPowerState(SYSTEM_POWER_STATE s) {
    return (s == PowerSystemWorking ||
            s == PowerSystemSleeping1 ||
            s == PowerSystemSleeping2 ||
            s == PowerSystemSleeping3 ||
            s == PowerSystemHibernate ||
            s == PowerSystemShutdown) ? TRUE : FALSE;
}

static BOOLEAN WinLvmIsValidDevicePowerState(DEVICE_POWER_STATE d) {
    return (d == PowerDeviceD0 ||
            d == PowerDeviceD1 ||
            d == PowerDeviceD2 ||
            d == PowerDeviceD3) ? TRUE : FALSE;
}

static void WinLvmRecordPowerTransition(PDEVICE_OBJECT DeviceObject,
                                        POWER_STATE_TYPE type,
                                        POWER_STATE state,
                                        SYSTEM_POWER_STATE *sys_slot,
                                        DEVICE_POWER_STATE *dev_slot) {
    /* WDK PoSetPowerState IRQL contract: only (DevicePowerState, PowerDeviceD0)
     * may be called at DISPATCH_LEVEL; all other states require <= APC_LEVEL. */
    if (type == SystemPowerState) {
        if (WinLvmIsValidSystemPowerState(state.SystemState)) {
            if (sys_slot) *sys_slot = state.SystemState;
            if (KeGetCurrentIrql() <= APC_LEVEL) {
                PoSetPowerState(DeviceObject, SystemPowerState, state);
            }
        }
    } else if (type == DevicePowerState) {
        if (WinLvmIsValidDevicePowerState(state.DeviceState)) {
            if (dev_slot) *dev_slot = state.DeviceState;
            if (state.DeviceState == PowerDeviceD0 || KeGetCurrentIrql() <= APC_LEVEL) {
                PoSetPowerState(DeviceObject, DevicePowerState, state);
            }
        }
    }
}

NTSTATUS WinLvmPower(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WL_EXT_KIND kind = *(WL_EXT_KIND *)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    UCHAR minor = irpSp->MinorFunction;

    if (kind == WL_EXT_BUS) {
        WINLVM_BUS_EXT *ext = (WINLVM_BUS_EXT *)DeviceObject->DeviceExtension;
        PDEVICE_OBJECT lower = ext->bus_pdo;
        switch (minor) {
        case IRP_MN_SET_POWER:
            WinLvmRecordPowerTransition(DeviceObject,
                                        irpSp->Parameters.Power.Type,
                                        irpSp->Parameters.Power.State,
                                        &ext->system_power_state,
                                        &ext->device_power_state);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        case IRP_MN_QUERY_POWER:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        case IRP_MN_WAIT_WAKE:
        case IRP_MN_POWER_SEQUENCE:
        default:
            /* Preserve incoming Irp->IoStatus.Status on unhandled Power minors. */
            break;
        }
        PoStartNextPowerIrp(Irp);
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(lower, Irp);
    }

    if (kind == WL_EXT_LV) {
        WINLVM_LV_EXT *lv = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
        NTSTATUS status;
        switch (minor) {
        case IRP_MN_SET_POWER:
            WinLvmRecordPowerTransition(DeviceObject,
                                        irpSp->Parameters.Power.Type,
                                        irpSp->Parameters.Power.State,
                                        &lv->system_power_state,
                                        &lv->device_power_state);
            status = STATUS_SUCCESS;
            Irp->IoStatus.Status = status;
            Irp->IoStatus.Information = 0;
            break;

        case IRP_MN_QUERY_POWER:
            status = STATUS_SUCCESS;
            Irp->IoStatus.Status = status;
            Irp->IoStatus.Information = 0;
            break;

        case IRP_MN_WAIT_WAKE:
        case IRP_MN_POWER_SEQUENCE:
        default:
            /* PDO must preserve incoming Irp->IoStatus.Status (e.g. STATUS_NOT_SUPPORTED). */
            status = Irp->IoStatus.Status;
            break;
        }
        PoStartNextPowerIrp(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    if (kind == WL_EXT_CONTROL) {
        NTSTATUS status;
        if (minor == IRP_MN_SET_POWER) {
            WinLvmRecordPowerTransition(DeviceObject,
                                        irpSp->Parameters.Power.Type,
                                        irpSp->Parameters.Power.State,
                                        NULL, NULL);
            status = STATUS_SUCCESS;
            Irp->IoStatus.Status = status;
            Irp->IoStatus.Information = 0;
        } else if (minor == IRP_MN_QUERY_POWER) {
            status = STATUS_SUCCESS;
            Irp->IoStatus.Status = status;
            Irp->IoStatus.Information = 0;
        } else {
            status = Irp->IoStatus.Status;
        }
        PoStartNextPowerIrp(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    NTSTATUS status = Irp->IoStatus.Status;
    PoStartNextPowerIrp(Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
