/*
 * winlvm_thin.c — Portable dm-thin LV adapter + WDM PASSIVE_LEVEL PV I/O helpers.
 *
 * Bridges VG/LV metadata to the freestanding lvmcore_thin engine so both host
 * differential tests and winlvm.sys execute identical on-disk block logic.
 */
#ifdef _KERNEL_MODE
#include "winlvm.h"
#else
#include "winlvm_vg.h"
#endif
#include "lvmcore/thin.h"

typedef struct {
    const lvmcore_volume_group *vg;
    const wl_vg_record         *rec;
    size_t                      lv_index;
    const wl_pv_io_ops         *ops;
} wl_sublv_adapter;

static int sublv_adapter_read(void *impl, uint64_t off, void *buf, size_t len) {
    wl_sublv_adapter *a = (wl_sublv_adapter *)impl;
    return wl_sublv_read(a->vg, a->rec, a->lv_index, a->ops, off, buf, len);
}

static int sublv_adapter_write(void *impl, uint64_t off, const void *buf, size_t len) {
    wl_sublv_adapter *a = (wl_sublv_adapter *)impl;
    return wl_sublv_write(a->vg, a->rec, a->lv_index, a->ops, off, buf, len);
}

static uint64_t sublv_raw_size(const lvmcore_volume_group *vg, size_t lv_index) {
    if (!vg || !vg->lvs || lv_index >= vg->lv_count) return 0;
    uint64_t exts = lvmcore_lv_extent_count(&vg->lvs[lv_index]);
    if (exts == UINT64_MAX || vg->extent_size_sectors == 0) return 0;
    if (exts > UINT64_MAX / vg->extent_size_sectors) return 0;
    uint64_t secs = exts * vg->extent_size_sectors;
    if (secs > UINT64_MAX / 512ULL) return 0;
    return secs * 512ULL;
}

static lvmcore_status resolve_thin_binding(const lvmcore_volume_group *vg,
                                           size_t lv_index,
                                           size_t *tmeta_idx,
                                           size_t *tdata_idx,
                                           uint64_t *dev_id_out,
                                           int *zero_new_out) {
    if (!vg || !vg->lvs || lv_index >= vg->lv_count) return LVMCORE_ERR_INVALID_ARG;
    const lvmcore_logical_volume *lv = &vg->lvs[lv_index];
    if (lv->segment_count == 0 || !lv->segments || lv->segments[0].type != LVMCORE_SEG_THIN ||
        !lv->segments[0].has_device_id) {
        return LVMCORE_ERR_UNSUPPORTED;
    }
    int pi = wl_find_lv_index(vg, lv->segments[0].pool_data);
    if (pi < 0) return LVMCORE_ERR_CORRUPT_METADATA;
    const lvmcore_logical_volume *pool = &vg->lvs[pi];
    if (pool->segment_count == 0 || !pool->segments ||
        pool->segments[0].type != LVMCORE_SEG_THIN_POOL) {
        return LVMCORE_ERR_CORRUPT_METADATA;
    }
    int tm = wl_find_lv_index(vg, pool->segments[0].pool_metadata);
    int td = wl_find_lv_index(vg, pool->segments[0].pool_data);
    if (tm < 0 || td < 0) return LVMCORE_ERR_CORRUPT_METADATA;

    *tmeta_idx = (size_t)tm;
    *tdata_idx = (size_t)td;
    *dev_id_out = lv->segments[0].device_id;
    *zero_new_out = pool->segments[0].has_zero_new_blocks ? pool->segments[0].zero_new_blocks : 1;
    return LVMCORE_OK;
}

lvmcore_status wl_thin_lv_read(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                               size_t lv_index, const wl_pv_io_ops *ops,
                               uint8_t *meta_scratch, size_t meta_scratch_cap,
                               uint64_t offset, void *dst, size_t len) {
    size_t tm = 0, td = 0;
    uint64_t dev_id = 0;
    int zero_new = 1;
    lvmcore_status st = resolve_thin_binding(vg, lv_index, &tm, &td, &dev_id, &zero_new);
    if (st != LVMCORE_OK) return st;

    uint64_t lv_sz = wl_lv_byte_size(vg, lv_index);
    if (lv_sz == 0 || offset > lv_sz || (uint64_t)len > lv_sz - offset) {
        return LVMCORE_ERR_INVALID_ARG;
    }

    wl_sublv_adapter m_ad = { vg, rec, tm, ops };
    wl_sublv_adapter d_ad = { vg, rec, td, ops };
    lvmcore_rw_source m_src = { &m_ad, sublv_raw_size(vg, tm), sublv_adapter_read, NULL };
    lvmcore_rw_source d_src = { &d_ad, sublv_raw_size(vg, td), sublv_adapter_read, NULL };

    return lvmcore_thin_read(&m_src, &d_src, dev_id, meta_scratch, meta_scratch_cap,
                             offset, dst, len);
}

lvmcore_status wl_thin_lv_write(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                                size_t lv_index, const wl_pv_io_ops *ops,
                                uint8_t *meta_scratch, size_t meta_scratch_cap,
                                uint8_t *chunk_buf, size_t chunk_buf_cap,
                                uint64_t offset, const void *src, size_t len) {
    size_t tm = 0, td = 0;
    uint64_t dev_id = 0;
    int zero_new = 1;
    lvmcore_status st = resolve_thin_binding(vg, lv_index, &tm, &td, &dev_id, &zero_new);
    if (st != LVMCORE_OK) return st;

    uint64_t lv_sz = wl_lv_byte_size(vg, lv_index);
    if (lv_sz == 0 || offset > lv_sz || (uint64_t)len > lv_sz - offset) {
        return LVMCORE_ERR_INVALID_ARG;
    }

    wl_sublv_adapter m_ad = { vg, rec, tm, ops };
    wl_sublv_adapter d_ad = { vg, rec, td, ops };
    lvmcore_rw_source m_src = { &m_ad, sublv_raw_size(vg, tm), sublv_adapter_read, sublv_adapter_write };
    lvmcore_rw_source d_src = { &d_ad, sublv_raw_size(vg, td), sublv_adapter_read, sublv_adapter_write };

    return lvmcore_thin_write(&m_src, &d_src, dev_id, meta_scratch, meta_scratch_cap,
                              chunk_buf, chunk_buf_cap, zero_new, offset, src, len);
}

