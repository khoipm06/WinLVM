/*
 * winlvm_vg.h — Multi-VG registry, PV arrival state machine, extent mapper,
 *               and portable complex-segment I/O dispatcher (freestanding C11).
 *
 * Compiled into both winlvm.sys and host unit tests without WDM or CRT
 * dependencies (except the _KERNEL_MODE WDM work-item dispatch declaration).
 * In the kernel driver all registry mutations run under the bus FDO spinlock;
 * view arenas and per-PV backing tags are owned by the caller.
 */
#ifndef WINLVM_DRIVER_WINLVM_VG_H
#define WINLVM_DRIVER_WINLVM_VG_H

#include "lvmcore/types.h"
#include "lvmcore/parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WL_VG_MAX      8
#define WL_VG_MAX_PVS  8
#define WL_VG_MAX_LVS  64
#define WL_VG_UUID_LEN 33 /* 32 raw LVM2 UUID chars + NUL */
#define WL_VG_NAME_LEN 64

typedef enum {
    WL_PV_MISSING = 0, /* declared in VG metadata, backing disk not yet arrived */
    WL_PV_PRESENT = 1, /* backing disk arrived and parsed */
    WL_PV_GONE    = 2  /* backing disk removed (eligible for re-insertion) */
} wl_pv_state;

typedef enum {
    WL_VG_INCOMPLETE = 0, /* at least one declared PV is not present */
    WL_VG_COMPLETE   = 1  /* all declared PVs are present */
} wl_vg_state;

typedef enum {
    WL_ARR_REJECTED    = 0, /* inconsistent or unparseable metadata */
    WL_ARR_DUPLICATE   = 1, /* same PV UUID already present at >= seqno */
    WL_ARR_PENDING     = 2, /* accepted; other declared PVs still missing */
    WL_ARR_COMPLETED   = 3, /* accepted; VG is now complete */
    WL_ARR_STALE_SEQNO = 4, /* higher seqno seen; old view kept because seam refused */
    WL_ARR_RECONCILED  = 5  /* higher seqno accepted; view swapped to newer metadata */
} wl_arrival_result;

typedef enum {
    WL_UPDATE_UNAVAILABLE = 0,
    WL_UPDATE_OK          = 1
} wl_update_result;

typedef wl_update_result (*wl_lv_update_fn)(void *ctx, const char *lv_uuid);

typedef struct wl_vg_record {
    char vg_uuid[WL_VG_UUID_LEN];
    char vg_name[WL_VG_NAME_LEN];
    uint64_t seqno;
    wl_vg_state state;
    int stale;
    int dead;
    unsigned refs;
    size_t pv_count;
    char pv_uuid[WL_VG_MAX_PVS][WL_VG_UUID_LEN];
    wl_pv_state pv_state[WL_VG_MAX_PVS];
    const void *pv_tag[WL_VG_MAX_PVS];
    const lvmcore_volume_group *view;
    unsigned char lv_activated[(WL_VG_MAX_LVS + 7) / 8];
} wl_vg_record;

typedef struct {
    uint32_t pv_index;
    uint64_t pv_byte;
    uint64_t len;
} wl_io_chunk;

typedef struct wl_map_iter {
    const lvmcore_volume_group *vg;
    const wl_vg_record *rec;
    size_t lv_index;
    uint64_t remaining;
    size_t seg;
    uint64_t seg_pos;
    int err;
} wl_map_iter;

/* Abstract per-PV read/write callbacks keyed by PV slot (no raw pv_tag deref). */
typedef struct wl_pv_io_ops {
    void *ctx;
    int (*read)(void *ctx, uint32_t pv_slot, uint64_t offset, void *buf, size_t len);
    int (*write)(void *ctx, uint32_t pv_slot, uint64_t offset, const void *buf, size_t len);
} wl_pv_io_ops;

void wl_vg_registry_init(wl_vg_record *recs, size_t count);
int wl_vg_find(const wl_vg_record *recs, size_t count, const char *vg_uuid);
void wl_vg_ref(wl_vg_record *r);
void wl_vg_unref(wl_vg_record *r);

wl_arrival_result wl_vg_arrival(wl_vg_record *recs, size_t count,
                                size_t *slot_out,
                                const lvmcore_volume_group *view,
                                const char *pv_uuid,
                                const void *instance_tag,
                                wl_lv_update_fn seam, void *seam_ctx,
                                size_t *newly_out, size_t newly_cap,
                                size_t *newly_count);

void wl_vg_pv_removed(wl_vg_record *recs, size_t count, const char *pv_uuid);

int wl_vg_lv_pv_complete(const wl_vg_record *r, size_t lv_index);
int wl_vg_lv_all_gone(const wl_vg_record *r, size_t lv_index);
int wl_vg_lv_any_pv_gone(const wl_vg_record *r, size_t lv_index);
int wl_vg_lv_activated(const wl_vg_record *r, size_t lv_index);
void wl_vg_lv_set_activated(wl_vg_record *r, size_t lv_index, int on);
int wl_vg_lv_child_eligible(const lvmcore_logical_volume *lv);
int wl_vg_lv_is_complex(const lvmcore_volume_group *vg, size_t lv_index);
int wl_vg_lv_raid_all_legs_present(const wl_vg_record *r, size_t lv_index);
size_t wl_vg_lv_collect_pv_slots(const wl_vg_record *r, size_t lv_index,
                                 uint32_t *slots_out, size_t slots_cap);
