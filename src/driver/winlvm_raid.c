/*
 * winlvm_raid.c — Portable raid1/mirror adapter, unified complex-LV dispatcher,
 *                 and WDM PASSIVE_LEVEL work-item queue (WinLvmComplexIoDispatch).
 */
#ifdef _KERNEL_MODE
#include "winlvm.h"
#else
#include "winlvm_vg.h"
#endif
#include "lvmcore/raid.h"
#include "lvmcore/thin.h"

static int raid_add_ovf(uint64_t a, uint64_t b, uint64_t *out) {
    *out = a + b;
    return *out < a;
}

static int raid_mul_ovf(uint64_t a, uint64_t b, uint64_t *out) {
    if (b != 0 && a > UINT64_MAX / b) return 1;
    *out = a * b;
    return 0;
}

static lvmcore_status raid_run_io(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                                  size_t lv_index, const wl_pv_io_ops *ops,
                                  uint64_t offset, void *dst, const void *src,
                                  size_t len, int is_write) {
    if (!vg || !rec || !ops || lv_index >= vg->lv_count ||
        (is_write ? (!src && len > 0) : (!dst && len > 0))) {
        return LVMCORE_ERR_INVALID_ARG;
    }
    const lvmcore_logical_volume *lv = &vg->lvs[lv_index];
    if (lv->segment_count == 0 || !lv->segments) return LVMCORE_ERR_UNSUPPORTED;
    if (len == 0) return LVMCORE_OK;

    uint64_t lv_sz = wl_lv_byte_size(vg, lv_index);
    if (lv_sz == 0 || offset > lv_sz || (uint64_t)len > lv_sz - offset) {
        return LVMCORE_ERR_INVALID_ARG;
    }

    uint64_t ext_bytes = 0;
    if (raid_mul_ovf(vg->extent_size_sectors, 512ULL, &ext_bytes) || ext_bytes == 0) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }

    uint64_t cur = offset;
    size_t done = 0;
    while (done < len) {
        size_t s = 0;
        uint64_t seg_off = 0, seg_bytes = 0;
        for (; s < lv->segment_count; s++) {
            uint64_t sb = 0, sl = 0, se = 0;
            if (raid_mul_ovf(lv->segments[s].start_extent, ext_bytes, &sb) ||
                raid_mul_ovf(lv->segments[s].extent_count, ext_bytes, &sl) ||
                raid_add_ovf(sb, sl, &se)) {
                return LVMCORE_ERR_OVERFLOW;
            }
            if (cur >= sb && cur < se) {
                seg_off = cur - sb;
                seg_bytes = sl;
                break;
            }
        }
        if (s >= lv->segment_count) return LVMCORE_ERR_INVALID_ARG;

        const lvmcore_segment *seg = &lv->segments[s];
        if ((seg->type != LVMCORE_SEG_RAID1 && seg->type != LVMCORE_SEG_MIRROR) ||
            seg->raid_leg_count == 0 || !seg->raid_legs) {
            return LVMCORE_ERR_UNSUPPORTED;
        }

        uint64_t avail = seg_bytes - seg_off;
        size_t step = ((uint64_t)(len - done) < avail) ? (len - done) : (size_t)avail;

        if (is_write) {
            /* Pass 1: fail closed before writing any byte if any leg is missing or OOB. */
            for (size_t l = 0; l < seg->raid_leg_count; l++) {
                int ii = wl_find_lv_index(vg, seg->raid_legs[l].image_lv);
                if (ii < 0 || !wl_sublv_pv_complete(rec, (size_t)ii)) return LVMCORE_ERR_IO;
                uint64_t leg_base = 0, leg_off = 0;
                uint64_t leg_sz = wl_lv_byte_size(vg, (size_t)ii);
                if (raid_mul_ovf(seg->raid_legs[l].start_extent, ext_bytes, &leg_base) ||
                    raid_add_ovf(leg_base, seg_off, &leg_off) ||
                    leg_off > leg_sz || (uint64_t)step > leg_sz - leg_off) {
                    return LVMCORE_ERR_IO;
                }
            }
            /* Pass 2: mirror write identically across all legs with O(1) stack. */
            for (size_t l = 0; l < seg->raid_leg_count; l++) {
                int ii = wl_find_lv_index(vg, seg->raid_legs[l].image_lv);
                uint64_t leg_off = seg->raid_legs[l].start_extent * ext_bytes + seg_off;
                if (wl_sublv_write(vg, rec, (size_t)ii, ops, leg_off,
                                   (const uint8_t *)src + done, step) != 0) {
                    return LVMCORE_ERR_IO;
                }
            }
        } else {
            /* Read from first healthy present leg with automatic failover on I/O error. */
            int ok = 0;
            for (size_t l = 0; l < seg->raid_leg_count; l++) {
                int ii = wl_find_lv_index(vg, seg->raid_legs[l].image_lv);
                if (ii < 0 || !wl_sublv_pv_complete(rec, (size_t)ii)) continue;
                uint64_t leg_base = 0, leg_off = 0;
                uint64_t leg_sz = wl_lv_byte_size(vg, (size_t)ii);
                if (raid_mul_ovf(seg->raid_legs[l].start_extent, ext_bytes, &leg_base) ||
                    raid_add_ovf(leg_base, seg_off, &leg_off) ||
                    leg_off > leg_sz || (uint64_t)step > leg_sz - leg_off) {
                    continue;
                }
                if (wl_sublv_read(vg, rec, (size_t)ii, ops, leg_off,
                                  (uint8_t *)dst + done, step) == 0) {
                    ok = 1;
                    break;
                }
            }
            if (!ok) return LVMCORE_ERR_IO;
        }

        cur += step;
        done += step;
    }
    return LVMCORE_OK;
}

