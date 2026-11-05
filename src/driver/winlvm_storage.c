/*
 * winlvm_storage.c — Storage, Disk, and Volume IOCTL handlers for LV PDOs.
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

#if !defined(_KERNEL_MODE) && !defined(HOST_WDM_TYPES_H)
#ifndef StorageDeviceSeekPenaltyProperty
#define StorageDeviceSeekPenaltyProperty 7
#endif

#ifndef _DEVICE_SEEK_PENALTY_DESCRIPTOR_DEFINED
typedef struct _DEVICE_SEEK_PENALTY_DESCRIPTOR {
    ULONG   Version;
    ULONG   Size;
    BOOLEAN IncursSeekPenalty;
} DEVICE_SEEK_PENALTY_DESCRIPTOR, *PDEVICE_SEEK_PENALTY_DESCRIPTOR;
#define _DEVICE_SEEK_PENALTY_DESCRIPTOR_DEFINED
#endif

#ifndef IOCTL_VOLUME_GET_GPT_ATTRIBUTES
#define IOCTL_VOLUME_GET_GPT_ATTRIBUTES CTL_CODE(IOCTL_VOLUME_BASE, 12, METHOD_BUFFERED, FILE_ANY_ACCESS)
typedef struct _VOLUME_GET_GPT_ATTRIBUTES_INFORMATION {
    ULONGLONG GptAttributes;
} VOLUME_GET_GPT_ATTRIBUTES_INFORMATION, *PVOLUME_GET_GPT_ATTRIBUTES_INFORMATION;
#endif

#ifndef IOCTL_VOLUME_IS_DYNAMIC
#define IOCTL_VOLUME_IS_DYNAMIC CTL_CODE(IOCTL_VOLUME_BASE, 14, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef IOCTL_VOLUME_ONLINE
#define IOCTL_VOLUME_ONLINE CTL_CODE(IOCTL_VOLUME_BASE, 2, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef IOCTL_VOLUME_POST_ONLINE
#define IOCTL_VOLUME_POST_ONLINE CTL_CODE(IOCTL_VOLUME_BASE, 4, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#endif

/* 1 MiB transfer cap guarantees 4 KiB striped transfers stay within WL_MAX_READ_PIECES (256). */
#define WINLVM_MAX_TRANSFER_BYTES (1024u * 1024u)
#define WINLVM_MAX_PHYSICAL_PAGES 257u

