/*
 * winlvm.h — Internal WDM driver declarations for winlvm.sys.
 *
 * Bus driver architecture:
 *   - ROOT\winlvm enumerates the bus FDO (WinLvmAddDevice).
 *   - PnP interface notifications and IOCTL_WINLVM_PROBE_DEVICE probe
 *     candidate backing devices, parse LVM2 metadata via lvmcore, and
 *     create one raw FILE_DEVICE_DISK child PDO per eligible, PV-complete LV.
 *   - Backing devices are accessed via IoGetDeviceObjectPointer device/file
 *     references and IoCallDriver (never ZwCreateFile).
 */
#ifndef WINLVM_DRIVER_WINLVM_H
#define WINLVM_DRIVER_WINLVM_H

#include <ntifs.h>
#include <wdmsec.h>
#include <wmilib.h>
#include <wmistr.h>
#include <ntdddisk.h>
#include <mountmgr.h>
#include <mountdev.h>
#include <ntddvol.h>
#include <wdmguid.h>
#include "../winlvm_ioctl.h"
#include "winlvm_vg.h"

#define WINLVM_WIN_TAG         ((unsigned long)'WMLV')
#define WL_MAX_CHILDREN        32
#define WINLVM_LOG_BUFFER_SIZE 32768u
#define WINLVM_VIEW_ARENA_CAP  (2u * 1024 * 1024)
#define WINLVM_IO_QUEUE_LIMIT  64u
#define WL_MAX_READ_PIECES     256

typedef enum {
    WL_IO_RUNNING   = 0,
    WL_IO_QUIESCING = 1,
    WL_IO_QUIESCED  = 2,
    WL_IO_REMOVING  = 3
} WL_IO_STATE;

typedef struct {
    KSPIN_LOCK       lock;
    IO_CSQ           csq;
    LIST_ENTRY       queued;
    KEVENT           drained;
    LONG             state;
    LONG             active;
    ULONG            queued_count;
    PDEVICE_OBJECT   device;
    PDRIVER_DISPATCH dispatch;
} WL_IO_GATE;

NTSTATUS WinLvmIoGateInit(WL_IO_GATE *gate);
BOOLEAN WinLvmIoGateEnter(WL_IO_GATE *gate);
void WinLvmIoGateLeave(WL_IO_GATE *gate);
NTSTATUS WinLvmIoGateQueue(WL_IO_GATE *gate, PIRP Irp);
NTSTATUS WinLvmIoGateBeginQuiesce(WL_IO_GATE *gate);
BOOLEAN WinLvmIoGateIsQuiesced(WL_IO_GATE *gate);
void WinLvmIoGateResume(WL_IO_GATE *gate, PDEVICE_OBJECT DeviceObject);
void WinLvmIoGateRemove(WL_IO_GATE *gate);

void WinLvmLog(const char *fmt, ...);
ULONG WinLvmLogDrain(char *out, ULONG cap);
#define WL_TRACE(msg)          WinLvmLog("winlvm: " msg "\n")
#define WL_TRACE_1(msg, a)     WinLvmLog("winlvm: " msg "\n", a)
#define WL_TRACE_2(msg, a, b)  WinLvmLog("winlvm: " msg "\n", a, b)
#define WL_TRACE_3(msg, a, b, c) WinLvmLog("winlvm: " msg "\n", a, b, c)

/* Child LV PDO PnP IDs: DeviceID uses <enumerator>\<id> form; HardwareIDs is REG_MULTI_SZ. */
static const WCHAR WinLvmDeviceId[] = L"WinLvm\\Volume";
static const WCHAR WinLvmHwId[] = L"WinLvm\\Volume\0WinLvmVolume\0";

typedef enum {
    WL_EXT_BUS     = 0x31425553,
    WL_EXT_LV      = 0x32564C53,
    WL_EXT_CONTROL = 0x3343544C
} WL_EXT_KIND;

typedef struct {
    WL_EXT_KIND kind;
} WINLVM_CONTROL_EXT;

/* Per-PV backing descriptor owned by the bus registry (pv_tag[]). */
typedef struct {
    PDEVICE_OBJECT backing_pdo;
    PFILE_OBJECT   backing_file;
    UNICODE_STRING disk_ifname;
    ULONG          logical_sector;
    ULONG          physical_sector;
    BOOLEAN        read_only;
} WINLVM_PV_TAG;

/* Heap-allocated parsed VG view and its backing lvmcore arena. */
typedef struct {
    lvmcore_arena        arena;
    lvmcore_volume_group vg;
    uint8_t              buf[WINLVM_VIEW_ARENA_CAP];
} WINLVM_VIEW_BLOCK;

typedef struct _WINLVM_BUS_EXT {
    WL_EXT_KIND        kind;
    PDEVICE_OBJECT     fdo;
    PDEVICE_OBJECT     pdo;
    PDEVICE_OBJECT     bus_pdo;
    KSPIN_LOCK         lock;
    wl_vg_record       vg_records[WL_VG_MAX];
    WINLVM_VIEW_BLOCK *vg_views[WL_VG_MAX];
    PDEVICE_OBJECT     children[WL_MAX_CHILDREN];
    size_t             child_count;
    WMILIB_CONTEXT     wmi;
    BOOLEAN            wmi_registered;
} WINLVM_BUS_EXT;