lvmcore_status wl_raid_lv_read(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                               size_t lv_index, const wl_pv_io_ops *ops,
                               uint64_t offset, void *dst, size_t len) {
    return raid_run_io(vg, rec, lv_index, ops, offset, dst, NULL, len, 0);
}

lvmcore_status wl_raid_lv_write(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                                size_t lv_index, const wl_pv_io_ops *ops,
                                uint64_t offset, const void *src, size_t len) {
    return raid_run_io(vg, rec, lv_index, ops, offset, NULL, src, len, 1);
}

lvmcore_status wl_complex_lv_read(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                                  size_t lv_index, const wl_pv_io_ops *ops,
                                  uint8_t *meta_scratch, size_t meta_scratch_cap,
                                  uint8_t *aux_buf, size_t aux_buf_cap,
                                  uint64_t offset, void *dst, size_t len) {
    if (!vg || !rec || !ops || lv_index >= vg->lv_count) return LVMCORE_ERR_INVALID_ARG;
    const lvmcore_logical_volume *lv = &vg->lvs[lv_index];
    if (lv->is_snapshot_cow ||
        (lv->segment_count > 0 && lv->segments[0].type == LVMCORE_SEG_SNAPSHOT)) {
        return wl_snap_lv_read(vg, rec, lv_index, ops, aux_buf, aux_buf_cap, offset, dst, len);
    }
    if (lv->is_snapshot_origin) {
        uint64_t lv_sz = wl_lv_byte_size(vg, lv_index);
        if (lv_sz == 0 || offset > lv_sz || (uint64_t)len > lv_sz - offset) {
            return LVMCORE_ERR_INVALID_ARG;
        }
        return (wl_sublv_read(vg, rec, lv_index, ops, offset, dst, len) == 0)
                   ? LVMCORE_OK : LVMCORE_ERR_IO;
    }
    if (lv->segment_count == 0 || !lv->segments) return LVMCORE_ERR_UNSUPPORTED;
    lvmcore_seg_type t = lv->segments[0].type;
    if (t == LVMCORE_SEG_THIN) {
        return wl_thin_lv_read(vg, rec, lv_index, ops, meta_scratch, meta_scratch_cap,
                               offset, dst, len);
    }
    if (t == LVMCORE_SEG_RAID1 || t == LVMCORE_SEG_MIRROR) {
        return wl_raid_lv_read(vg, rec, lv_index, ops, offset, dst, len);
    }
    return LVMCORE_ERR_UNSUPPORTED;
}

