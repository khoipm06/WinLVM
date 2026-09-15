/*
 * winlvm_vg.h — multi-VG registry + PV arrival state machine (host-clean core).
 *
 * W2 Task 8 core: every function here is freestanding C11, no WDM, no CRT —
 * compiled both into winlvm.sys (kernel glue lives in winlvm_bus.c) and the
 * host test tests/lvmcore/test_winlvm_vg.c. All state mutations are designed
 * to run under the bus FDO spinlock in the kernel; the host test runs them
 * single-threaded.
 *
 * Model (plan W2):
 *   - Registry: fixed array of records keyed by VG UUID. A record is created
 *     on the FIRST PV arrival that parses; later PVs of the same VG mark
 *     their slot. Records are NEVER freed by the arrival path (v22 UAF
 *     lesson: only the bus REMOVE sweep frees them).
 *   - Per-PV state: MISSING -> PRESENT, PRESENT -> GONE on removal,
 *     GONE -> PRESENT on re-insert (same record object, only the backing
 *     pointer is swapped by the glue — no free under live children).
 *   - Per-LV activation gating: an LV becomes activatable when ALL PVs
 *     referenced by ITS segments are present (fail-closed per-LV, not
 *     per-VG). wl_vg_arrival returns the list of LV indices that JUST
 *     became activatable; the glue creates child PDOs for exactly those.
 *   - Duplicate-arrival dedupe: same PV UUID arriving again (any instance)
 *     is a no-op (WL_ARR_DUPLICATE) — repeated arrivals must not re-create
 *     live children.
 *   - Seqno reconciliation on LATE arrival: a PV arriving after the record
 *     is active with a HIGHER seqno means the active map is stale. Policy:
 *     re-target from the max-seqno winner. If no live children pin the old
 *     view (refs == 0), the view swaps in place (WL_ARR_RECONCILED). With
 *     live children, every LV whose geometry changed must go through the
 *     UPDATE_LV seam; the seam is W3 and does not exist yet — the stub
 *     returns WL_UPDATE_UNAVAILABLE, the record is marked stale, and the
 *     OLD view keeps serving (never freed under live child PDOs).
 *
 * Ownership contract with the glue:
 *   - `view` points into an arena the GLUE owns. It is stored on the record
 *     only while accepted; on RECONCILED the glue replaces record->view with
 *     the new winner arena and frees the old one only when refs == 0.
 *   - `pv_tag` is an opaque glue instance tag (kernel: owner pointer of the
 *     per-PV disk-interface-name copy; host tests: any stable pointer).
 */
#ifndef WINLVM_DRIVER_WINLVM_VG_H
#define WINLVM_DRIVER_WINLVM_VG_H

#include <stddef.h>
#include <stdint.h>

#include "lvmcore/types.h"
#include "lvmcore/parser.h"

#define WL_VG_MAX        8
#define WL_VG_MAX_PVS    8
#define WL_VG_MAX_LVS    16
#define WL_VG_UUID_LEN   33 /* raw 32 chars + NUL (lvmcore convention) */
#define WL_VG_NAME_LEN   64

/* Per-PV arrival state. */
typedef enum {
    WL_PV_MISSING = 0, /* declared in metadata, disk not arrived yet */
    WL_PV_PRESENT = 1, /* backing disk arrived */
    WL_PV_GONE    = 2  /* backing disk removed (re-insert allowed) */
} wl_pv_state;

/* VG record state. Children are tracked separately: `refs` counts live child
 * PDOs and lv_activated[] marks LVs the glue already created children for. */
typedef enum {
    WL_VG_INCOMPLETE = 0, /* some declared PVs have not arrived */
    WL_VG_COMPLETE   = 1  /* all declared PVs present */
} wl_vg_state;

/* Outcome of wl_vg_arrival — drives both glue behavior and host tests. */
typedef enum {
    WL_ARR_REJECTED    = 0, /* unparseable/inconsistent — fail closed */
    WL_ARR_DUPLICATE   = 1, /* same PV UUID already present — no-op */
    WL_ARR_PENDING     = 2, /* accepted; other PVs still missing */
    WL_ARR_COMPLETED   = 3, /* accepted; this arrival completed the VG */
    WL_ARR_STALE_SEQNO = 4, /* higher seqno seen; old view kept (seam said
                             * unavailable) — record marked stale */
    WL_ARR_RECONCILED  = 5  /* higher seqno; view swapped to the winner */
} wl_arrival_result;

/* UPDATE_LV seam (W3). TODO-W3: not implemented — the kernel stub returns
 * WL_UPDATE_UNAVAILABLE so the driver keeps the old (stale) view instead of
 * faking success or tearing down live children. */
typedef enum {
    WL_UPDATE_UNAVAILABLE = 0,
    WL_UPDATE_OK          = 1
} wl_update_result;
typedef wl_update_result (*wl_lv_update_fn)(void *ctx, const char *lv_uuid);

typedef struct wl_vg_record {
    char vg_uuid[WL_VG_UUID_LEN]; /* key: VG UUID (view->id) */
    char vg_name[WL_VG_NAME_LEN];
    uint64_t seqno;              /* seqno of the pinned view */
    wl_vg_state state;
    int stale;                   /* 1: a higher-seqno view was seen but not
                                  * applied (old view still serving) */
    int dead;                    /* 1: ALL PVs gone — children fail closed */
    unsigned refs;               /* live child PDO count (glue-managed) */
    size_t pv_count;
    char pv_uuid[WL_VG_MAX_PVS][WL_VG_UUID_LEN];
    wl_pv_state pv_state[WL_VG_MAX_PVS];
    const void *pv_tag[WL_VG_MAX_PVS];
    const lvmcore_volume_group *view; /* pinned parsed view (arena-owned) */
    unsigned char lv_activated[(WL_VG_MAX_LVS + 7) / 8];
} wl_vg_record;

