/*
 * winlvm_mount.c — Mount Manager IOCTLs and volume FSCTLs for LV PDOs.
 *
 * Dual-compiled for Windows kernel mode (_KERNEL_MODE) and host unit tests
 * via host_wdm_types.h.
 */
#ifdef _KERNEL_MODE
#include <stddef.h>
#include "winlvm.h"
#else
#include "host_wdm_types.h"
#endif

#ifndef IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME
#define IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME CTL_CODE(MOUNTDEV_IOCTL, 3, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef IOCTL_MOUNTDEV_LINK_CREATED
#define IOCTL_MOUNTDEV_LINK_CREATED CTL_CODE(MOUNTDEV_IOCTL, 4, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef IOCTL_MOUNTDEV_LINK_DELETED
#define IOCTL_MOUNTDEV_LINK_DELETED CTL_CODE(MOUNTDEV_IOCTL, 5, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef IOCTL_MOUNTDEV_QUERY_STABLE_GUID
#define IOCTL_MOUNTDEV_QUERY_STABLE_GUID CTL_CODE(MOUNTDEV_IOCTL, 6, METHOD_BUFFERED, FILE_ANY_ACCESS)
typedef struct _MOUNTDEV_STABLE_GUID {
    GUID StableGuid;
} MOUNTDEV_STABLE_GUID, *PMOUNTDEV_STABLE_GUID;
#endif

static WCHAR WinLvmAsciiLowerW_Mount(WCHAR c) {
    return (c >= L'A' && c <= L'Z') ? (WCHAR)(c + (L'a' - L'A')) : c;
}

static BOOLEAN WinLvmPrefixEqualI(const WCHAR *s, size_t len, const char *prefix, size_t plen) {
    if (len < plen) return FALSE;
    for (size_t i = 0; i < plen; i++) {
        if (WinLvmAsciiLowerW_Mount(s[i]) != (WCHAR)(unsigned char)prefix[i]) {
            return FALSE;
        }
    }
    return TRUE;
}

/*
 * Parse a MountMgr drive-letter link ("\DosDevices\X:" or "\??\X:", with
 * optional trailing backslash) into an uppercase ASCII drive letter 'A'..'Z'.
 * Returns 0 for non-drive-letter links such as "\??\Volume{...}".
 */
static WCHAR WinLvmExtractDriveLetter(const WCHAR *name, USHORT byte_len) {
    if (!name || byte_len == 0 || (byte_len % sizeof(WCHAR)) != 0) return 0;
    size_t cch = (size_t)(byte_len / sizeof(WCHAR));
    if (cch > 0 && name[cch - 1] == L'\\') cch--;

    size_t letter_idx = 0;
    if (cch == 14 && WinLvmPrefixEqualI(name, cch, "\\dosdevices\\", 12)) {
        letter_idx = 12;
    } else if (cch == 6 && WinLvmPrefixEqualI(name, cch, "\\??\\", 4)) {
        letter_idx = 4;
    } else {
        return 0;
    }

    if (name[letter_idx + 1] != L':') return 0;
    WCHAR ch = name[letter_idx];
    if (ch >= L'a' && ch <= L'z') ch = (WCHAR)(ch - (L'a' - L'A'));
    if (ch >= L'A' && ch <= L'Z') return ch;
    return 0;
}

static void WinLvmUuidToGuid(const char *uuid, GUID *guid) {
    unsigned char bytes[16];
    BOOLEAN is_hex = TRUE;
    RtlZeroMemory(guid, sizeof(GUID));
    for (int i = 0; i < 16; i++) {
        char c1 = uuid[i * 2];
        char c2 = uuid[i * 2 + 1];
        int v1 = -1, v2 = -1;
        if (c1 >= '0' && c1 <= '9') v1 = c1 - '0';
        else if (c1 >= 'a' && c1 <= 'f') v1 = c1 - 'a' + 10;
        else if (c1 >= 'A' && c1 <= 'F') v1 = c1 - 'A' + 10;
        if (c2 >= '0' && c2 <= '9') v2 = c2 - '0';
        else if (c2 >= 'a' && c2 <= 'f') v2 = c2 - 'a' + 10;
        else if (c2 >= 'A' && c2 <= 'F') v2 = c2 - 'A' + 10;
        if (v1 < 0 || v2 < 0) {
            is_hex = FALSE;
            break;
        }
        bytes[i] = (unsigned char)((v1 << 4) | v2);
    }
    if (is_hex) {
        RtlCopyMemory(guid, bytes, sizeof(GUID));
    } else {
        RtlCopyMemory(guid, uuid, sizeof(GUID));
    }
}

static const char *WinLvmMountGetLvUuid(const WINLVM_LV_EXT *ext) {
#ifdef _KERNEL_MODE
    return (ext && ext->lv_uuid[0] != '\0') ? ext->lv_uuid : NULL;
#else
    const lvmcore_volume_group *view = (ext && ext->record) ? ext->record->view : NULL;
    if (!view || !view->lvs || ext->lv_index >= view->lv_count) return NULL;
    return view->lvs[ext->lv_index].id;
#endif
}