lvmcore_status wl_complex_lv_write(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                                   size_t lv_index, const wl_pv_io_ops *ops,
                                   uint8_t *meta_scratch, size_t meta_scratch_cap,
                                   uint8_t *chunk_buf, uint8_t *aux_buf, size_t buf_cap,
                                   uint64_t offset, const void *src, size_t len) {
    if (!vg || !rec || !ops || lv_index >= vg->lv_count) return LVMCORE_ERR_INVALID_ARG;
    const lvmcore_logical_volume *lv = &vg->lvs[lv_index];
    if (lv->is_snapshot_cow ||
        (lv->segment_count > 0 && lv->segments[0].type == LVMCORE_SEG_SNAPSHOT)) {
        return wl_snap_lv_write(vg, rec, lv_index, ops, chunk_buf, aux_buf, buf_cap,
                                offset, src, len);
    }
    if (lv->is_snapshot_origin) {
        return wl_snap_origin_lv_write(vg, rec, lv_index, ops, chunk_buf, aux_buf, buf_cap,
                                       offset, src, len);
    }
    if (lv->segment_count == 0 || !lv->segments) return LVMCORE_ERR_UNSUPPORTED;
    lvmcore_seg_type t = lv->segments[0].type;
    if (t == LVMCORE_SEG_THIN) {
        return wl_thin_lv_write(vg, rec, lv_index, ops, meta_scratch, meta_scratch_cap,
                                chunk_buf, buf_cap, offset, src, len);
    }
    if (t == LVMCORE_SEG_RAID1 || t == LVMCORE_SEG_MIRROR) {
        return wl_raid_lv_write(vg, rec, lv_index, ops, offset, src, len);
    }
    return LVMCORE_ERR_UNSUPPORTED;
}

#ifdef _KERNEL_MODE
#ifdef _MSC_VER
#define WL_COMPLEX_POOL_NX NonPagedPoolNx
#define WL_MDL_NOEXEC      MdlMappingNoExecute
#else
#define WL_COMPLEX_POOL_NX ((POOL_TYPE)512)
#define WL_MDL_NOEXEC      0x40000000u
#endif

static volatile LONG g_complex_lock_state = 0;
static KEVENT        g_complex_write_lock;

static void WinLvmComplexLockAcquire(void) {
    if (InterlockedCompareExchange(&g_complex_lock_state, 1, 0) == 0) {
        KeInitializeEvent(&g_complex_write_lock, SynchronizationEvent, TRUE);
        InterlockedExchange(&g_complex_lock_state, 2);
    } else {
        while (InterlockedCompareExchange(&g_complex_lock_state, 2, 2) != 2) {
            YieldProcessor();
        }
    }
    KeWaitForSingleObject(&g_complex_write_lock, Executive, KernelMode, FALSE, NULL);
}

static void WinLvmComplexLockRelease(void) {
    KeSetEvent(&g_complex_write_lock, IO_NO_INCREMENT, FALSE);
}

static size_t wl_checked_chunk_bytes(uint64_t sectors, size_t want) {
    const uint64_t max_bytes = 16ULL * 1024ULL * 1024ULL;
    uint64_t cb = 0;
    if (raid_mul_ovf(sectors, 512ULL, &cb)) return want;
    if (cb > want && cb <= max_bytes) return (size_t)cb;
    return want;
}

