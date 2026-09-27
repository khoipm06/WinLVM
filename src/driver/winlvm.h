/*
 * winlvm.h — winlvm.sys internal types.
 *
 * Architecture mirrors maharmstone/winmd (pure C WDM):
 *   - The INF enumerates ROOT\winlvm; the PnP manager creates the root bus
 *     PDO and calls WinLvmAddDevice, which creates the bus FDO.
 *   - Disk-interface arrival notifications probe candidate PVs; a worker
 *     parses the LVM2 metadata and creates one child PDO per mappable LV.
 *   - Child PDOs are reported via IRP_MN_QUERY_DEVICE_RELATIONS/BusRelations
 *     and match the INF's WinLvmVolume hardware ID. They are FILE_DEVICE_DISK
 *     volumes: reads/writes are forwarded to the backing disk device object.
 *
 * NEVER ZwCreateFile: backing devices are addressed through device objects
 * we already hold (IoGetDeviceObjectPointer once per arrival, then
 * IoCallDriver).
 */
#ifndef WINLVM_DRIVER_WINLVM_H
#define WINLVM_DRIVER_WINLVM_H

#include <ntddk.h>
#include <wdm.h>
#include <wmilib.h>
#include <wmistr.h>   /* WMIREG_FLAG_* — NOT pulled in by km/wmilib.h (it
                       * defines WMIGUIDREGINFO itself and includes nothing) */
#include <ntdddisk.h>
#include <mountmgr.h>
#include <mountdev.h>
#include <ntddvol.h>
#include <wdmguid.h>
#include "lvm_read.h"
#include "winlvm_vg.h"

#define WINLVM_WIN_TAG ((unsigned long)'WMLV')
#define WL_MAX_CHILDREN 32
/* Arena cap per accepted VG view: user metadata text may reach
 * LVMCORE_MAX_METADATA_BYTES (1 MiB) plus PV/LV/segment arrays. */
#define WINLVM_VIEW_ARENA_CAP (2u * 1024 * 1024)
#define WINLVM_IO_QUEUE_LIMIT 64u

typedef enum {
    WL_IO_RUNNING = 0,
    WL_IO_QUIESCING = 1,
    WL_IO_QUIESCED = 2,
    WL_IO_REMOVING = 3
} WL_IO_STATE;

typedef struct {
    KSPIN_LOCK lock;
    IO_CSQ csq;
    LIST_ENTRY queued;
    KEVENT drained;
    volatile LONG state;
    volatile LONG active;
    ULONG queued_count;
    PDEVICE_OBJECT device;
    PDRIVER_DISPATCH dispatch;
} WL_IO_GATE;

NTSTATUS WinLvmIoGateInit(WL_IO_GATE *gate);
BOOLEAN WinLvmIoGateEnter(WL_IO_GATE *gate);
void WinLvmIoGateLeave(WL_IO_GATE *gate);
NTSTATUS WinLvmIoGateQueue(WL_IO_GATE *gate, PIRP Irp);
NTSTATUS WinLvmIoGateBeginQuiesce(WL_IO_GATE *gate);
void WinLvmIoGateResume(WL_IO_GATE *gate, PDEVICE_OBJECT DeviceObject);
void WinLvmIoGateRemove(WL_IO_GATE *gate);

void WinLvmLog(const char *fmt, ...);
#define WL_TRACE(msg) WinLvmLog("winlvm: " msg "\n")
#define WL_TRACE_1(msg, a) WinLvmLog("winlvm: " msg "\n", a)
#define WL_TRACE_2(msg, a, b) WinLvmLog("winlvm: " msg "\n", a, b)

/* Device ID and Hardware IDs reported by child LV PDOs.
 * WDM contract: DeviceID MUST have the form <enumerator>\<device-id> with a
 * backslash, else PnP cannot build the device instance path and drops the
 * devnode without error. HardwareIDs is a REG_MULTI_SZ matching the INF. */
static const WCHAR WinLvmDeviceId[] = L"WinLvm\\Volume";
static const WCHAR WinLvmHwId[] = L"WinLvm\\Volume\0WinLvmVolume\0";  /* REG_MULTI_SZ */

