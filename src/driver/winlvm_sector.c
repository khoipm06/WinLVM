/*
 * winlvm_sector.c — Backing-disk geometry/writability queries, synchronous
 *                   sector-aligned metadata reads, and LV-to-PV piece mapping.
 */
#include "winlvm.h"
#include <ntstrsafe.h>

static NTSTATUS WinLvmSyncDeviceIoctl(PDEVICE_OBJECT pdo, PFILE_OBJECT file, ULONG code,
                                      PVOID in_buf, ULONG in_len,
                                      PVOID out_buf, ULONG out_len) {
    KEVENT ev;
    IO_STATUS_BLOCK iosb;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    RtlZeroMemory(&iosb, sizeof(iosb));
    PIRP irp = IoBuildDeviceIoControlRequest(
        code, pdo, in_buf, in_len, out_buf, out_len, FALSE, &ev, &iosb);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    IoGetNextIrpStackLocation(irp)->FileObject = file;
    NTSTATUS status = IoCallDriver(pdo, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    return status;
}

NTSTATUS WinLvmOpenBackingDevice(const UNICODE_STRING *symLinkOrNtPath,
                                 PDRIVER_OBJECT own_driver,
                                 UNICODE_STRING *nt_path_out,
                                 PFILE_OBJECT *file_out,
                                 PDEVICE_OBJECT *pdo_out,
                                 BOOLEAN *read_only_out) {
    RtlZeroMemory(nt_path_out, sizeof(*nt_path_out));
    *file_out = NULL;
    *pdo_out = NULL;
    *read_only_out = FALSE;

    if (!symLinkOrNtPath || !symLinkOrNtPath->Buffer ||
        symLinkOrNtPath->Length < sizeof(WCHAR) ||
        (symLinkOrNtPath->Length % sizeof(WCHAR)) != 0 ||
        symLinkOrNtPath->Length > (USHORT)(0xFFFEu - sizeof(WCHAR))) {
        return STATUS_INVALID_PARAMETER;
    }

    USHORT len = symLinkOrNtPath->Length;
    PWCHAR path_buf = (PWCHAR)ExAllocatePoolZero(
        NonPagedPool, (size_t)len + sizeof(WCHAR), WINLVM_WIN_TAG);
    if (!path_buf) return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(path_buf, symLinkOrNtPath->Buffer, len);
    if (len >= 4 * sizeof(WCHAR) &&
        path_buf[0] == L'\\' && path_buf[1] == L'\\' &&
        (path_buf[2] == L'.' || path_buf[2] == L'?') && path_buf[3] == L'\\') {
        path_buf[1] = L'?';
        path_buf[2] = L'?';
    }

    UNICODE_STRING nt_path;
    nt_path.Buffer = path_buf;
    nt_path.Length = len;
    nt_path.MaximumLength = (USHORT)(len + sizeof(WCHAR));
    if (WinLvmShouldIgnoreLinkName(&nt_path)) {
        ExFreePoolWithTag(path_buf, WINLVM_WIN_TAG);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    PFILE_OBJECT disk_file = NULL;
    PDEVICE_OBJECT related_dev = NULL;
    NTSTATUS status = IoGetDeviceObjectPointer(
        &nt_path, FILE_READ_ATTRIBUTES, &disk_file, &related_dev);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(path_buf, WINLVM_WIN_TAG);
        return status;
    }

    /* Target the underlying storage device stack rather than disk_file->Vpb->DeviceObject
     * so storage IRPs never traverse a mounted RAW/FLTMGR filesystem filter stack. */
    PDEVICE_OBJECT base_dev = disk_file->DeviceObject ? disk_file->DeviceObject : related_dev;
    if (!base_dev) {
        ObDereferenceObject(disk_file);
        ExFreePoolWithTag(path_buf, WINLVM_WIN_TAG);
        return STATUS_NO_SUCH_DEVICE;
    }
    PDEVICE_OBJECT storage_dev = IoGetAttachedDeviceReference(base_dev);
    if (!storage_dev) {
        ObDereferenceObject(disk_file);
        ExFreePoolWithTag(path_buf, WINLVM_WIN_TAG);
        return STATUS_NO_SUCH_DEVICE;
    }
    if (own_driver &&
        (storage_dev->DriverObject == own_driver || base_dev->DriverObject == own_driver)) {
        ObDereferenceObject(storage_dev);
        ObDereferenceObject(disk_file);
        ExFreePoolWithTag(path_buf, WINLVM_WIN_TAG);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    *nt_path_out = nt_path;
    *file_out = disk_file;
    *pdo_out = storage_dev;
    *read_only_out = FALSE;
    return STATUS_SUCCESS;
}

void WinLvmQuerySectorSize(PDEVICE_OBJECT disk_pdo, PFILE_OBJECT disk_file,
                           ULONG *logical_out, ULONG *physical_out) {
    ULONG logical = 512, physical = 512;
    if (disk_pdo && disk_file) {
        STORAGE_PROPERTY_QUERY q;
        STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR d;
        RtlZeroMemory(&q, sizeof(q));
        RtlZeroMemory(&d, sizeof(d));
        q.PropertyId = StorageAccessAlignmentProperty;
        q.QueryType = PropertyStandardQuery;

        NTSTATUS status = WinLvmSyncDeviceIoctl(
            disk_pdo, disk_file, IOCTL_STORAGE_QUERY_PROPERTY,
            &q, sizeof(q), &d, sizeof(d));
        if (NT_SUCCESS(status) &&
            d.BytesPerLogicalSector >= 512 && d.BytesPerLogicalSector <= 65536 &&
            (d.BytesPerLogicalSector & (d.BytesPerLogicalSector - 1)) == 0 &&
            d.BytesPerPhysicalSector >= d.BytesPerLogicalSector) {
            logical = d.BytesPerLogicalSector;
            physical = d.BytesPerPhysicalSector;
        }
    }
    *logical_out = logical;
    *physical_out = physical;
}

BOOLEAN WinLvmQueryDiskWritable(PDEVICE_OBJECT disk_pdo, PFILE_OBJECT disk_file) {
    if (!disk_pdo || !disk_file) return FALSE;
    NTSTATUS status = WinLvmSyncDeviceIoctl(
        disk_pdo, disk_file, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0);
    return (status == STATUS_MEDIA_WRITE_PROTECTED) ? FALSE : TRUE;
}

size_t winlvm_disk_source_read(void *impl, uint64_t off, void *buf, size_t len) {
    WINLVM_DISK_SOURCE *src = (WINLVM_DISK_SOURCE *)impl;

    if (!src || !src->disk_pdo || !buf || len == 0) return 0;
    if (len > UINT64_MAX - off) return 0;

    uint64_t sec = (src->sector_size >= 512) ? (uint64_t)src->sector_size : 512ULL;
    uint64_t start = (off / sec) * sec;
    uint64_t raw_end = off + (uint64_t)len;
    if ((sec - 1ULL) > UINT64_MAX - raw_end) return 0;
    uint64_t end = ((raw_end + sec - 1ULL) / sec) * sec;
    if (end <= start || (end - start) > 0xFFFFFFFFULL) return 0;

    size_t window = (size_t)(end - start);
    size_t skip = (size_t)(off - start);

    unsigned char *bounce = (unsigned char *)ExAllocatePoolZero(
        NonPagedPool, window, WINLVM_WIN_TAG);
    if (!bounce) return 0;

    LARGE_INTEGER offset;
    offset.QuadPart = (LONGLONG)start;

    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IO_STATUS_BLOCK io_status;
    RtlZeroMemory(&io_status, sizeof(io_status));

    PIRP irp = IoBuildSynchronousFsdRequest(
        IRP_MJ_READ, src->disk_pdo, bounce, (ULONG)window,
        &offset, &event, &io_status);
    if (!irp) {
        ExFreePoolWithTag(bounce, WINLVM_WIN_TAG);
        return 0;
    }

    IoGetNextIrpStackLocation(irp)->FileObject = src->file;
    NTSTATUS status = IoCallDriver(src->disk_pdo, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = io_status.Status;
    }

    size_t got = 0;
    if (NT_SUCCESS(status) && (size_t)io_status.Information > skip) {
        size_t avail = (size_t)io_status.Information - skip;
        if (avail > len) avail = len;
        RtlCopyMemory(buf, bounce + skip, avail);
        got = avail;
    } else if (!NT_SUCCESS(status)) {
        WL_TRACE_2("disk_source_read: off=%I64u failed 0x%08x", start, (unsigned)status);
    }

    ExFreePoolWithTag(bounce, WINLVM_WIN_TAG);
    return got;
}

NTSTATUS WinLvmReadVgFromDevice(PDEVICE_OBJECT disk_pdo, PFILE_OBJECT disk_file,
                                ULONG logical_sector, WINLVM_VIEW_BLOCK **block_out,
                                char pv_uuid_out[WL_VG_UUID_LEN]) {
    *block_out = NULL;
    if (pv_uuid_out) pv_uuid_out[0] = '\0';

    WINLVM_VIEW_BLOCK *block = (WINLVM_VIEW_BLOCK *)ExAllocatePoolZero(
        NonPagedPool, sizeof(WINLVM_VIEW_BLOCK), WINLVM_WIN_TAG);
    if (!block) return STATUS_INSUFFICIENT_RESOURCES;

    lvmcore_arena_init(&block->arena, block->buf, WINLVM_VIEW_ARENA_CAP);

    uint64_t disk_len = UINT64_MAX;
    GET_LENGTH_INFORMATION len_info;
    RtlZeroMemory(&len_info, sizeof(len_info));
    if (NT_SUCCESS(WinLvmSyncDeviceIoctl(
            disk_pdo, disk_file, IOCTL_DISK_GET_LENGTH_INFO,
            NULL, 0, &len_info, sizeof(len_info))) &&
        len_info.Length.QuadPart > 0) {
        disk_len = (uint64_t)len_info.Length.QuadPart;
    }

    WINLVM_DISK_SOURCE src;
    src.disk_pdo = disk_pdo;
    src.file = disk_file;
    src.sector_size = logical_sector;

    lvmcore_byte_source bsrc;
    bsrc.impl = &src;
    bsrc.size = disk_len;
    bsrc.read = winlvm_disk_source_read;

    lvmcore_pv_label label;
    lvmcore_err err;
    RtlZeroMemory(&label, sizeof(label));
    RtlZeroMemory(&err, sizeof(err));
    lvmcore_status st = lvmcore_read_pv_label(&bsrc, &block->arena, &label, &err);
    if (st != LVMCORE_OK) {
        WL_TRACE_2("probe label failed: st=%d err=%s", (int)st, err.message);
        ExFreePoolWithTag(block, WINLVM_WIN_TAG);
        return (st == LVMCORE_ERR_NO_LABEL) ? STATUS_NOT_FOUND : STATUS_DATA_ERROR;
    }

    st = lvmcore_read_vg(&bsrc, LVMCORE_READ_FLAG_DEFAULT, &block->arena, &block->vg, &err);
    if (st != LVMCORE_OK) {
        WL_TRACE_2("probe vg failed: st=%d err=%s", (int)st, err.message);
        ExFreePoolWithTag(block, WINLVM_WIN_TAG);
        return STATUS_DATA_ERROR;
    }

    if (pv_uuid_out) {
        RtlCopyMemory(pv_uuid_out, label.pv_uuid, WL_VG_UUID_LEN);
        pv_uuid_out[WL_VG_UUID_LEN - 1] = '\0';
    }
    *block_out = block;
    return STATUS_SUCCESS;
}

static BOOLEAN WinLvmGuidIsZero(const GUID *g) {
    static const GUID kZeroGuid = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };
    return RtlEqualMemory(g, &kZeroGuid, sizeof(GUID)) ? TRUE : FALSE;
}

BOOLEAN WinLvmGetDiskPartitions(PDEVICE_OBJECT disk_pdo, PFILE_OBJECT disk_file,
                                ULONG *device_num_out, ULONG *parts_out,
                                size_t parts_cap, size_t *parts_count_out) {
    *device_num_out = 0;
    *parts_count_out = 0;
    if (!disk_pdo || !disk_file || !parts_out || parts_cap == 0 ||
        KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return FALSE;
    }

    STORAGE_DEVICE_NUMBER sdn;
    RtlZeroMemory(&sdn, sizeof(sdn));
    if (!NT_SUCCESS(WinLvmSyncDeviceIoctl(
            disk_pdo, disk_file, IOCTL_STORAGE_GET_DEVICE_NUMBER,
            NULL, 0, &sdn, sizeof(sdn)))) {
        return FALSE;
    }
    if (sdn.DeviceType != FILE_DEVICE_DISK ||
        (sdn.PartitionNumber != 0 && sdn.PartitionNumber != (ULONG)-1)) {
        return FALSE;
    }

    const ULONG max_entries = 128u;
    const ULONG buf_len = (ULONG)(FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) +
                                  max_entries * sizeof(PARTITION_INFORMATION_EX));
    PDRIVE_LAYOUT_INFORMATION_EX layout = (PDRIVE_LAYOUT_INFORMATION_EX)ExAllocatePoolZero(
        NonPagedPool, buf_len, WINLVM_WIN_TAG);
    if (!layout) return FALSE;

    NTSTATUS st = WinLvmSyncDeviceIoctl(
        disk_pdo, disk_file, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
        NULL, 0, layout, buf_len);
    if (!NT_SUCCESS(st) ||
        (layout->PartitionStyle != PARTITION_STYLE_GPT &&
         layout->PartitionStyle != PARTITION_STYLE_MBR)) {
        ExFreePoolWithTag(layout, WINLVM_WIN_TAG);
        return FALSE;
    }

    ULONG limit = layout->PartitionCount;
    if (limit > max_entries) limit = max_entries;
    size_t found = 0;
    for (ULONG i = 0; i < limit && found < parts_cap; i++) {
        const PARTITION_INFORMATION_EX *e = &layout->PartitionEntry[i];
        if (e->PartitionLength.QuadPart <= 0 || e->PartitionNumber == 0) continue;
        if (layout->PartitionStyle == PARTITION_STYLE_GPT) {
            if (e->PartitionStyle != PARTITION_STYLE_GPT ||
                WinLvmGuidIsZero(&e->Gpt.PartitionType)) {
                continue;
            }
        } else {
            UCHAR pt = e->Mbr.PartitionType;
            if (e->PartitionStyle != PARTITION_STYLE_MBR ||
                pt == PARTITION_ENTRY_UNUSED || pt == PARTITION_EXTENDED ||
                pt == PARTITION_XINT13_EXTENDED || pt == 0x85 || pt == 0xEE) {
                continue;
            }
        }
        BOOLEAN dup = FALSE;
        for (size_t j = 0; j < found; j++) {
            if (parts_out[j] == e->PartitionNumber) { dup = TRUE; break; }
        }
        if (!dup) parts_out[found++] = e->PartitionNumber;
    }

    ExFreePoolWithTag(layout, WINLVM_WIN_TAG);
    *device_num_out = sdn.DeviceNumber;
    *parts_count_out = found;
    return (found > 0) ? TRUE : FALSE;
}

