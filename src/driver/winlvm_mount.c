/*
 * winlvm_mount.c — Mount Manager IOCTLs and volume FSCTLs for LV PDOs.
 *
 * Dual-compiled for Windows kernel mode (_KERNEL_MODE) and host unit tests
 * via host_wdm_types.h.
 */
#ifdef _KERNEL_MODE
#include <stddef.h>
#include "winlvm.h"
#else
#include "host_wdm_types.h"
#endif

#ifndef IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME
#define IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME CTL_CODE(MOUNTDEV_IOCTL, 3, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef IOCTL_MOUNTDEV_QUERY_STABLE_GUID
#define IOCTL_MOUNTDEV_QUERY_STABLE_GUID CTL_CODE(MOUNTDEV_IOCTL, 5, METHOD_BUFFERED, FILE_ANY_ACCESS)
typedef struct _MOUNTDEV_STABLE_GUID {
    GUID StableGuid;
} MOUNTDEV_STABLE_GUID, *PMOUNTDEV_STABLE_GUID;
#endif

static void WinLvmUuidToGuid(const char *uuid, GUID *guid) {
    unsigned char bytes[16];
    BOOLEAN is_hex = TRUE;
    RtlZeroMemory(guid, sizeof(GUID));
    for (int i = 0; i < 16; i++) {
        char c1 = uuid[i * 2];
        char c2 = uuid[i * 2 + 1];
        int v1 = -1, v2 = -1;
        if (c1 >= '0' && c1 <= '9') v1 = c1 - '0';
        else if (c1 >= 'a' && c1 <= 'f') v1 = c1 - 'a' + 10;
        else if (c1 >= 'A' && c1 <= 'F') v1 = c1 - 'A' + 10;
        if (c2 >= '0' && c2 <= '9') v2 = c2 - '0';
        else if (c2 >= 'a' && c2 <= 'f') v2 = c2 - 'a' + 10;
        else if (c2 >= 'A' && c2 <= 'F') v2 = c2 - 'A' + 10;
        if (v1 < 0 || v2 < 0) {
            is_hex = FALSE;
            break;
        }
        bytes[i] = (unsigned char)((v1 << 4) | v2);
    }
    if (is_hex) {
        RtlCopyMemory(guid, bytes, sizeof(GUID));
    } else {
        RtlCopyMemory(guid, uuid, sizeof(GUID));
    }
}

static const char *WinLvmMountGetLvUuid(const WINLVM_LV_EXT *ext) {
#ifdef _KERNEL_MODE
    return (ext && ext->lv_uuid[0] != '\0') ? ext->lv_uuid : NULL;
#else
    const lvmcore_volume_group *view = (ext && ext->record) ? ext->record->view : NULL;
    if (!view || !view->lvs || ext->lv_index >= view->lv_count) return NULL;
    return view->lvs[ext->lv_index].id;
#endif
}

NTSTATUS WinLvmMountIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp, WINLVM_LV_EXT *ext) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;
    ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID sysBuf = Irp->AssociatedIrp.SystemBuffer;
    UNREFERENCED_PARAMETER(DeviceObject);

    if (code == IOCTL_MOUNTDEV_QUERY_DEVICE_NAME) {
        if (outLen < sizeof(MOUNTDEV_NAME)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;

        PMOUNTDEV_NAME mountName = (PMOUNTDEV_NAME)sysBuf;
        UNICODE_STRING devName;
        RtlInitUnicodeString(&devName, ext->devname);
        mountName->NameLength = devName.Length;

        if (outLen < (ULONG)(offsetof(MOUNTDEV_NAME, Name) + devName.Length)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
            return STATUS_BUFFER_OVERFLOW;
        }

        RtlCopyMemory(mountName->Name, devName.Buffer, devName.Length);
        Irp->IoStatus.Information = offsetof(MOUNTDEV_NAME, Name) + devName.Length;
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_MOUNTDEV_QUERY_UNIQUE_ID) {
        if (outLen < sizeof(MOUNTDEV_UNIQUE_ID)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;

        const char *lv_uuid = WinLvmMountGetLvUuid(ext);
        if (!lv_uuid) {
            Irp->IoStatus.Information = 0;
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        PMOUNTDEV_UNIQUE_ID uniqueId = (PMOUNTDEV_UNIQUE_ID)sysBuf;
        uniqueId->UniqueIdLength = 32;

        if (outLen < (ULONG)(offsetof(MOUNTDEV_UNIQUE_ID, UniqueId) + 32)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
            return STATUS_BUFFER_OVERFLOW;
        }

        RtlCopyMemory(uniqueId->UniqueId, lv_uuid, 32);
        Irp->IoStatus.Information = offsetof(MOUNTDEV_UNIQUE_ID, UniqueId) + 32;
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME) {
        Irp->IoStatus.Information = 0;
        return STATUS_NOT_IMPLEMENTED;
    }

    if (code == IOCTL_MOUNTDEV_QUERY_STABLE_GUID) {
        if (outLen < sizeof(MOUNTDEV_STABLE_GUID)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_STABLE_GUID);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;

        const char *lv_uuid = WinLvmMountGetLvUuid(ext);
        if (!lv_uuid) {
            Irp->IoStatus.Information = 0;
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        PMOUNTDEV_STABLE_GUID mdsg = (PMOUNTDEV_STABLE_GUID)sysBuf;
        WinLvmUuidToGuid(lv_uuid, &mdsg->StableGuid);
        Irp->IoStatus.Information = sizeof(MOUNTDEV_STABLE_GUID);
        return STATUS_SUCCESS;
    }

    if (code == FSCTL_DISMOUNT_VOLUME || code == FSCTL_LOCK_VOLUME ||
        code == FSCTL_UNLOCK_VOLUME || code == FSCTL_ALLOW_EXTENDED_DASD_IO) {
        Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}
