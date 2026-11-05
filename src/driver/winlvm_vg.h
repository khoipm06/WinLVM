/*
 * winlvm_vg.h — Multi-VG registry and PV arrival state machine (freestanding C11).
 *
 * Compiled into both winlvm.sys and host unit tests without WDM or CRT
 * dependencies. In the kernel driver all mutations run under the bus FDO
 * spinlock; view arenas and per-PV backing tags are owned by the caller.
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
#define WL_VG_MAX_LVS  16
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
uint64_t wl_lv_byte_size(const lvmcore_volume_group *vg, size_t lv_index);

void wl_map_begin(wl_map_iter *it, const lvmcore_volume_group *vg,
                  const wl_vg_record *rec, size_t lv_index,
                  uint64_t lv_byte, uint64_t len);
int wl_map_next(wl_map_iter *it, uint32_t *pv_index, uint64_t *pv_byte,
                uint64_t *piece_len);

#ifdef __cplusplus
}
#endif

#endif /* WINLVM_DRIVER_WINLVM_VG_H */