NTSTATUS WinLvmProbePartitionList(ULONG device_num, const ULONG *parts,
                                  size_t count, BOOLEAN explicit_probe) {
    NTSTATUS any_ok = STATUS_NOT_FOUND;
    for (size_t i = 0; i < count; i++) {
        WCHAR buf[64];
        UNICODE_STRING u;
        if (NT_SUCCESS(RtlStringCchPrintfW(
                buf, sizeof(buf) / sizeof(WCHAR),
                L"\\??\\Harddisk%uPartition%u",
                (unsigned)device_num, (unsigned)parts[i]))) {
            RtlInitUnicodeString(&u, buf);
            NTSTATUS st = WinLvmProbeDevicePathEx(&u, FALSE, explicit_probe);
            if (NT_SUCCESS(st)) {
                any_ok = STATUS_SUCCESS;
                continue;
            }
            if (st != STATUS_OBJECT_NAME_NOT_FOUND &&
                st != STATUS_OBJECT_PATH_NOT_FOUND) {
                continue;
            }
        }
        if (NT_SUCCESS(RtlStringCchPrintfW(
                buf, sizeof(buf) / sizeof(WCHAR),
                L"\\Device\\Harddisk%u\\Partition%u",
                (unsigned)device_num, (unsigned)parts[i]))) {
            RtlInitUnicodeString(&u, buf);
            NTSTATUS st = WinLvmProbeDevicePathEx(&u, FALSE, explicit_probe);
            if (NT_SUCCESS(st)) any_ok = STATUS_SUCCESS;
        }
    }
    return any_ok;
}

