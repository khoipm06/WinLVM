/* winlvm_mount.c */
#include "winlvm.h"

NTSTATUS WinLvmMountIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp, WINLVM_LV_EXT *ext) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;

    if (code == IOCTL_MOUNTDEV_QUERY_DEVICE_NAME) {
        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUNTDEV_NAME)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
            return STATUS_BUFFER_OVERFLOW;
        }

        ULONG nameLen = ext->vg->volume_ifname.Length;
        ULONG reqSize = sizeof(MOUNTDEV_NAME) + nameLen;

        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength < reqSize) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
            return STATUS_BUFFER_OVERFLOW;
        }

        PMOUNTDEV_NAME mountName = (PMOUNTDEV_NAME)Irp->AssociatedIrp.SystemBuffer;
        mountName->NameLength = (USHORT)nameLen;
        RtlCopyMemory(mountName->Name, ext->vg->volume_ifname.Buffer, nameLen);
        Irp->IoStatus.Information = reqSize;
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_MOUNTDEV_QUERY_UNIQUE_ID) {
        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUNTDEV_UNIQUE_ID)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
            return STATUS_BUFFER_OVERFLOW;
        }

        winlvm_logical_volume *lv = &ext->vg->vg.lvs[ext->lv_index];
        ULONG idLen = 32; /* raw UUID */
        ULONG reqSize = sizeof(MOUNTDEV_UNIQUE_ID) + idLen;

        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength < reqSize) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
            return STATUS_BUFFER_OVERFLOW;
        }

        PMOUNTDEV_UNIQUE_ID uniqueId = (PMOUNTDEV_UNIQUE_ID)Irp->AssociatedIrp.SystemBuffer;
        uniqueId->UniqueIdLength = (USHORT)idLen;
        RtlCopyMemory(uniqueId->UniqueId, lv->id, idLen);
        Irp->IoStatus.Information = reqSize;
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME) {
        return STATUS_NOT_IMPLEMENTED;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}
