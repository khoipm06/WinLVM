/* winlvm_mount.c — Mount Manager + storage IOCTL surface for LV PDOs.
 *
 * Modeled on winmd's deliberately-tiny surface (mountmgr.c): the two
 * MOUNTDEV queries plus the storage geometry/length queries partmgr and
 * mountmgr issue before they will treat the device as a volume. Everything
 * else returns STATUS_INVALID_DEVICE_REQUEST, which is a valid answer. */
#include <stddef.h>   /* offsetof — MOUNTDEV variable-length sizing */
#include <ntifs.h>    /* FSCTL_* volume-lock codes (SOURCES-only include; */
                    /* ntifs.h after ntddk.h per WDK build order) */
#include "winlvm.h"

NTSTATUS WinLvmMountIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp, WINLVM_LV_EXT *ext) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;

    if (code == IOCTL_MOUNTDEV_QUERY_DEVICE_NAME) {
        /* MSDN contract (mountmgr/ni-mountmgr-ioctl_mountdev_query_device_name):
         * buffer < sizeof(MOUNTDEV_NAME) is not answerable at all — the
         * caller cannot even see the NameLength field. winbtrfs volume.c:313
         * answers STATUS_BUFFER_TOO_SMALL for that case; we match. */
        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUNTDEV_NAME)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
            return STATUS_BUFFER_TOO_SMALL;
        }

        PMOUNTDEV_NAME mountName = (PMOUNTDEV_NAME)Irp->AssociatedIrp.SystemBuffer;

        /* mountmgr requires a \Device-namespace name it can open — the
         * volume interface symlink is not valid here (winmd pattern). */
        UNICODE_STRING devName;
        RtlInitUnicodeString(&devName, ext->devname);
        mountName->NameLength = (USHORT)devName.Length;

        /* MSDN: too small for header+name -> Information = sizeof(MOUNTDEV_NAME),
         * Status = STATUS_BUFFER_OVERFLOW, and the NameLength member MUST be
         * filled so the caller can size its retry (winbtrfs volume.c:321-323
         * identical, modulo its TOO_SMALL/OF split). offsetof, not sizeof:
         * the header size is the length field plus the name that follows it. */
        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength <
            offsetof(MOUNTDEV_NAME, Name) + devName.Length) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
            return STATUS_BUFFER_OVERFLOW;
        }

        RtlCopyMemory(mountName->Name, devName.Buffer, devName.Length);
        Irp->IoStatus.Information = offsetof(MOUNTDEV_NAME, Name) + devName.Length;
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_MOUNTDEV_QUERY_UNIQUE_ID) {
        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUNTDEV_UNIQUE_ID)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
            return STATUS_BUFFER_TOO_SMALL;
        }

        const lvmcore_volume_group *view = ext->record->view;
        if (!view || ext->lv_index >= view->lv_count) {
            Irp->IoStatus.Information = 0;
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        const lvmcore_logical_volume *lv = &view->lvs[ext->lv_index];
        PMOUNTDEV_UNIQUE_ID uniqueId = (PMOUNTDEV_UNIQUE_ID)Irp->AssociatedIrp.SystemBuffer;
        uniqueId->UniqueIdLength = 32; /* raw 32-char LV UUID (dashes stripped) */

        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength <
            offsetof(MOUNTDEV_UNIQUE_ID, UniqueId) + 32) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
            return STATUS_BUFFER_OVERFLOW;
        }

        RtlCopyMemory(uniqueId->UniqueId, lv->id, 32);
        Irp->IoStatus.Information = offsetof(MOUNTDEV_UNIQUE_ID, UniqueId) + 32;
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
        /* Per-PV sector size query (StorageAccessAlignmentProperty) feeds
         * ext->logical_sector; the volume answers geometry in those units.
         * CHS values are synthetic (whole-disk volume): the only fields
         * consumers rely on are BytesPerSector and Cylinders. */
        ULONG bps = ext->logical_sector ? ext->logical_sector : 512;
        ULONGLONG chs_bytes = (ULONGLONG)63 * 255 * bps;
        PDISK_GEOMETRY g = (PDISK_GEOMETRY)Irp->AssociatedIrp.SystemBuffer;
        g->Cylinders.QuadPart = chs_bytes ? (LONGLONG)(ext->byte_size / chs_bytes) : 0;
        g->MediaType = FixedMedia;
        g->TracksPerCylinder = 255;
        g->SectorsPerTrack = 63;
        g->BytesPerSector = bps;
        Irp->IoStatus.Information = sizeof(DISK_GEOMETRY);
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_DISK_GET_DRIVE_GEOMETRY_EX) {
        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(DISK_GEOMETRY_EX)) {
            Irp->IoStatus.Information = sizeof(DISK_GEOMETRY_EX);
            return STATUS_BUFFER_OVERFLOW;
        }
        ULONG bps = ext->logical_sector ? ext->logical_sector : 512;
        ULONGLONG chs_bytes = (ULONGLONG)63 * 255 * bps;
        PDISK_GEOMETRY_EX g = (PDISK_GEOMETRY_EX)Irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(g, sizeof(*g));
        g->Geometry.Cylinders.QuadPart = chs_bytes ? (LONGLONG)(ext->byte_size / chs_bytes) : 0;
        g->Geometry.MediaType = FixedMedia;
        g->Geometry.TracksPerCylinder = 255;
        g->Geometry.SectorsPerTrack = 63;
        g->Geometry.BytesPerSector = bps;
        g->DiskSize.QuadPart = (LONGLONG)ext->byte_size;
        Irp->IoStatus.Information = sizeof(DISK_GEOMETRY_EX);
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

    /* Raw-volume semantics: diskspd/format-style tools open \\.\E: while a
     * filesystem may still hold cached state. The volume manager sends
     * dismount/lock FSCTLs and expects an answer; a stateless passthrough
     * volume has no cached state to flush — answer SUCCESS immediately
     * (classpnp raw-volume pattern; winbtrfs does the same in its volume
     * ioctl default). Without this, the 2026-09-13 diskspd raw-open
     * wedge: mountmgr's dismount handshake waited forever. */
    if (code == FSCTL_DISMOUNT_VOLUME || code == FSCTL_LOCK_VOLUME ||
        code == FSCTL_UNLOCK_VOLUME) {
        return STATUS_SUCCESS;
    }
    if (code == FSCTL_ALLOW_EXTENDED_DASD_IO) {
        Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}