typedef enum {
    WL_EXT_BUS = 0x31425553,
    WL_EXT_LV = 0x32564C53,
    WL_EXT_CONTROL = 0x3343544C
} WL_EXT_KIND;

#define WINLVM_CONTROL_DEVICE_NAME L"\\Device\\WinLvmControl"
#define WINLVM_CONTROL_DOS_NAME    L"\\DosDevices\\WinLvmControl"

#define IOCTL_WINLVM_INVENTORY      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_WINLVM_REFRESH        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_WINLVM_QUIESCE_VG     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_WINLVM_RESUME_VG      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_WINLVM_ACTIVATE_LV    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_WINLVM_DEACTIVATE_LV  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_WINLVM_UPDATE_LV      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_WINLVM_GET_LOG        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_READ_DATA)

typedef struct {
    ULONG version;
    ULONG size;
} WINLVM_CONTROL_REQUEST;

typedef struct {
    WINLVM_CONTROL_REQUEST header;
    CHAR lv_uuid[WL_VG_UUID_LEN];
    ULONGLONG byte_size;
    ULONG logical_sector;
    ULONG physical_sector;
} WINLVM_UPDATE_LV_REQUEST;

typedef struct {
    ULONG version;
    ULONG size;
    ULONG record_count;
    ULONG reserved;
} WINLVM_INVENTORY;

typedef struct {
    WL_EXT_KIND kind;
} WINLVM_CONTROL_EXT;

/* Glue-side per-PV backing: the arrived disk behind one PV slot of a
 * record. Core stores an opaque tag pointer per PV (pv_tag[]); this is
 * what it points at. Disk PDO, interface link and FILE_OBJECT are
 * referenced/owned. Sector sizes come from a probe-time
 * StorageAccessAlignmentProperty query (512/512e/4Kn) — the read path's
 * alignment authority (plan line 218-220). */
typedef struct {
    PDEVICE_OBJECT backing_pdo; /* disk PDO holding the PV (referenced) */
    PFILE_OBJECT   backing_file; /* disk file object (referenced) — the
                                  * sync IOCTL and forwarded IRPs ride on it */
    UNICODE_STRING disk_ifname; /* \??\ disk interface link (owned) — maps */
                                /* REMOVAL notifications back to the PV    */
    ULONG          logical_sector;   /* BytesPerLogicalSector  (>=512) */
    ULONG          physical_sector;  /* BytesPerPhysicalSector (>=logical) */
} WINLVM_PV_TAG;

/* A parsed VG view: arena + the view struct. The record's `view` points
 * at the embedded lvmcore_volume_group; the block is owned by the bus
 * (freed only at bus REMOVE, or on a refs==0 RECONCILED swap). */
typedef struct {
    lvmcore_arena        arena;
    lvmcore_volume_group vg;
    uint8_t              buf[WINLVM_VIEW_ARENA_CAP];
} WINLVM_VIEW_BLOCK;

/* Bus FDO extension. */
typedef struct _WINLVM_BUS_EXT {
    WL_EXT_KIND         kind;          /* WL_EXT_BUS */
    PDEVICE_OBJECT      fdo;           /* this device */
    PDEVICE_OBJECT      pdo;           /* root-enumerated PDO below */
    PDEVICE_OBJECT      bus_pdo;       /* attach point (same as pdo here) */
    KSPIN_LOCK          lock;          /* children/vg_records */
    /* Multi-VG registry (W2 Task 8): fixed array of records keyed by VG
     * UUID, with one glue-owned view arena per accepted record. Records
     * are never freed individually — the v22 UAF class is dead at the
     * root (bus REMOVE sweeps them). */
    wl_vg_record        vg_records[WL_VG_MAX];
    WINLVM_VIEW_BLOCK  *vg_views[WL_VG_MAX]; /* arena per record slot */
    PDEVICE_OBJECT      children[WL_MAX_CHILDREN];
    size_t              child_count;
    WMILIB_CONTEXT      wmi;           /* zero-GUID wmilib context (START reg) */
} WINLVM_BUS_EXT;

