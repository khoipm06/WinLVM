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
#include <ntdddisk.h>
#include <mountmgr.h>
#include <mountdev.h>
#include <ntddvol.h>
#include <wdmguid.h>
#include "lvm_read.h"

#define WINLVM_WIN_TAG ((unsigned long)'WMLV')
#define WL_MAX_CHILDREN 32

/* Tracing (shared by winlvm.c / winlvm_bus.c / winlvm_child.c). */
#define WL_TRACE(msg) DbgPrint("winlvm: " msg "\n")
#define WL_TRACE_1(msg, a) DbgPrint("winlvm: " msg "\n", a)

/* Device ID and Hardware IDs reported by child LV PDOs.
 * WDM contract: DeviceID MUST have the form <enumerator>\<device-id> with a
 * backslash, else PnP cannot build the device instance path and drops the
 * devnode without error. HardwareIDs is a REG_MULTI_SZ matching the INF. */
static const WCHAR WinLvmDeviceId[] = L"WinLvm\\Volume";
static const WCHAR WinLvmHwId[] = L"WinLvm\\Volume\0WinLvmVolume\0";  /* REG_MULTI_SZ */

typedef enum { WL_EXT_BUS = 0x31425553, WL_EXT_LV = 0x32564C53 } WL_EXT_KIND;

/* One cached volume group (single-PV groups in this phase). */
typedef struct {
    winlvm_volume_group vg;        /* as parsed by lvm_read.c */
    PDEVICE_OBJECT       backing_pdo; /* disk PDO holding the PV (referenced) */
} WINLVM_VG_RECORD;

/* Bus FDO extension. */
typedef struct _WINLVM_BUS_EXT {
    WL_EXT_KIND         kind;          /* WL_EXT_BUS */
    PDEVICE_OBJECT      fdo;           /* this device */
    PDEVICE_OBJECT      pdo;           /* root-enumerated PDO below */
    PDEVICE_OBJECT      bus_pdo;       /* attach point (same as pdo here) */
    KSPIN_LOCK          lock;          /* children/vgs */
    WINLVM_VG_RECORD   *vgs;           /* single cached VG for now */
    PDEVICE_OBJECT      children[WL_MAX_CHILDREN];
    size_t              child_count;
} WINLVM_BUS_EXT;

/* LV PDO extension. */
typedef struct _WINLVM_LV_EXT {
    WL_EXT_KIND         kind;          /* WL_EXT_LV */
    PDEVICE_OBJECT      pdo;           /* this device */
    WINLVM_VG_RECORD   *record;       /* borrowed from the bus */
    size_t              lv_index;     /* index into record->vg.lvs[] */
    PDEVICE_OBJECT      backing_pdo;   /* PV disk PDO (referenced) */
    ULONGLONG           byte_size;
    UNICODE_STRING      ifname;        /* volume interface, if registered */
    WCHAR               devname[64];   /* \Device\WinLvm<uuid> (mountmgr) */
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

/* winlvm_bus.c / winlvm_child.c — kind halves of WinLvmPnp. */
DRIVER_DISPATCH WinLvmBusPnp;
DRIVER_DISPATCH WinLvmChildPnp;

/* winlvm_io.c */
size_t winlvm_disk_source_read(void *impl, uint64_t off, void *buf, size_t len);

/* winlvm_mount.c */
NTSTATUS WinLvmMountIoctl(PDEVICE_OBJECT, PIRP, WINLVM_LV_EXT *);

#endif