typedef struct WL_IO_CTX WL_IO_CTX;

typedef struct {
    uint32_t   pv_slot;
    uint32_t   backing_idx;
    uint32_t   len;
    uint32_t   bounce;
    uint64_t   pv_byte;
    uint32_t   parent_off;
    uint64_t   sub_offset;
    uint32_t   sub_len;
    uint32_t   bounce_skip;
    ULONG      logical;
    PUCHAR     bounce_buf;
    PIRP       sub_irp;
    WL_IO_CTX *ctx;
} WL_IO_PIECE;

typedef struct WL_IO_CTX {
    PIRP              parent;
    PDEVICE_OBJECT    parent_dev;
    PUCHAR            parent_base;
    ULONG             total_len;
    size_t            bytes_copied;
    volatile LONG     remaining;
    volatile NTSTATUS status;
    WL_IO_GATE       *gate;
    UCHAR             major_function;
    WL_IO_PIECE       pieces[WL_MAX_READ_PIECES];
} WL_IO_CTX;

typedef struct _WINLVM_LV_EXT {
    WL_EXT_KIND    kind;
    PDEVICE_OBJECT pdo;
    wl_vg_record  *record;
    size_t         lv_index;
    CHAR           lv_uuid[WL_VG_UUID_LEN];
    PDEVICE_OBJECT backing_pdo[WL_VG_MAX_PVS];
    PFILE_OBJECT   backing_file[WL_VG_MAX_PVS];
    ULONG          backing_logical[WL_VG_MAX_PVS];
    ULONG          backing_physical[WL_VG_MAX_PVS];
    uint32_t       pv_slot[WL_VG_MAX_PVS];
    size_t         backing_count;
    /* Strictest member bounds: logical max, physical max across backing PVs. */
    ULONG          logical_sector;
    ULONG          physical_sector;
    ULONGLONG      byte_size;
    BOOLEAN        read_only;
    BOOLEAN        unavailable;
    BOOLEAN        deactivating;
    BOOLEAN        reported_to_pnp;
    BOOLEAN        wmi_registered;
    UNICODE_STRING ifname;
    WCHAR          devname[64];
    WMILIB_CONTEXT wmi;
    WL_IO_GATE     io_gate;
} WINLVM_LV_EXT;

typedef struct {
    PDEVICE_OBJECT disk_pdo;
    PFILE_OBJECT   file;
    ULONG          sector_size;
} WINLVM_DISK_SOURCE;

extern WINLVM_BUS_EXT *g_bus_ext;
extern PVOID           g_disk_notify_cookie;
extern PVOID           g_volume_notify_cookie;
extern PDEVICE_OBJECT  g_control_device;
extern BOOLEAN         g_control_link_created;

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

NTSTATUS WinLvmQueryWmiRegInfo(PDEVICE_OBJECT DeviceObject, PULONG RegFlags,
                               PUNICODE_STRING InstanceName,
                               PUNICODE_STRING *RegistryPath,
                               PUNICODE_STRING MofResourceName,
                               PDEVICE_OBJECT *Pdo);
void WinLvmWmiInit(PDEVICE_OBJECT DeviceObject);
void WinLvmWmiDeregister(PDEVICE_OBJECT DeviceObject);
NTSTATUS WinLvmCreateControlDevice(PDRIVER_OBJECT DriverObject);
void WinLvmDeleteControlDevice(void);

#define WL_MAX_DROPPED_REFS (WL_MAX_CHILDREN * WL_VG_MAX_PVS)

typedef struct {
    PDEVICE_OBJECT pdos[WL_MAX_DROPPED_REFS];
    PFILE_OBJECT   files[WL_MAX_DROPPED_REFS];
    size_t         count;
} WINLVM_RECONCILE_DROPS;

/* winlvm.c helpers */
void WinLvmFreePvTag(WINLVM_PV_TAG *tag);
void WinLvmFreeReconcileDrops(WINLVM_RECONCILE_DROPS *drops);

/* winlvm_pnp.c */
DRIVER_DISPATCH WinLvmBusPnp;
void WinLvmBusResetInactiveRecords(WINLVM_BUS_EXT *bus);
BOOLEAN WinLvmQuiesceRecordChildren(WINLVM_BUS_EXT *bus, const wl_vg_record *r,
                                    PDEVICE_OBJECT *out_pdos, size_t *out_count);
BOOLEAN WinLvmAllRecordChildrenQuiescedLocked(WINLVM_BUS_EXT *bus, const wl_vg_record *r);
void WinLvmResumeRecordChildren(PDEVICE_OBJECT *pdos, size_t count);
wl_update_result WinLvmUpdateLvSeam(void *ctx, const char *lv_uuid);
void WinLvmReconcileChildrenLocked(WINLVM_BUS_EXT *bus, wl_vg_record *r,
                                   WINLVM_RECONCILE_DROPS *drops);