static NTSTATUS WinLvmHandleStorageQueryProperty(WINLVM_LV_EXT *ext, PIRP Irp,
                                                 ULONG inLen, ULONG outLen) {
    PVOID sysBuf = Irp->AssociatedIrp.SystemBuffer;
    if (!sysBuf || inLen < sizeof(STORAGE_PROPERTY_QUERY)) {
        Irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }
    PSTORAGE_PROPERTY_QUERY q = (PSTORAGE_PROPERTY_QUERY)sysBuf;
    if (q->QueryType != PropertyStandardQuery && q->QueryType != PropertyExistsQuery) {
        return STATUS_NOT_SUPPORTED;
    }

    if (q->PropertyId == StorageDeviceProperty) {
        static const char kVendor[] = "WinLVM";
        static const char kProduct[] = "Logical Volume";
        static const char kRev[] = "1.0";
        const char *raw_uuid = "";
#ifdef _KERNEL_MODE
        if (ext->lv_uuid[0] != '\0') raw_uuid = ext->lv_uuid;
#else
        const lvmcore_volume_group *view = ext->record ? ext->record->view : NULL;
        if (view && view->lvs && ext->lv_index < view->lv_count) {
            raw_uuid = view->lvs[ext->lv_index].id;
        }
#endif

        ULONG vLen = (ULONG)strlen(kVendor) + 1;
        ULONG pLen = (ULONG)strlen(kProduct) + 1;
        ULONG rLen = (ULONG)strlen(kRev) + 1;
        ULONG sLen = (ULONG)strlen(raw_uuid) + 1;
        ULONG totalReq = (ULONG)(sizeof(STORAGE_DEVICE_DESCRIPTOR) + vLen + pLen + rLen + sLen);

        if (outLen < sizeof(STORAGE_DESCRIPTOR_HEADER)) {
            Irp->IoStatus.Information = sizeof(STORAGE_DESCRIPTOR_HEADER);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (outLen < totalReq) {
            PSTORAGE_DESCRIPTOR_HEADER hdr = (PSTORAGE_DESCRIPTOR_HEADER)sysBuf;
            hdr->Version = sizeof(STORAGE_DEVICE_DESCRIPTOR);
            hdr->Size = totalReq;
            Irp->IoStatus.Information = sizeof(STORAGE_DESCRIPTOR_HEADER);
            return STATUS_BUFFER_OVERFLOW;
        }
        PSTORAGE_DEVICE_DESCRIPTOR desc = (PSTORAGE_DEVICE_DESCRIPTOR)sysBuf;
        RtlZeroMemory(desc, totalReq);
        desc->Version = sizeof(STORAGE_DEVICE_DESCRIPTOR);
        desc->Size = totalReq;
        desc->DeviceType = FILE_DEVICE_DISK;
        desc->RemovableMedia = FALSE;
        desc->CommandQueueing = TRUE;
        desc->BusType = BusTypeVirtual;

        ULONG off = (ULONG)sizeof(STORAGE_DEVICE_DESCRIPTOR);
        desc->VendorIdOffset = off;
        RtlCopyMemory((PUCHAR)desc + off, kVendor, vLen);
        off += vLen;
        desc->ProductIdOffset = off;
        RtlCopyMemory((PUCHAR)desc + off, kProduct, pLen);
        off += pLen;
        desc->ProductRevisionOffset = off;
        RtlCopyMemory((PUCHAR)desc + off, kRev, rLen);
        off += rLen;
        desc->SerialNumberOffset = off;
        RtlCopyMemory((PUCHAR)desc + off, raw_uuid, sLen);

        Irp->IoStatus.Information = totalReq;
        return STATUS_SUCCESS;
    }

    if (q->PropertyId == StorageAdapterProperty) {
        if (outLen < sizeof(STORAGE_DESCRIPTOR_HEADER)) {
            Irp->IoStatus.Information = sizeof(STORAGE_DESCRIPTOR_HEADER);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (outLen < sizeof(STORAGE_ADAPTER_DESCRIPTOR)) {
            PSTORAGE_DESCRIPTOR_HEADER hdr = (PSTORAGE_DESCRIPTOR_HEADER)sysBuf;
            hdr->Version = sizeof(STORAGE_ADAPTER_DESCRIPTOR);
            hdr->Size = sizeof(STORAGE_ADAPTER_DESCRIPTOR);
            Irp->IoStatus.Information = sizeof(STORAGE_DESCRIPTOR_HEADER);
            return STATUS_BUFFER_OVERFLOW;
        }
        PSTORAGE_ADAPTER_DESCRIPTOR desc = (PSTORAGE_ADAPTER_DESCRIPTOR)sysBuf;
        RtlZeroMemory(desc, sizeof(*desc));
        desc->Version = sizeof(STORAGE_ADAPTER_DESCRIPTOR);
        desc->Size = sizeof(STORAGE_ADAPTER_DESCRIPTOR);
        desc->MaximumTransferLength = WINLVM_MAX_TRANSFER_BYTES;
        desc->MaximumPhysicalPages = WINLVM_MAX_PHYSICAL_PAGES;
        desc->CommandQueueing = TRUE;
        desc->AcceleratedTransfer = TRUE;
        desc->BusType = (UCHAR)BusTypeVirtual;
        desc->BusMajorVersion = 1;
        Irp->IoStatus.Information = sizeof(STORAGE_ADAPTER_DESCRIPTOR);
        return STATUS_SUCCESS;
    }

    if (q->PropertyId == StorageAccessAlignmentProperty) {
        if (outLen < sizeof(STORAGE_DESCRIPTOR_HEADER)) {
            Irp->IoStatus.Information = sizeof(STORAGE_DESCRIPTOR_HEADER);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (outLen < sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR)) {
            PSTORAGE_DESCRIPTOR_HEADER hdr = (PSTORAGE_DESCRIPTOR_HEADER)sysBuf;
            hdr->Version = sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR);
            hdr->Size = sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR);
            Irp->IoStatus.Information = sizeof(STORAGE_DESCRIPTOR_HEADER);
            return STATUS_BUFFER_OVERFLOW;
        }
        PSTORAGE_ACCESS_ALIGNMENT_DESCRIPTOR desc = (PSTORAGE_ACCESS_ALIGNMENT_DESCRIPTOR)sysBuf;
        RtlZeroMemory(desc, sizeof(*desc));
        desc->Version = sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR);
        desc->Size = sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR);
        desc->BytesPerCacheLine = 64;
        desc->BytesPerLogicalSector = ext->logical_sector ? ext->logical_sector : 512;
        desc->BytesPerPhysicalSector = ext->physical_sector ? ext->physical_sector : 512;
        Irp->IoStatus.Information = sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR);
        return STATUS_SUCCESS;
    }

    if (q->PropertyId == StorageDeviceSeekPenaltyProperty) {
        if (outLen < sizeof(STORAGE_DESCRIPTOR_HEADER)) {
            Irp->IoStatus.Information = sizeof(STORAGE_DESCRIPTOR_HEADER);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (outLen < sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR)) {
            PSTORAGE_DESCRIPTOR_HEADER hdr = (PSTORAGE_DESCRIPTOR_HEADER)sysBuf;
            hdr->Version = sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR);
            hdr->Size = sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR);
            Irp->IoStatus.Information = sizeof(STORAGE_DESCRIPTOR_HEADER);
            return STATUS_BUFFER_OVERFLOW;
        }
        PDEVICE_SEEK_PENALTY_DESCRIPTOR desc = (PDEVICE_SEEK_PENALTY_DESCRIPTOR)sysBuf;
        desc->Version = sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR);
        desc->Size = sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR);
        desc->IncursSeekPenalty = FALSE;
        Irp->IoStatus.Information = sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR);
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS WinLvmStorageIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp, WINLVM_LV_EXT *ext) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;
    ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID sysBuf = Irp->AssociatedIrp.SystemBuffer;
    UNREFERENCED_PARAMETER(DeviceObject);

    if (code == IOCTL_DISK_GET_LENGTH_INFO) {
        if (outLen < sizeof(GET_LENGTH_INFORMATION)) {
            Irp->IoStatus.Information = sizeof(GET_LENGTH_INFORMATION);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;
        ((PGET_LENGTH_INFORMATION)sysBuf)->Length.QuadPart = (LONGLONG)ext->byte_size;
        Irp->IoStatus.Information = sizeof(GET_LENGTH_INFORMATION);
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_DISK_GET_DRIVE_GEOMETRY) {
        if (outLen < sizeof(DISK_GEOMETRY)) {
            Irp->IoStatus.Information = sizeof(DISK_GEOMETRY);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;
        ULONG bps = ext->logical_sector ? ext->logical_sector : 512;
        ULONGLONG chs_bytes = (ULONGLONG)63 * 255 * bps;
        PDISK_GEOMETRY g = (PDISK_GEOMETRY)sysBuf;
        g->Cylinders.QuadPart = chs_bytes ? (LONGLONG)(ext->byte_size / chs_bytes) : 0;
        g->MediaType = FixedMedia;
        g->TracksPerCylinder = 255;
        g->SectorsPerTrack = 63;
        g->BytesPerSector = bps;
        Irp->IoStatus.Information = sizeof(DISK_GEOMETRY);
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_DISK_GET_DRIVE_GEOMETRY_EX) {
        if (outLen < sizeof(DISK_GEOMETRY_EX)) {
            Irp->IoStatus.Information = sizeof(DISK_GEOMETRY_EX);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;
        ULONG bps = ext->logical_sector ? ext->logical_sector : 512;
        ULONGLONG chs_bytes = (ULONGLONG)63 * 255 * bps;
        PDISK_GEOMETRY_EX g = (PDISK_GEOMETRY_EX)sysBuf;
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

    if (code == IOCTL_DISK_IS_WRITABLE) {
        BOOLEAN ro = FALSE;
#ifdef _KERNEL_MODE
        ro = ext->read_only;
#else
        const lvmcore_volume_group *view = ext->record ? ext->record->view : NULL;
        if (view) {
            if (view->system_id[0] != '\0' || view->read_only) ro = TRUE;
            if (view->lvs && ext->lv_index < view->lv_count &&
                view->lvs[ext->lv_index].read_only) {
                ro = TRUE;
            }
        }
#endif
        Irp->IoStatus.Information = 0;
        return ro ? STATUS_MEDIA_WRITE_PROTECTED : STATUS_SUCCESS;
    }

    if (code == IOCTL_STORAGE_CHECK_VERIFY || code == IOCTL_DISK_CHECK_VERIFY) {
        Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_DISK_GET_PARTITION_INFO) {
        if (outLen < sizeof(PARTITION_INFORMATION)) {
            Irp->IoStatus.Information = sizeof(PARTITION_INFORMATION);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;
        PPARTITION_INFORMATION p = (PPARTITION_INFORMATION)sysBuf;
        RtlZeroMemory(p, sizeof(*p));
        p->PartitionLength.QuadPart = (LONGLONG)ext->byte_size;
        p->PartitionNumber = 1;
        p->PartitionType = 0x07;
        p->RecognizedPartition = TRUE;
        Irp->IoStatus.Information = sizeof(PARTITION_INFORMATION);
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_DISK_GET_PARTITION_INFO_EX) {
        if (outLen < sizeof(PARTITION_INFORMATION_EX)) {
            Irp->IoStatus.Information = sizeof(PARTITION_INFORMATION_EX);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;
        PPARTITION_INFORMATION_EX p = (PPARTITION_INFORMATION_EX)sysBuf;
        RtlZeroMemory(p, sizeof(*p));
        p->PartitionStyle = PARTITION_STYLE_MBR;
        p->PartitionLength.QuadPart = (LONGLONG)ext->byte_size;
        p->PartitionNumber = 1;
        p->Mbr.PartitionType = 0x07;
        p->Mbr.RecognizedPartition = TRUE;
        Irp->IoStatus.Information = sizeof(PARTITION_INFORMATION_EX);
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_DISK_GET_DRIVE_LAYOUT_EX) {
        if (outLen < sizeof(DRIVE_LAYOUT_INFORMATION_EX)) {
            Irp->IoStatus.Information = sizeof(DRIVE_LAYOUT_INFORMATION_EX);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;
        PDRIVE_LAYOUT_INFORMATION_EX layout = (PDRIVE_LAYOUT_INFORMATION_EX)sysBuf;
        RtlZeroMemory(layout, sizeof(DRIVE_LAYOUT_INFORMATION_EX));
        layout->PartitionStyle = PARTITION_STYLE_MBR;
        layout->PartitionCount = 1;
        layout->Mbr.Signature = 0x574C564Du;

        PPARTITION_INFORMATION_EX entry = &layout->PartitionEntry[0];
        entry->PartitionStyle = PARTITION_STYLE_MBR;
        entry->PartitionLength.QuadPart = (LONGLONG)ext->byte_size;
        entry->PartitionNumber = 1;
        entry->Mbr.PartitionType = 0x07;
        entry->Mbr.RecognizedPartition = TRUE;
        Irp->IoStatus.Information = sizeof(DRIVE_LAYOUT_INFORMATION_EX);
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_DISK_GET_DRIVE_LAYOUT) {
        if (outLen < sizeof(DRIVE_LAYOUT_INFORMATION)) {
            Irp->IoStatus.Information = sizeof(DRIVE_LAYOUT_INFORMATION);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;
        PDRIVE_LAYOUT_INFORMATION layout = (PDRIVE_LAYOUT_INFORMATION)sysBuf;
        RtlZeroMemory(layout, sizeof(DRIVE_LAYOUT_INFORMATION));
        layout->PartitionCount = 1;
        layout->Signature = 0x574C564Du;

        PPARTITION_INFORMATION entry = &layout->PartitionEntry[0];
        entry->PartitionLength.QuadPart = (LONGLONG)ext->byte_size;
        entry->PartitionNumber = 1;
        entry->PartitionType = 0x07;
        entry->RecognizedPartition = TRUE;
        Irp->IoStatus.Information = sizeof(DRIVE_LAYOUT_INFORMATION);
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_STORAGE_QUERY_PROPERTY) {
        return WinLvmHandleStorageQueryProperty(
            ext, Irp, irpSp->Parameters.DeviceIoControl.InputBufferLength, outLen);
    }

    if (code == IOCTL_STORAGE_GET_DEVICE_NUMBER) {
        if (outLen < sizeof(STORAGE_DEVICE_NUMBER)) {
            Irp->IoStatus.Information = sizeof(STORAGE_DEVICE_NUMBER);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;
        PSTORAGE_DEVICE_NUMBER sdn = (PSTORAGE_DEVICE_NUMBER)sysBuf;
        sdn->DeviceType = FILE_DEVICE_DISK;
        sdn->DeviceNumber = (ULONG)(ext->lv_index + 1);
        sdn->PartitionNumber = 1;
        Irp->IoStatus.Information = sizeof(STORAGE_DEVICE_NUMBER);
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS) {
        if (outLen < sizeof(VOLUME_DISK_EXTENTS)) {
            Irp->IoStatus.Information = 0;
            return STATUS_INVALID_PARAMETER;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;
        ULONG num_extents = (ULONG)(ext->backing_count > 0 ? ext->backing_count : 1);
        ULONG req_size = (ULONG)(FIELD_OFFSET(VOLUME_DISK_EXTENTS, Extents[0]) +
                                 (num_extents * sizeof(DISK_EXTENT)));
        PVOLUME_DISK_EXTENTS extents = (PVOLUME_DISK_EXTENTS)sysBuf;
        if (outLen < req_size) {
            extents->NumberOfDiskExtents = num_extents;
            Irp->IoStatus.Information = sizeof(VOLUME_DISK_EXTENTS);
            return STATUS_BUFFER_OVERFLOW;
        }
        extents->NumberOfDiskExtents = num_extents;
        if (ext->backing_count > 0) {
            ULONGLONG per_extent = ext->byte_size / num_extents;
            for (ULONG i = 0; i < num_extents; i++) {
                extents->Extents[i].DiskNumber = (ULONG)(ext->pv_slot[i] + 1);
                extents->Extents[i].StartingOffset.QuadPart = 0;
                if (i == num_extents - 1) {
                    extents->Extents[i].ExtentLength.QuadPart =
                        (LONGLONG)(ext->byte_size - per_extent * i);
                } else {
                    extents->Extents[i].ExtentLength.QuadPart = (LONGLONG)per_extent;
                }
            }
        } else {
            extents->Extents[0].DiskNumber = (ULONG)(ext->lv_index + 1);
            extents->Extents[0].StartingOffset.QuadPart = 0;
            extents->Extents[0].ExtentLength.QuadPart = (LONGLONG)ext->byte_size;
        }
        Irp->IoStatus.Information = req_size;
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_VOLUME_IS_DYNAMIC) {
        if (outLen < sizeof(BOOLEAN)) {
            Irp->IoStatus.Information = sizeof(BOOLEAN);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;
        *(PBOOLEAN)sysBuf = FALSE;
        Irp->IoStatus.Information = sizeof(BOOLEAN);
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_VOLUME_ONLINE || code == IOCTL_VOLUME_POST_ONLINE) {
        Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_VOLUME_GET_GPT_ATTRIBUTES) {
        if (outLen < sizeof(VOLUME_GET_GPT_ATTRIBUTES_INFORMATION)) {
            Irp->IoStatus.Information = sizeof(VOLUME_GET_GPT_ATTRIBUTES_INFORMATION);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;
        RtlZeroMemory(sysBuf, sizeof(VOLUME_GET_GPT_ATTRIBUTES_INFORMATION));
        Irp->IoStatus.Information = sizeof(VOLUME_GET_GPT_ATTRIBUTES_INFORMATION);
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}
