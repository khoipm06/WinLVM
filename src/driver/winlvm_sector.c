/*
 * winlvm_sector.c — Backing-disk geometry/writability queries, synchronous
 *                   sector-aligned metadata reads, and LV-to-PV piece mapping.
 */
#include "winlvm.h"

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

        KEVENT ev;
        IO_STATUS_BLOCK iosb;
        KeInitializeEvent(&ev, NotificationEvent, FALSE);
        RtlZeroMemory(&iosb, sizeof(iosb));
        PIRP irp = IoBuildDeviceIoControlRequest(
            IOCTL_STORAGE_QUERY_PROPERTY, disk_pdo, &q, sizeof(q),
            &d, sizeof(d), FALSE, &ev, &iosb);
        if (irp) {
            IoGetNextIrpStackLocation(irp)->FileObject = disk_file;
            NTSTATUS status = IoCallDriver(disk_pdo, irp);
            if (status == STATUS_PENDING) {
                KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
                status = iosb.Status;
            }
            if (NT_SUCCESS(status) &&
                d.BytesPerLogicalSector >= 512 && d.BytesPerLogicalSector <= 65536 &&
                (d.BytesPerLogicalSector & (d.BytesPerLogicalSector - 1)) == 0 &&
                d.BytesPerPhysicalSector >= d.BytesPerLogicalSector) {
                logical = d.BytesPerLogicalSector;
                physical = d.BytesPerPhysicalSector;
            }
        }
    }
    *logical_out = logical;
    *physical_out = physical;
}

BOOLEAN WinLvmQueryDiskWritable(PDEVICE_OBJECT disk_pdo, PFILE_OBJECT disk_file) {
    if (!disk_pdo || !disk_file) return FALSE;
    KEVENT ev;
    IO_STATUS_BLOCK iosb;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    RtlZeroMemory(&iosb, sizeof(iosb));

    PIRP irp = IoBuildDeviceIoControlRequest(
        IOCTL_DISK_IS_WRITABLE, disk_pdo, NULL, 0, NULL, 0, FALSE, &ev, &iosb);
    if (!irp) return TRUE;
    IoGetNextIrpStackLocation(irp)->FileObject = disk_file;
    NTSTATUS status = IoCallDriver(disk_pdo, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
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
    KEVENT ev;
    IO_STATUS_BLOCK iosb;
    RtlZeroMemory(&len_info, sizeof(len_info));
    RtlZeroMemory(&iosb, sizeof(iosb));
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    PIRP len_irp = IoBuildDeviceIoControlRequest(
        IOCTL_DISK_GET_LENGTH_INFO, disk_pdo, NULL, 0,
        &len_info, sizeof(len_info), FALSE, &ev, &iosb);
    if (len_irp) {
        IoGetNextIrpStackLocation(len_irp)->FileObject = disk_file;
        NTSTATUS st_len = IoCallDriver(disk_pdo, len_irp);
        if (st_len == STATUS_PENDING) {
            KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
            st_len = iosb.Status;
        }
        if (NT_SUCCESS(st_len) && len_info.Length.QuadPart > 0) {
            disk_len = (uint64_t)len_info.Length.QuadPart;
        }
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
        return STATUS_NOT_FOUND;
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