void WinLvmSwapRecordViewLocked(wl_vg_record *r,
                                WINLVM_VIEW_BLOCK **view_slot,
                                WINLVM_VIEW_BLOCK *next_block,
                                WINLVM_VIEW_BLOCK **old_view_out,
                                WINLVM_PV_TAG **dropped_tags,
                                size_t *dropped_tag_count);

/* winlvm_child.c */
DRIVER_DISPATCH WinLvmChildPnp;
void WinLvmDeleteLvPdo(PDEVICE_OBJECT LvPdo);
BOOLEAN WinLvmComputeLvReadOnly(const wl_vg_record *record, size_t lv_index,
                                const size_t *slots, size_t slot_count);
BOOLEAN WinLvmCollectLvBackingSlotsLocked(const wl_vg_record *record, size_t lv_index,
                                          size_t *slots_out, size_t *slot_count_out,
                                          ULONG *max_stack_out, ULONG *max_logical_out,
                                          ULONG *max_physical_out);
BOOLEAN WinLvmProbeWorkerCreateLvPdo(WINLVM_BUS_EXT *bus, wl_vg_record *r,
                                     size_t lv_index);
void WinLvmReactivateRecordLvsLocked(WINLVM_BUS_EXT *bus, wl_vg_record *r,
                                     size_t *newly_out, size_t newly_cap,
                                     size_t *newly_count_out,
                                     PDEVICE_OBJECT *revived_out,
                                     size_t *revived_count_out);
void WinLvmFinishProbeActivation(WINLVM_BUS_EXT *bus, size_t slot,
                                 const size_t *newly, size_t newly_count,
                                 PDEVICE_OBJECT *revived, size_t revived_count,
                                 BOOLEAN force_invalidate);

/* winlvm_probe.c */
void WinLvmProbeSubsystemInit(void);
void WinLvmProbeSubsystemResume(void);
void WinLvmStopAndDrainProbeWork(void);
BOOLEAN WinLvmShouldIgnoreLinkName(const UNICODE_STRING *name);
NTSTATUS WinLvmRegisterDiskNotify(PDRIVER_OBJECT DriverObject);
void WinLvmUnregisterDiskNotify(void);
void WinLvmRegisterDiskNotifyDeferred(PDRIVER_OBJECT DriverObject);
NTSTATUS WinLvmPnpDiskNotify(PVOID NotificationStructure, PVOID Context);
NTSTATUS WinLvmProbeDevicePath(const UNICODE_STRING *symLinkOrNtPath);
NTSTATUS WinLvmProbeDevicePathEx(const UNICODE_STRING *symLinkOrNtPath,
                                 BOOLEAN allow_part_scan,
                                 BOOLEAN explicit_probe);

/* winlvm_control.c */
NTSTATUS WinLvmControlDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS WinLvmBusActivateLvByUuid(WINLVM_BUS_EXT *bus, const char *lv_uuid);
NTSTATUS WinLvmBusDeactivateLvByUuid(WINLVM_BUS_EXT *bus, const char *lv_uuid);

/* winlvm_subirp.c */
NTSTATUS LvFanOutIo(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* winlvm_sector.c */
NTSTATUS WinLvmOpenBackingDevice(const UNICODE_STRING *symLinkOrNtPath,
                                 PDRIVER_OBJECT own_driver,
                                 UNICODE_STRING *nt_path_out,
                                 PFILE_OBJECT *file_out,
                                 PDEVICE_OBJECT *pdo_out,
                                 BOOLEAN *read_only_out);
void WinLvmQuerySectorSize(PDEVICE_OBJECT disk_pdo, PFILE_OBJECT disk_file,
                           ULONG *logical_out, ULONG *physical_out);
BOOLEAN WinLvmQueryDiskWritable(PDEVICE_OBJECT disk_pdo, PFILE_OBJECT disk_file);
NTSTATUS WinLvmReadVgFromDevice(PDEVICE_OBJECT disk_pdo, PFILE_OBJECT disk_file,
                                ULONG logical_sector, WINLVM_VIEW_BLOCK **block_out,
                                char pv_uuid_out[WL_VG_UUID_LEN]);
BOOLEAN WinLvmGetDiskPartitions(PDEVICE_OBJECT disk_pdo, PFILE_OBJECT disk_file,
                                ULONG *device_num_out, ULONG *parts_out,
                                size_t parts_cap, size_t *parts_count_out);
NTSTATUS WinLvmProbePartitionList(ULONG device_num, const ULONG *parts,
                                  size_t count, BOOLEAN explicit_probe);
size_t LvMapToPieces(WINLVM_LV_EXT *ext, const lvmcore_volume_group *view,
                     uint64_t lv_byte, uint32_t len, WL_IO_PIECE *pieces, size_t cap);
size_t winlvm_disk_source_read(void *impl, uint64_t off, void *buf, size_t len);

/* winlvm_mount.c */
NTSTATUS WinLvmMountIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp, WINLVM_LV_EXT *ext);

/* winlvm_storage.c */
NTSTATUS WinLvmStorageIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp, WINLVM_LV_EXT *ext);

#endif /* WINLVM_DRIVER_WINLVM_H */