/* ---- I/O fan-out context (winlvm_io.c) ------------------------------
 * Lifetime diagram (all in winlvm_io.c):
 *   caller IRP (WinLvmRead / WinLvmWrite)
 *     └─ wl_map_begin/next → N pieces (cap WL_MAX_READ_PIECES)
 *     └─ one WL_IO_CTX from nonpaged pool, ctx->remaining = N
 *     └─ per piece: sub-IRP = IoAllocateIrp(backing->StackSize + 1, FALSE)
 *             (ISSUE-21), MDL over the parent buffer (aligned) or a
 *             sector-aligned bounce (unaligned read), IoSetCompletionRoutine
 *     └─ completion: free sub-IRP/MDL/bounce, InterlockedDecrement
 *             remaining; last one frees the ctx (back to pool) and
 *             completes the PARENT IRP exactly once.
 *   ANY failure latches ctx->status; the parent completes with a terminal
 *   status and Information = 0 — never a partial completion.
 * Struct + logic live in ONE place (winlvm_io.c); size visible here for
 * forward declaration. */
#define WL_MAX_READ_PIECES 256

typedef struct WL_IO_CTX WL_IO_CTX;

/* One sub-I/O of a fan-out. Parent buffer slice = [parent_off,
 * parent_off + len): aligned pieces build a fresh MDL directly over the
 * locked parent slice; unaligned read pieces read an aligned window into
 * bounce_buf and copy the slice back in the completion routine. */
typedef struct {
    uint32_t pv_slot;      /* record PV slot (mapper output) */
    uint32_t backing_idx;  /* index into WINLVM_LV_EXT backing tables */
    uint32_t len;          /* piece length delivered to the parent */
    uint32_t bounce;       /* 1 = unaligned → bounce buffer (aligned path) */
    uint64_t pv_byte;      /* backing byte offset (pe_start-resolved) */
    uint32_t parent_off;   /* offset within the parent request window */
    uint64_t sub_offset;   /* backing offset actually issued (aligned down when bounce) */
    uint32_t sub_len;      /* issued length (aligned up when bounce, else == len) */
    uint32_t bounce_skip;  /* bytes skipped inside the bounce window (bounce only) */
    ULONG    logical;      /* backing logical sector size (bounce rounding) */
    PUCHAR   bounce_buf;   /* aligned bounce buffer (NULL for aligned pieces) */
    PIRP     sub_irp;      /* sub-IRP; NULL if this piece was not issued */
    WL_IO_CTX *ctx;        /* owning fan-out context */
} WL_IO_PIECE;

typedef struct WL_IO_CTX {
    PIRP                 parent;        /* the caller's IRP (completed once) */
    PDEVICE_OBJECT       parent_dev;    /* the LV PDO (for completion boost) */
    PUCHAR               parent_base;   /* mapped parent buffer base */
    ULONG                total_len;     /* original request length */
    size_t               bytes_copied;  /* bytes actually transferred */
    size_t               pieces_issued; /* pieces actually sent */
    volatile LONG        remaining;     /* sub-IRPs still in flight */
    volatile NTSTATUS    status;        /* first failure latched (if any) */
    WL_IO_GATE          *gate;
    UCHAR                major_function; /* IRP_MJ_READ or IRP_MJ_WRITE */
    WL_IO_PIECE          pieces[WL_MAX_READ_PIECES];
} WL_IO_CTX;

typedef WL_IO_PIECE WL_READ_PIECE;
typedef WL_IO_CTX   WL_READ_CTX;