size_t LvMapToPieces(WINLVM_LV_EXT *ext,
                     const lvmcore_volume_group *view,
                     uint64_t lv_byte, uint32_t len,
                     WL_IO_PIECE *pieces, size_t cap) {
    wl_map_iter it;
    wl_map_begin(&it, view, ext->record, ext->lv_index, lv_byte, len);
    size_t n = 0;
    uint32_t parent_off = 0;

    for (;;) {
        uint32_t pv_slot;
        uint64_t pv_byte, piece_len;
        int r = wl_map_next(&it, &pv_slot, &pv_byte, &piece_len);
        if (r == 0) break;
        if (r < 0 || n >= cap) return 0;
        if (piece_len == 0 || piece_len > (uint64_t)len ||
            parent_off + (uint32_t)piece_len > len) {
            return 0;
        }

        size_t backing_idx = SIZE_MAX;
        for (size_t k = 0; k < ext->backing_count; k++) {
            if (ext->pv_slot[k] == pv_slot) {
                backing_idx = k;
                break;
            }
        }
        if (backing_idx == SIZE_MAX) return 0;

        ULONG logical = ext->backing_logical[backing_idx];
        if (logical == 0 || (logical & (logical - 1)) != 0) return 0;

        WL_IO_PIECE *pc = &pieces[n];
        RtlZeroMemory(pc, sizeof(*pc));
        pc->pv_slot = pv_slot;
        pc->backing_idx = (uint32_t)backing_idx;
        pc->len = (uint32_t)piece_len;
        pc->parent_off = parent_off;
        pc->pv_byte = pv_byte;
        pc->logical = logical;

        uint64_t mask = (uint64_t)logical - 1;
        if ((pv_byte & mask) == 0 && (piece_len & mask) == 0) {
            pc->bounce = 0;
            pc->sub_offset = pv_byte;
            pc->sub_len = (uint32_t)piece_len;
        } else {
            uint64_t win_start = pv_byte & ~mask;
            if (piece_len > UINT64_MAX - pv_byte) return 0;
            if (mask > UINT64_MAX - (pv_byte + piece_len)) return 0;
            uint64_t win_end = (pv_byte + piece_len + mask) & ~mask;
            if (win_end <= win_start || (win_end - win_start) > 0xFFFFFFFFULL) return 0;
            pc->bounce = 1;
            pc->sub_offset = win_start;
            pc->sub_len = (uint32_t)(win_end - win_start);
            pc->bounce_skip = (uint32_t)(pv_byte - win_start);
        }

        if ((uint64_t)pc->parent_off + pc->len > len) return 0;
        parent_off += pc->len;
        n++;
    }
    return n;
}
