/*
 * winlvm.h — winlvm.sys internal types.
 *
 * Architecture mirrors maharmstone/winmd (pure C WDM):
 *   - One control device object (CDO), created in DriverEntry, owned by the
 *     driver instance. Holds the PnP cookie for disk interface notifications
 *     and the in-memory VG cache.
 *   - One PDO per LV, created by a deferred worker when a disk arrives and
 *     proves to be an LVM PV. Each PDO's device extension carries the LV's
 *     segment map and the backing disk PDO pointer.
 *
 * I/O: IoGetDeviceObjectPointer + IoAllocateIrp + IoCallDriver to probe PVs;
 * reads through the LV PDO fan out into sub-IRPs to the backing disk. Writes
 * are refused with STATUS_MEDIA_WRITE_PROTECTED until Phase 3.
 *
 * NEVER ZwCreateFile. The previous prototype did ZwCreateFile on every IRP;
 * that is the structural reason it was unbootable and the reason it was
 * deleted (FEASIBILITY.md §2, winlvm-project-direction memory).
 */
#ifndef WINLVM_DRIVER_WINLVM_H
#define WINLVM_DRIVER_WINLVM_H

#include <ntddk.h>
#include <wdm.h>
#include "lvm_read.h"

#define WINLVM_WIN_TAG ((unsigned long)'WMLV')

/* Forward decls of our internal device-extension types. */
typedef struct _WINLVM_CDO_EXT WINLVM_CDO_EXT;
typedef struct _WINLVM_LV_EXT  WINLVM_LV_EXT;

/* The volume group as we cache it on the CDO. lvm_read fills a
 * winlvm_volume_group on the stack of the probe worker; we copy it into a
 * nonpaged-pool record. The driver keeps ONE cached VG per PV-device-group
 * (we only resolve single-PV groups at this stage). */
typedef struct {
    winlvm_volume_group vg;          /* as parsed by lvm_read.c */
    PDEVICE_OBJECT      *backing_pdos; /* nonpaged array, indexed by vg.pvs[] */
    UNICODE_STRING       volume_ifname; /* registered GUID_DEVINTERFACE_VOLUME, if any */
} WINLVM_VG_RECORD;

/* CDO extension. */
struct _WINLVM_CDO_EXT {
    PDEVICE_OBJECT      control_device;
    PVOID               disk_notify_cookie;
    KSPIN_LOCK          lock;        /* protects the cached vg list */
    WINLVM_VG_RECORD   *vgs;         /* simple array; nonpaged */
    size_t              vg_count;
};

/* LV PDO extension. */
struct _WINLVM_LV_EXT {
    PDEVICE_OBJECT      pdo;
    WINLVM_VG_RECORD   *vg;          /* borrowed from the CDO */
    size_t              lv_index;     /* index into vg->vg.lvs[] */
    PDEVICE_OBJECT      backing_pdo;  /* first PV's disk PDO */
    ULONGLONG           byte_size;
};

/* lvm_read.c expects a winlvm_byte_source whose impl is a kernel-side
 * struct holding the disk PDO + FILE_OBJECT. read() builds an IRP_MJ_READ,
 * sends it with IoCallDriver, and waits on the event. */
typedef struct {
    PDEVICE_OBJECT  disk_pdo;        /* top of the stack we send IRPs to */
    PFILE_OBJECT    file;            /* referenced file object for that PDO */
    KEVENT          done;            /* completion synchronisation */
} WINLVM_DISK_SOURCE;

/* Forward decls of the dispatchers. */
DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD     WinLvmUnload;
DRIVER_ADD_DEVICE WinLvmAddDevice;

DRIVER_DISPATCH WinLvmCreateClose;
DRIVER_DISPATCH WinLvmRead;
DRIVER_DISPATCH WinLvmWrite;
DRIVER_DISPATCH WinLvmFlush;
DRIVER_DISPATCH WinLvmShutdown;
DRIVER_DISPATCH WinLvmDeviceControl;
DRIVER_DISPATCH WinLvmPnp;

NTSTATUS WinLvmPnpDiskNotify(PVOID NotificationStructure, PVOID Context);
void WinLvmProbeWorker(PVOID);

/* winlvm_io.c */
size_t winlvm_disk_source_read(void *impl, uint64_t off, void *buf, size_t len);

/* winlvm_mount.c */
NTSTATUS WinLvmMountIoctl(PDEVICE_OBJECT, PIRP, WINLVM_LV_EXT *);

#endif