static int raid_str_eq(const char *a, const char *b, size_t cap) {
    for (size_t i = 0; i < cap; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

static size_t WinLvmComplexChunkScratchBytes(const lvmcore_volume_group *vg, size_t lv_index) {
    size_t want = 65536u;
    if (!vg || !vg->lvs || lv_index >= vg->lv_count) return want;
    const lvmcore_logical_volume *lv = &vg->lvs[lv_index];
    if (lv->segment_count > 0 && lv->segments[0].type == LVMCORE_SEG_THIN) {
        int pi = wl_find_lv_index(vg, lv->segments[0].pool_data);
        if (pi >= 0 && vg->lvs[pi].segment_count > 0) {
            want = wl_checked_chunk_bytes(vg->lvs[pi].segments[0].chunk_size_sectors, want);
        }
    } else if (lv->segment_count > 0 && lv->segments[0].type == LVMCORE_SEG_SNAPSHOT) {
        want = wl_checked_chunk_bytes(lv->segments[0].chunk_size_sectors, want);
    } else if (lv->is_snapshot_cow && lv->snapshot_wrapper[0]) {
        int wi = wl_find_lv_index(vg, lv->snapshot_wrapper);
        if (wi >= 0 && vg->lvs[wi].segment_count > 0) {
            want = wl_checked_chunk_bytes(vg->lvs[wi].segments[0].chunk_size_sectors, want);
        }
    } else if (lv->is_snapshot_origin) {
        for (size_t i = 0; i < vg->lv_count; i++) {
            const lvmcore_logical_volume *cand = &vg->lvs[i];
            if (cand->segment_count == 0 || !cand->segments) continue;
            if (cand->segments[0].type != LVMCORE_SEG_SNAPSHOT) continue;
            if (!raid_str_eq(cand->segments[0].origin, lv->name, sizeof(lv->name))) continue;
            want = wl_checked_chunk_bytes(cand->segments[0].chunk_size_sectors, want);
        }
    }
    return want;
}

typedef struct {
    PIO_WORKITEM   work_item;
    PDEVICE_OBJECT device;
    PIRP           irp;
    uint64_t       offset;
    uint32_t       length;
    BOOLEAN        is_write;
} WL_COMPLEX_IO_WORK;

#define WL_COMPLEX_POOL_TAG 0x434D5657UL /* "WVMC" */

static void *WinLvmComplexAllocZero(size_t sz) {
    void *p = ExAllocatePoolWithTag(WL_COMPLEX_POOL_NX, sz, WL_COMPLEX_POOL_TAG);
    if (p) RtlZeroMemory(p, sz);
    return p;
}

static BOOLEAN WinLvmComplexIsReadOnly(const WINLVM_LV_EXT *ext,
                                       const lvmcore_volume_group *view) {
    if (!ext || !view) return TRUE;
    if (ext->read_only || view->read_only || view->system_id[0] != '\0') return TRUE;
    if (ext->lv_index < view->lv_count && view->lvs[ext->lv_index].read_only) return TRUE;
    return FALSE;
}

static VOID WinLvmComplexIoWorker(PDEVICE_OBJECT DeviceObject, PVOID Context) {
    UNREFERENCED_PARAMETER(DeviceObject);
    WL_COMPLEX_IO_WORK *w = (WL_COMPLEX_IO_WORK *)Context;
    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)w->device->DeviceExtension;
    PIRP irp = w->irp;
    NTSTATUS final_st = STATUS_DEVICE_NOT_CONNECTED;
    ULONG_PTR info = 0;

    const lvmcore_volume_group *view =
        (ext->record && !ext->record->dead && !ext->unavailable && !ext->deactivating)
            ? ext->record->view : NULL;
    PVOID sys_va = irp->MdlAddress
                       ? MmGetSystemAddressForMdlSafe(
                             irp->MdlAddress,
                             (MM_PAGE_PRIORITY)(NormalPagePriority | WL_MDL_NOEXEC))
                       : NULL;
    if (view && ext->lv_index < view->lv_count && w->is_write &&
        WinLvmComplexIsReadOnly(ext, view)) {
        final_st = STATUS_MEDIA_WRITE_PROTECTED;
    } else if (view && ext->lv_index < view->lv_count && sys_va) {
        size_t chunk_cap = WinLvmComplexChunkScratchBytes(view, ext->lv_index);
        uint8_t *meta_buf = (uint8_t *)WinLvmComplexAllocZero(LVMCORE_THIN_SCRATCH_META_BYTES);
        uint8_t *chunk_buf = (uint8_t *)WinLvmComplexAllocZero(chunk_cap);
        uint8_t *aux_buf = (uint8_t *)WinLvmComplexAllocZero(chunk_cap);
        if (!meta_buf || !chunk_buf || !aux_buf) {
            final_st = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            wl_pv_io_ops kops;
            WinLvmInitKernelPvOps(&kops, ext);
            lvmcore_status st;
            WinLvmComplexLockAcquire();
            if (w->is_write) {
                st = wl_complex_lv_write(view, ext->record, ext->lv_index, &kops,
                                         meta_buf, LVMCORE_THIN_SCRATCH_META_BYTES,
                                         chunk_buf, aux_buf, chunk_cap,
                                         w->offset, sys_va, w->length);
            } else {
                st = wl_complex_lv_read(view, ext->record, ext->lv_index, &kops,
                                        meta_buf, LVMCORE_THIN_SCRATCH_META_BYTES,
                                        aux_buf, chunk_cap,
                                        w->offset, sys_va, w->length);
            }
            WinLvmComplexLockRelease();
            if (st == LVMCORE_OK) {
                final_st = STATUS_SUCCESS;
                info = w->length;
            } else if (st == LVMCORE_ERR_CORRUPT_METADATA) {
                final_st = STATUS_DATA_ERROR;
            } else if (st == LVMCORE_ERR_INVALID_ARG) {
                final_st = STATUS_INVALID_PARAMETER;
            } else {
                final_st = STATUS_IO_DEVICE_ERROR;
            }
        }
        if (meta_buf) ExFreePoolWithTag(meta_buf, WL_COMPLEX_POOL_TAG);
        if (chunk_buf) ExFreePoolWithTag(chunk_buf, WL_COMPLEX_POOL_TAG);
        if (aux_buf) ExFreePoolWithTag(aux_buf, WL_COMPLEX_POOL_TAG);
    } else if (!sys_va) {
        final_st = STATUS_INSUFFICIENT_RESOURCES;
    }

    IoFreeWorkItem(w->work_item);
    ExFreePoolWithTag(w, WL_COMPLEX_POOL_TAG);

    irp->IoStatus.Status = final_st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    WinLvmIoGateLeave(&ext->io_gate);
}

static NTSTATUS WinLvmComplexCompleteNow(WINLVM_LV_EXT *ext, PIRP Irp, NTSTATUS st) {
    Irp->IoStatus.Status = st;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    WinLvmIoGateLeave(&ext->io_gate);
    return st;
}

NTSTATUS WinLvmComplexIoDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    WINLVM_LV_EXT *ext = (WINLVM_LV_EXT *)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    BOOLEAN is_write = (irpSp->MajorFunction == IRP_MJ_WRITE) ? TRUE : FALSE;
    LONGLONG raw_off = is_write ? irpSp->Parameters.Write.ByteOffset.QuadPart
                                : irpSp->Parameters.Read.ByteOffset.QuadPart;
    ULONG io_len = is_write ? irpSp->Parameters.Write.Length
                            : irpSp->Parameters.Read.Length;

    if (!ext->record || ext->record->dead || ext->unavailable || ext->deactivating ||
        !ext->record->view || ext->lv_index >= ext->record->view->lv_count) {
        return WinLvmComplexCompleteNow(ext, Irp, STATUS_DEVICE_NOT_CONNECTED);
    }
    const lvmcore_volume_group *view = ext->record->view;
    if (is_write && WinLvmComplexIsReadOnly(ext, view)) {
        return WinLvmComplexCompleteNow(ext, Irp, STATUS_MEDIA_WRITE_PROTECTED);
    }
    if (io_len == 0) {
        return WinLvmComplexCompleteNow(ext, Irp, STATUS_SUCCESS);
    }
    if (raw_off < 0 || !Irp->MdlAddress || MmGetMdlByteCount(Irp->MdlAddress) < io_len) {
        return WinLvmComplexCompleteNow(ext, Irp, STATUS_INVALID_PARAMETER);
    }
    if ((ULONGLONG)raw_off >= ext->byte_size) {
        return WinLvmComplexCompleteNow(ext, Irp, STATUS_END_OF_FILE);
    }
    ULONGLONG avail = ext->byte_size - (ULONGLONG)raw_off;
    if ((ULONGLONG)io_len > avail) {
        io_len = (ULONG)avail;
    }

    PIO_WORKITEM wi = IoAllocateWorkItem(DeviceObject);
    WL_COMPLEX_IO_WORK *w = (WL_COMPLEX_IO_WORK *)WinLvmComplexAllocZero(sizeof(WL_COMPLEX_IO_WORK));
    if (!wi || !w) {
        if (wi) IoFreeWorkItem(wi);
        if (w) ExFreePoolWithTag(w, WL_COMPLEX_POOL_TAG);
        return WinLvmComplexCompleteNow(ext, Irp, STATUS_INSUFFICIENT_RESOURCES);
    }

    w->work_item = wi;
    w->device = DeviceObject;
    w->irp = Irp;
    w->offset = (uint64_t)raw_off;
    w->length = io_len;
    w->is_write = is_write;

    IoMarkIrpPending(Irp);
    IoQueueWorkItem(wi, WinLvmComplexIoWorker, DelayedWorkQueue, w);
    return STATUS_PENDING;
}
#endif /* _KERNEL_MODE */