NTSTATUS WinLvmMountIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp, WINLVM_LV_EXT *ext) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = irpSp->Parameters.DeviceIoControl.IoControlCode;
    ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID sysBuf = Irp->AssociatedIrp.SystemBuffer;
    UNREFERENCED_PARAMETER(DeviceObject);

    if (code == IOCTL_MOUNTDEV_QUERY_DEVICE_NAME) {
        if (outLen < sizeof(MOUNTDEV_NAME)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;

        PMOUNTDEV_NAME mountName = (PMOUNTDEV_NAME)sysBuf;
        UNICODE_STRING devName;
        RtlInitUnicodeString(&devName, ext->devname);
        mountName->NameLength = devName.Length;

        if (outLen < (ULONG)(offsetof(MOUNTDEV_NAME, Name) + devName.Length)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
            return STATUS_BUFFER_OVERFLOW;
        }

        RtlCopyMemory(mountName->Name, devName.Buffer, devName.Length);
        Irp->IoStatus.Information = offsetof(MOUNTDEV_NAME, Name) + devName.Length;
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_MOUNTDEV_QUERY_UNIQUE_ID) {
        if (outLen < sizeof(MOUNTDEV_UNIQUE_ID)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;

        const char *lv_uuid = WinLvmMountGetLvUuid(ext);
        if (!lv_uuid) {
            Irp->IoStatus.Information = 0;
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        PMOUNTDEV_UNIQUE_ID uniqueId = (PMOUNTDEV_UNIQUE_ID)sysBuf;
        uniqueId->UniqueIdLength = 32;

        if (outLen < (ULONG)(offsetof(MOUNTDEV_UNIQUE_ID, UniqueId) + 32)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
            return STATUS_BUFFER_OVERFLOW;
        }

        RtlCopyMemory(uniqueId->UniqueId, lv_uuid, 32);
        Irp->IoStatus.Information = offsetof(MOUNTDEV_UNIQUE_ID, UniqueId) + 32;
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_MOUNTDEV_LINK_CREATED || code == IOCTL_MOUNTDEV_LINK_DELETED) {
        ULONG inLen = irpSp->Parameters.DeviceIoControl.InputBufferLength;
        if (!sysBuf || inLen < (ULONG)offsetof(MOUNTDEV_NAME, Name)) {
            Irp->IoStatus.Information = 0;
            return STATUS_INVALID_PARAMETER;
        }
        PMOUNTDEV_NAME mountName = (PMOUNTDEV_NAME)sysBuf;
        if ((mountName->NameLength % sizeof(WCHAR)) != 0 ||
            (ULONG)offsetof(MOUNTDEV_NAME, Name) + mountName->NameLength > inLen) {
            Irp->IoStatus.Information = 0;
            return STATUS_INVALID_PARAMETER;
        }
        WCHAR letter = WinLvmExtractDriveLetter(mountName->Name, mountName->NameLength);
        if (letter != 0) {
            if (code == IOCTL_MOUNTDEV_LINK_CREATED) {
                ext->drive_letter = letter;
            } else if (ext->drive_letter == letter) {
                ext->drive_letter = 0;
            }
        }
        Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    if (code == IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME) {
        Irp->IoStatus.Information = 0;
        return STATUS_NOT_IMPLEMENTED;
    }

    if (code == IOCTL_MOUNTDEV_QUERY_STABLE_GUID) {
        if (outLen < sizeof(MOUNTDEV_STABLE_GUID)) {
            Irp->IoStatus.Information = sizeof(MOUNTDEV_STABLE_GUID);
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (!sysBuf) return STATUS_INVALID_PARAMETER;

        const char *lv_uuid = WinLvmMountGetLvUuid(ext);
        if (!lv_uuid) {
            Irp->IoStatus.Information = 0;
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        PMOUNTDEV_STABLE_GUID mdsg = (PMOUNTDEV_STABLE_GUID)sysBuf;
        WinLvmUuidToGuid(lv_uuid, &mdsg->StableGuid);
        Irp->IoStatus.Information = sizeof(MOUNTDEV_STABLE_GUID);
        return STATUS_SUCCESS;
    }

    if (code == FSCTL_DISMOUNT_VOLUME || code == FSCTL_LOCK_VOLUME ||
        code == FSCTL_UNLOCK_VOLUME || code == FSCTL_ALLOW_EXTENDED_DASD_IO) {
        Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

#ifdef _KERNEL_MODE
void WinLvmSendMountMgrArrival(const WCHAR *devname) {
    if (!devname || devname[0] == 0 || KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    UNICODE_STRING mm_name;
    RtlInitUnicodeString(&mm_name, MOUNTMGR_DEVICE_NAME);
    PFILE_OBJECT mm_file = NULL;
    PDEVICE_OBJECT mm_dev = NULL;
    NTSTATUS st = IoGetDeviceObjectPointer(
        &mm_name, FILE_READ_ATTRIBUTES, &mm_file, &mm_dev);
    if (!NT_SUCCESS(st) || !mm_file || !mm_dev) return;

    UNICODE_STRING dev_name;
    RtlInitUnicodeString(&dev_name, devname);
    ULONG in_len = (ULONG)(FIELD_OFFSET(MOUNTMGR_TARGET_NAME, DeviceName) + dev_name.Length);
    PMOUNTMGR_TARGET_NAME target = (PMOUNTMGR_TARGET_NAME)ExAllocatePoolZero(
        NonPagedPool, in_len, WINLVM_WIN_TAG);
    if (target) {
        target->DeviceNameLength = dev_name.Length;
        RtlCopyMemory(target->DeviceName, dev_name.Buffer, dev_name.Length);

        KEVENT ev;
        IO_STATUS_BLOCK iosb;
        KeInitializeEvent(&ev, NotificationEvent, FALSE);
        RtlZeroMemory(&iosb, sizeof(iosb));
        PIRP irp = IoBuildDeviceIoControlRequest(
            IOCTL_MOUNTMGR_VOLUME_ARRIVAL_NOTIFICATION,
            mm_dev, target, in_len, NULL, 0, FALSE, &ev, &iosb);
        if (irp) {
            IoGetNextIrpStackLocation(irp)->FileObject = mm_file;
            st = IoCallDriver(mm_dev, irp);
            if (st == STATUS_PENDING) {
                KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
                st = iosb.Status;
            }
            WL_TRACE_2("mountmgr arrival %wZ -> 0x%08x", &dev_name, (unsigned)st);
        }
        ExFreePoolWithTag(target, WINLVM_WIN_TAG);
    }
    ObDereferenceObject(mm_file);
}

/*
 * Synchronized VPB_MOUNTED probe under the global VPB spinlock so concurrent
 * dismounts/VPB swaps cannot free LvPdo->Vpb while we inspect its flags.
 */
static BOOLEAN WinLvmIsVolumeMounted(PDEVICE_OBJECT LvPdo) {
    if (!LvPdo) return FALSE;
    KIRQL irql;
    IoAcquireVpbSpinLock(&irql);
    BOOLEAN mounted = (LvPdo->Vpb && (LvPdo->Vpb->Flags & VPB_MOUNTED)) ? TRUE : FALSE;
    IoReleaseVpbSpinLock(irql);
    return mounted;
}

/*
 * Force-dismount any already-mounted VPB on our own LV PDO and bounce the
 * volume device interface so Windows tears down a stale read-only (or RW)
 * filesystem instance before re-mounting under the updated read_only state.
 * FILE_READ_DATA is required alongside FILE_READ_ATTRIBUTES so IopParseDevice
 * routes through Vpb->DeviceObject instead of taking the DirectOpen bypass.
 */
void WinLvmRemountLvVolumeIfMounted(PDEVICE_OBJECT LvPdo) {
    if (!LvPdo || KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    WINLVM_LV_EXT *lvext = (WINLVM_LV_EXT *)LvPdo->DeviceExtension;
    if (!lvext || lvext->devname[0] == 0) return;

    if (WinLvmIsVolumeMounted(LvPdo)) {
        UNICODE_STRING dev_name;
        RtlInitUnicodeString(&dev_name, lvext->devname);
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, &dev_name,
                                   OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                                   NULL, NULL);
        HANDLE vh = NULL;
        IO_STATUS_BLOCK iosb;
        RtlZeroMemory(&iosb, sizeof(iosb));
        NTSTATUS st = ZwCreateFile(
            &vh, SYNCHRONIZE | FILE_READ_DATA | FILE_READ_ATTRIBUTES, &oa, &iosb, NULL,
            FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
        if (NT_SUCCESS(st) && vh) {
            RtlZeroMemory(&iosb, sizeof(iosb));
            NTSTATUS dst = ZwFsControlFile(
                vh, NULL, NULL, NULL, &iosb,
                FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0);
            WL_TRACE_2("remount dismount %wZ -> 0x%08x", &dev_name, (unsigned)dst);
            ZwClose(vh);
        } else {
            WL_TRACE_2("remount open %wZ -> 0x%08x", &dev_name, (unsigned)st);
        }
    }
    WinLvmLvSetInterfaceEnabled(LvPdo, FALSE);
    WinLvmLvSetInterfaceEnabled(LvPdo, TRUE);
}

void WinLvmTriggerLvVolumeMount(PDEVICE_OBJECT LvPdo) {
    if (!LvPdo || KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    WINLVM_LV_EXT *lvext = (WINLVM_LV_EXT *)LvPdo->DeviceExtension;
    if (!lvext || lvext->devname[0] == 0) return;
    if (WinLvmIsVolumeMounted(LvPdo)) return;

    UNICODE_STRING dev_name;
    RtlInitUnicodeString(&dev_name, lvext->devname);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &dev_name,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);
    HANDLE vh = NULL;
    IO_STATUS_BLOCK iosb;
    RtlZeroMemory(&iosb, sizeof(iosb));
    NTSTATUS st = ZwCreateFile(
        &vh, SYNCHRONIZE | FILE_READ_DATA | FILE_READ_ATTRIBUTES, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (NT_SUCCESS(st) && vh) ZwClose(vh);
    WL_TRACE_2("remount trigger %wZ -> 0x%08x", &dev_name, (unsigned)st);
}
#endif