uint64_t wl_lv_byte_size(const lvmcore_volume_group *vg, size_t lv_index);

/* Sub-LV status helpers (shared between winlvm_vg.c and winlvm_map.c). */
int wl_sublv_pv_complete(const wl_vg_record *r, size_t lv_index);
int wl_sublv_all_gone(const wl_vg_record *r, size_t lv_index);
int wl_sublv_any_pv_gone(const wl_vg_record *r, size_t lv_index);
int wl_find_lv_index(const lvmcore_volume_group *vg, const char *name);

/* Direct linear/striped mapping iterator (fails closed on internal/complex LVs). */
void wl_map_begin(wl_map_iter *it, const lvmcore_volume_group *vg,
                  const wl_vg_record *rec, size_t lv_index,
                  uint64_t lv_byte, uint64_t len);
/* Raw mapping iterator permitting internal linear/striped sub-LVs (_tmeta/_tdata/_cow/_rimage). */
void wl_map_begin_raw(wl_map_iter *it, const lvmcore_volume_group *vg,
                      const wl_vg_record *rec, size_t lv_index,
                      uint64_t lv_byte, uint64_t len);
int wl_map_next(wl_map_iter *it, uint32_t *pv_index, uint64_t *pv_byte,
                uint64_t *piece_len);

/* Linear/striped sub-LV read/write via wl_map_begin_raw + wl_pv_io_ops. */
int wl_sublv_read(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                  size_t lv_index, const wl_pv_io_ops *ops,
                  uint64_t offset, void *buf, size_t len);
int wl_sublv_write(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                   size_t lv_index, const wl_pv_io_ops *ops,
                   uint64_t offset, const void *buf, size_t len);

/* Portable freestanding complex-segment I/O engines. */
lvmcore_status wl_thin_lv_read(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                               size_t lv_index, const wl_pv_io_ops *ops,
                               uint8_t *meta_scratch, size_t meta_scratch_cap,
                               uint64_t offset, void *dst, size_t len);
lvmcore_status wl_thin_lv_write(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                                size_t lv_index, const wl_pv_io_ops *ops,
                                uint8_t *meta_scratch, size_t meta_scratch_cap,
                                uint8_t *chunk_buf, size_t chunk_buf_cap,
                                uint64_t offset, const void *src, size_t len);

lvmcore_status wl_snap_lv_read(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                               size_t lv_index, const wl_pv_io_ops *ops,
                               uint8_t *table_buf, size_t table_buf_cap,
                               uint64_t offset, void *dst, size_t len);
lvmcore_status wl_snap_lv_write(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                                size_t lv_index, const wl_pv_io_ops *ops,
                                uint8_t *chunk_buf, uint8_t *table_buf, size_t buf_cap,
                                uint64_t offset, const void *src, size_t len);
lvmcore_status wl_snap_origin_lv_write(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                                       size_t lv_index, const wl_pv_io_ops *ops,
                                       uint8_t *chunk_buf, uint8_t *table_buf, size_t buf_cap,
                                       uint64_t offset, const void *src, size_t len);

lvmcore_status wl_raid_lv_read(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                               size_t lv_index, const wl_pv_io_ops *ops,
                               uint64_t offset, void *dst, size_t len);
lvmcore_status wl_raid_lv_write(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                                size_t lv_index, const wl_pv_io_ops *ops,
                                uint64_t offset, const void *src, size_t len);

/* Unified complex LV dispatcher (handles thin, snapshot, snapshot-origin, raid1, mirror). */
lvmcore_status wl_complex_lv_read(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                                  size_t lv_index, const wl_pv_io_ops *ops,
                                  uint8_t *meta_scratch, size_t meta_scratch_cap,
                                  uint8_t *aux_buf, size_t aux_buf_cap,
                                  uint64_t offset, void *dst, size_t len);
lvmcore_status wl_complex_lv_write(const lvmcore_volume_group *vg, const wl_vg_record *rec,
                                   size_t lv_index, const wl_pv_io_ops *ops,
                                   uint8_t *meta_scratch, size_t meta_scratch_cap,
                                   uint8_t *chunk_buf, uint8_t *aux_buf, size_t buf_cap,
                                   uint64_t offset, const void *src, size_t len);

#if defined(_KERNEL_MODE) && (defined(_NTDDK_) || defined(_WDMDDK_) || defined(WINLVM_DRIVER_WINLVM_H))
/* Bind kernel PV I/O callbacks to the pinned WINLVM_LV_EXT backing tables. */
void WinLvmInitKernelPvOps(wl_pv_io_ops *ops, void *lv_ext);
/* WDM PASSIVE_LEVEL work-item dispatcher for thin/snapshot/raid1/mirror IRPs. */
NTSTATUS WinLvmComplexIoDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp);
#endif

#ifdef __cplusplus
}
#endif

#endif /* WINLVM_DRIVER_WINLVM_VG_H */