/* LV PDO extension. */
typedef struct _WINLVM_LV_EXT {
    WL_EXT_KIND         kind;          /* WL_EXT_LV */
    PDEVICE_OBJECT      pdo;           /* this device */
    wl_vg_record       *record;       /* borrowed from the bus registry */
    size_t              lv_index;     /* index into record->view->lvs[] */
    /* Per-PV backing table — every PV this LV's stripes reference, each
     * with its own ObReferenceObject (PDO + file). NEVER derefed through
     * pv_tag at I/O time: removal frees tags under no I/O lock; these
     * refs keep every backing alive while the child lives (Task 9
     * multi-PV read fan-out). backing_pdo[0] == historical backing_pdo. */
    PDEVICE_OBJECT      backing_pdo[WL_VG_MAX_PVS];
    PFILE_OBJECT        backing_file[WL_VG_MAX_PVS];
    ULONG               backing_logical[WL_VG_MAX_PVS];
    ULONG               backing_physical[WL_VG_MAX_PVS];
    uint32_t            pv_slot[WL_VG_MAX_PVS]; /* record slot for each idx */
    size_t              backing_count; /* distinct PVs in this LV */
    /* Volume-level sector size for geometry/alignment answers: the
     * strictest member bounds — logical min, physical max (a volume on
     * mixed 512e/4Kn members must be addressable by every backing). */
    ULONG               logical_sector;
    ULONG               physical_sector;
    ULONGLONG           byte_size;
    UNICODE_STRING      ifname;        /* volume interface, if registered */
    WCHAR               devname[64];   /* \Device\WinLvm<uuid> (mountmgr) */
    WMILIB_CONTEXT      wmi;           /* zero-GUID wmilib context (START reg) */
    WL_IO_GATE           io_gate;      /* atomic admission + quiesce queue */
} WINLVM_LV_EXT;

/* Kernel-side byte source: IRP_MJ_READ against the disk PDO. */
typedef struct {
    PDEVICE_OBJECT  disk_pdo;
    PFILE_OBJECT    file;
    KEVENT          done;
} WINLVM_DISK_SOURCE;

/* Globals (winlvm.c). */
extern WINLVM_BUS_EXT *g_bus_ext;      /* set by AddDevice, cleared on REMOVE */
extern PVOID            g_disk_notify_cookie;
extern PDEVICE_OBJECT   g_control_device;
extern BOOLEAN          g_control_link_created;

/* Dispatchers. */
DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD     WinLvmUnload;
DRIVER_ADD_DEVICE WinLvmAddDevice;

DRIVER_DISPATCH WinLvmCreateClose;
DRIVER_DISPATCH WinLvmSystemControl;
DRIVER_DISPATCH WinLvmRead;
DRIVER_DISPATCH WinLvmWrite;
DRIVER_DISPATCH WinLvmFlush;
DRIVER_DISPATCH WinLvmShutdown;
DRIVER_DISPATCH WinLvmDeviceControl;
DRIVER_DISPATCH WinLvmPnp;
DRIVER_DISPATCH WinLvmPower;

NTSTATUS WinLvmPnpDiskNotify(PVOID NotificationStructure, PVOID Context);
void WinLvmProbeWorker(PVOID);

/* winlvm.c — zero-GUID WMI provider (Oracle 2026-09-12 verdict: register
 * wmilib on both the bus FDO and every LV PDO at START, dispatch through
 * WmiSystemControl — the classpnp shape that satisfies the verifier's
 * VfWmiTestStartedPdoStack probe; v16–v18 never registered). */
NTSTATUS WinLvmQueryWmiRegInfo(PDEVICE_OBJECT DeviceObject, PULONG RegFlags,
                               PUNICODE_STRING InstanceName,
                               PUNICODE_STRING *RegistryPath,
                               PUNICODE_STRING MofResourceName,
                               PDEVICE_OBJECT *Pdo);
void WinLvmWmiInit(PDEVICE_OBJECT DeviceObject);

/* winlvm_bus.c / winlvm_child.c — kind halves of WinLvmPnp. */
DRIVER_DISPATCH WinLvmBusPnp;
DRIVER_DISPATCH WinLvmChildPnp;
void WinLvmBusResetInactiveRecords(WINLVM_BUS_EXT *bus);

/* winlvm_io.c */
size_t winlvm_disk_source_read(void *impl, uint64_t off, void *buf, size_t len);

/* winlvm_mount.c */
NTSTATUS WinLvmMountIoctl(PDEVICE_OBJECT, PIRP, WINLVM_LV_EXT *);

#endif