#ifdef _KERNEL_MODE
#define WL_THIN_POOL_TAG 0x544D5657UL /* "WVMT" */
#ifdef _MSC_VER
#define WL_THIN_POOL_NX NonPagedPoolNx
#else
#define WL_THIN_POOL_NX ((POOL_TYPE)512)
#endif

static int WinLvmFindBacking(const WINLVM_LV_EXT *ext, uint32_t pv_slot,
                             PDEVICE_OBJECT *pdo_out, PFILE_OBJECT *file_out,
                             ULONG *sec_out) {
    if (!ext) return 0;
    size_t n = ext->backing_count;
    if (n > WL_VG_MAX_PVS) n = WL_VG_MAX_PVS;
    for (size_t k = 0; k < n; k++) {
        if (ext->pv_slot[k] == pv_slot) {
            if (!ext->backing_pdo[k] || !ext->backing_file[k]) return 0;
            *pdo_out = ext->backing_pdo[k];
            *file_out = ext->backing_file[k];
            *sec_out = (ext->backing_logical[k] >= 512u) ? ext->backing_logical[k] : 512u;
            return 1;
        }
    }
    return 0;
}

/*
 * Synchronous sector-aligned backing-device read/write helpers at PASSIVE_LEVEL.
 * Uses only the LV PDO's io_gate-pinned backing_pdo/backing_file arrays (never
 * dereferencing bus-registry pv_tag pointers) and bounce-aligns sub-sector writes.
 */
static int WinLvmSyncPvRead(void *ctx, uint32_t pv_slot, uint64_t off, void *buf, size_t len) {
    const WINLVM_LV_EXT *ext = (const WINLVM_LV_EXT *)ctx;
    PDEVICE_OBJECT pdo = NULL;
    PFILE_OBJECT file = NULL;
    ULONG sec = 512u;
    if (!WinLvmFindBacking(ext, pv_slot, &pdo, &file, &sec) || (!buf && len > 0)) return -1;
    if (len == 0) return 0;

    WINLVM_DISK_SOURCE dsrc;
    dsrc.disk_pdo = pdo;
    dsrc.file = file;
    dsrc.sector_size = sec;
    return (winlvm_disk_source_read(&dsrc, off, buf, len) == len) ? 0 : -1;
}

static int WinLvmSyncPvWrite(void *ctx, uint32_t pv_slot, uint64_t off, const void *buf, size_t len) {
    const WINLVM_LV_EXT *ext = (const WINLVM_LV_EXT *)ctx;
    if (!ext || ext->read_only) return -1;
    PDEVICE_OBJECT pdo = NULL;
    PFILE_OBJECT file = NULL;
    ULONG sec_u = 512u;
    if (!WinLvmFindBacking(ext, pv_slot, &pdo, &file, &sec_u) || (!buf && len > 0)) return -1;
    if (len == 0) return 0;
    if (len > UINT64_MAX - off) return -1;

    uint64_t sec = (uint64_t)sec_u;
    uint64_t start = (off / sec) * sec;
    uint64_t raw_end = off + (uint64_t)len;
    if ((sec - 1ULL) > UINT64_MAX - raw_end) return -1;
    uint64_t end = ((raw_end + sec - 1ULL) / sec) * sec;
    if (end <= start || (end - start) > 0xFFFFFFFFULL) return -1;

    size_t window = (size_t)(end - start);
    size_t skip = (size_t)(off - start);
    unsigned char *bounce = (unsigned char *)ExAllocatePoolWithTag(
        WL_THIN_POOL_NX, window, WL_THIN_POOL_TAG);
    if (!bounce) return -1;
    RtlZeroMemory(bounce, window);

    if (skip > 0 || window > len) {
        if (WinLvmSyncPvRead(ctx, pv_slot, start, bounce, window) != 0) {
            ExFreePoolWithTag(bounce, WL_THIN_POOL_TAG);
            return -1;
        }
    }
    RtlCopyMemory(bounce + skip, buf, len);

    LARGE_INTEGER loff;
    loff.QuadPart = (LONGLONG)start;
    KEVENT ev;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    IO_STATUS_BLOCK iosb;
    RtlZeroMemory(&iosb, sizeof(iosb));

    PIRP irp = IoBuildSynchronousFsdRequest(
        IRP_MJ_WRITE, pdo, bounce, (ULONG)window, &loff, &ev, &iosb);
    if (!irp) {
        ExFreePoolWithTag(bounce, WL_THIN_POOL_TAG);
        return -1;
    }
    IoGetNextIrpStackLocation(irp)->FileObject = file;
    NTSTATUS st = IoCallDriver(pdo, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        st = iosb.Status;
    }
    ExFreePoolWithTag(bounce, WL_THIN_POOL_TAG);
    return NT_SUCCESS(st) ? 0 : -1;
}

void WinLvmInitKernelPvOps(wl_pv_io_ops *ops, void *lv_ext) {
    if (!ops) return;
    ops->ctx = lv_ext;
    ops->read = WinLvmSyncPvRead;
    ops->write = WinLvmSyncPvWrite;
}
#endif /* _KERNEL_MODE */
