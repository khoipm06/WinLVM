/* winlvm_mount.c — Mount Manager + storage IOCTL surface for LV PDOs.
 *
 * Modeled on winmd's deliberately-tiny surface (mountmgr.c): the two
 * MOUNTDEV queries plus the storage geometry/length queries partmgr and
 * mountmgr issue before they will treat the device as a volume. Everything
 * else returns STATUS_INVALID_DEVICE_REQUEST, which is a valid answer. */
#include "winlvm.h"

NTSTATUS WinLvmMountIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp, WINLVM_LV_EXT *ext) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;

    if (code == IOCTL_MOUNTDEV_QUERY_DEVICE_NAME) {
        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUNTDEV_NAME)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
            return STATUS_BUFFER_OVERFLOW;
        }

        /* winmd returns a synthetic \Device\WinMD{uuid}; we return the
         * registered volume interface name of this LV's PDO. */
        ULONG nameLen = ext->ifname.Length;
        ULONG reqSize = sizeof(MOUNTDEV_NAME) + nameLen;

        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength < reqSize) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
            return STATUS_BUFFER_OVERFLOW;
        }

        PMOUNTDEV_NAME mountName = (PMOUNTDEV_NAME)Irp->AssociatedIrp.SystemBuffer;
        mountName->NameLength = (USHORT)nameLen;
        RtlCopyMemory(mountName->Name, ext->ifname.Buffer, nameLen);
        Irp->IoStatus.Information = reqSize;
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_MOUNTDEV_QUERY_UNIQUE_ID) {
        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUNTDEV_UNIQUE_ID)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
            return STATUS_BUFFER_OVERFLOW;
        }

        winlvm_logical_volume *lv = &ext->record->vg.lvs[ext->lv_index];
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

    if (code == IOCTL_DISK_GET_LENGTH_INFO) {
        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(GET_LENGTH_INFORMATION)) {
            Irp->IoStatus.Information = sizeof(GET_LENGTH_INFORMATION);
            return STATUS_BUFFER_OVERFLOW;
        }
        PGET_LENGTH_INFORMATION p = (PGET_LENGTH_INFORMATION)Irp->AssociatedIrp.SystemBuffer;
        p->Length.QuadPart = (LONGLONG)ext->byte_size;
        Irp->IoStatus.Information = sizeof(GET_LENGTH_INFORMATION);
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_DISK_GET_DRIVE_GEOMETRY) {
        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(DISK_GEOMETRY)) {
            Irp->IoStatus.Information = sizeof(DISK_GEOMETRY);
            return STATUS_BUFFER_OVERFLOW;
        }
        PDISK_GEOMETRY g = (PDISK_GEOMETRY)Irp->AssociatedIrp.SystemBuffer;
        g->Cylinders.QuadPart = (LONGLONG)(ext->byte_size / (63 * 255 * 512));
        g->MediaType = FixedMedia;
        g->TracksPerCylinder = 255;
        g->SectorsPerTrack = 63;
        g->BytesPerSector = 512;
        Irp->IoStatus.Information = sizeof(DISK_GEOMETRY);
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_STORAGE_CHECK_VERIFY ||
        code == IOCTL_DISK_CHECK_VERIFY) {
        /* No removable media; the media is present whenever the PDO exists. */
        Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_DISK_GET_PARTITION_INFO_EX) {
        /* We present a whole-disk-style volume; reporting a single
         * unrecognized (PARTITION_ENTRY_UNUSED) entry lets partmgr treat
         * the PDO as an unpartitioned device it can put a volume on. */
        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PARTITION_INFORMATION_EX)) {
            Irp->IoStatus.Information = sizeof(PARTITION_INFORMATION_EX);
            return STATUS_BUFFER_OVERFLOW;
        }
        PPARTITION_INFORMATION_EX p = (PPARTITION_INFORMATION_EX)Irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(p, sizeof(*p));
        p->PartitionStyle = PARTITION_STYLE_MBR;
        p->PartitionNumber = 0;
        p->RewritePartition = FALSE;
        p->StartingOffset.QuadPart = 0;
        p->PartitionLength.QuadPart = (LONGLONG)ext->byte_size;
        p->Mbr.PartitionType = PARTITION_ENTRY_UNUSED;
        p->Mbr.BootIndicator = FALSE;
        p->Mbr.RecognizedPartition = FALSE;
        p->Mbr.HiddenSectors = 0;
        Irp->IoStatus.Information = sizeof(PARTITION_INFORMATION_EX);
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME) {
        return STATUS_NOT_IMPLEMENTED;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}