/* One contiguous piece of an LV byte range, on one PV. */
typedef struct {
    uint32_t pv_index; /* record PV slot */
    uint64_t pv_byte;  /* absolute byte offset on that PV's backing disk */
    uint64_t len;
} wl_io_chunk;

/* Streaming LV-byte -> (PV, byte, len) map. Pieces are cut at segment
 * boundaries and, inside striped segments, at stripe-chunk boundaries.
 * Linear segments are always one contiguous piece (LVM2 allocates the
 * extents of a linear segment contiguously on one PV). */
typedef struct wl_map_iter {
    const lvmcore_volume_group *vg;
    const wl_vg_record *rec;
    size_t lv_index;
    uint64_t remaining;  /* LV bytes left to map */
    size_t seg;          /* current segment index */
    uint64_t seg_pos;    /* byte position within current segment */
    int err;             /* 1 after a refusal — subsequent next() = -1 */
} wl_map_iter;

/* ---- registry ------------------------------------------------------ */

/* Zero every slot. Call once at AddDevice (kernel) / test setup (host). */
void wl_vg_registry_init(wl_vg_record *recs, size_t count);

/* Slot index of a live record with this VG UUID, or -1. */
int wl_vg_find(const wl_vg_record *recs, size_t count, const char *vg_uuid);

/* Child-PDO refcount (glue calls ref on PDO create, unref on child REMOVE). */
void wl_vg_ref(wl_vg_record *r);
void wl_vg_unref(wl_vg_record *r);

/*
 * PV arrival state machine — find-or-create + dedupe + per-LV activation
 * gating + seqno reconciliation. `view` is the parsed VG from the arriving
 * PV's metadata (glue-owned arena); `pv_uuid` the arriving PV's UUID (label
 * and metadata agree — lvmcore cross-checks); `instance_tag` the opaque
 * backing-disk tag.
 *
 * On success (PENDING/COMPLETED/RECONCILED/STALE_SEQNO/DUPLICATE) the slot is
 * returned in *slot_out. On PENDING/COMPLETED/RECONCILED, newly-activatable
 * LV indices (complete PV set, visible, mappable, no child yet) are written
 * to newly_out[] — the glue creates child PDOs for exactly this list and
 * marks them activated.
 *
 * On REJECTED the record was NOT created/modified and the caller still owns
 * the view (glue frees its arena + drops the backing refs).
 *
 * View ownership: on first-arrival create or RECONCILED swap, the record
 * takes `view`; the glue must free the OLD arena only when it swapped with
 * refs == 0 (the only path where a swap is committed).
 */
wl_arrival_result wl_vg_arrival(wl_vg_record *recs, size_t count,
                                size_t *slot_out,
                                const lvmcore_volume_group *view,
                                const char *pv_uuid,
                                const void *instance_tag,
                                wl_lv_update_fn seam, void *seam_ctx,
                                size_t *newly_out, size_t newly_cap,
                                size_t *newly_count);

/* PV removal: mark the slot GONE. When ALL PVs are gone the record goes
 * dead (children fail closed; glue re-reports relations so PnP removes
 * them — v22 SURPRISE_REMOVAL/SUCCESS contract). Unknown UUID: no-op. */
void wl_vg_pv_removed(wl_vg_record *recs, size_t count, const char *pv_uuid);

/* ---- per-LV queries ------------------------------------------------ */

/* 1 iff every PV referenced by this LV's segments is PRESENT. Fails closed
 * (0) on NULL view, non-visible, non-mappable, unknown PV key, overflow. */
int wl_vg_lv_pv_complete(const wl_vg_record *r, size_t lv_index);

/* 1 iff every PV referenced by this LV's segments is GONE (child should be
 * dropped by the next QDR cycle). Fails closed the other way: any PV still
 * PRESENT/MISSING keeps the child. */
int wl_vg_lv_all_gone(const wl_vg_record *r, size_t lv_index);

/* 1 iff the glue already created a child for this LV (idempotence guard for
 * repeated arrivals / re-invalidate). */
int wl_vg_lv_activated(const wl_vg_record *r, size_t lv_index);
void wl_vg_lv_set_activated(wl_vg_record *r, size_t lv_index, int on);

/* Child eligibility: visible AND mappable. Internal LVs (rimage/rmeta/…
 * even when mappable) must NOT become volumes. */
int wl_vg_lv_child_eligible(const lvmcore_logical_volume *lv);

/* Total byte size of an LV (sum of segment extents * extent size). 0 on
 * overflow — caller fails closed. */
uint64_t wl_lv_byte_size(const lvmcore_volume_group *vg, size_t lv_index);

/* ---- LV byte mapping ----------------------------------------------- */

/* Begin mapping [lv_byte, lv_byte + len) of lv_index. len == 0 or lv_byte
 * beyond the LV leaves the iterator exhausted (next() returns 0 at once). */
void wl_map_begin(wl_map_iter *it, const lvmcore_volume_group *vg,
                  const wl_vg_record *rec, size_t lv_index,
                  uint64_t lv_byte, uint64_t len);

/* Next contiguous piece. Returns 1 and fills the outputs, 0 at end of
 * range, -1 on refusal (non-mappable LV, unknown PV, math overflow —
 * fail closed: the WHOLE IRP must fail, never a partial map). */
int wl_map_next(wl_map_iter *it, uint32_t *pv_index, uint64_t *pv_byte,
                uint64_t *piece_len);

#endif /* WINLVM_DRIVER_WINLVM_VG_H */
