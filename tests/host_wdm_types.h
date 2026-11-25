/*
 * host_wdm_types.h — Freestanding Windows Driver Kit (WDK) type compatibility shim
 * for building and unit-testing Windows storage surface IOCTL drivers on host Linux.
 *
 * Strictly defines official WDK binary struct layouts, status codes, and IOCTL/FSCTL
 * control codes for CONTRACT-STORAGE-SURFACE-001.
 */
#ifndef HOST_WDM_TYPES_H
#define HOST_WDM_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "winlvm_vg.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201)
#endif

/* -------------------------------------------------------------------------
 * Basic NT Types & Constants
 * ------------------------------------------------------------------------- */
typedef int32_t   NTSTATUS;
typedef uint32_t  ULONG;
typedef uint16_t  USHORT;
typedef uint8_t   UCHAR;
typedef char      CHAR;
typedef uint16_t  WCHAR;
typedef uint8_t   BOOLEAN;
typedef int32_t   LONG;
typedef int64_t   LONGLONG;
typedef uint64_t  ULONGLONG;
typedef void     *PVOID;
typedef uint8_t  *PUCHAR;
typedef uint8_t  *PBOOLEAN;
typedef uint32_t  DEVICE_TYPE;
typedef int16_t   CSHORT;
typedef int8_t    CCHAR;
typedef uintptr_t ULONG_PTR;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) ((void)(P))
#endif

#ifndef FIELD_OFFSET
#define FIELD_OFFSET(type, field) offsetof(type, field)
#endif

#ifndef RtlZeroMemory
#define RtlZeroMemory(Destination, Length) memset((Destination), 0, (Length))
#endif

#ifndef RtlCopyMemory
#define RtlCopyMemory(Destination, Source, Length) memcpy((Destination), (Source), (Length))
#endif

/* -------------------------------------------------------------------------
 * NTSTATUS Codes
 * ------------------------------------------------------------------------- */
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL          ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_BUFFER_OVERFLOW
#define STATUS_BUFFER_OVERFLOW           ((NTSTATUS)0x80000005L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER         ((NTSTATUS)0xC000000DL)
#endif
#ifndef STATUS_INVALID_DEVICE_REQUEST
#define STATUS_INVALID_DEVICE_REQUEST    ((NTSTATUS)0xC0000010L)
#endif
#ifndef STATUS_NOT_SUPPORTED
#define STATUS_NOT_SUPPORTED             ((NTSTATUS)0xC00000BBL)
#endif
#ifndef STATUS_NOT_IMPLEMENTED
#define STATUS_NOT_IMPLEMENTED           ((NTSTATUS)0xC0000002L)
#endif
#ifndef STATUS_MEDIA_WRITE_PROTECTED
#define STATUS_MEDIA_WRITE_PROTECTED     ((NTSTATUS)0xC00000A2L)
#endif
#ifndef STATUS_DEVICE_REMOVED
#define STATUS_DEVICE_REMOVED            ((NTSTATUS)0xC00002B6L)
#endif
#ifndef STATUS_INSUFFICIENT_RESOURCES
#define STATUS_INSUFFICIENT_RESOURCES    ((NTSTATUS)0xC000009AL)
#endif
#ifndef STATUS_DEVICE_BUSY
#define STATUS_DEVICE_BUSY               ((NTSTATUS)0x80000011L)
#endif
#ifndef STATUS_PENDING
#define STATUS_PENDING                   ((NTSTATUS)0x00000103L)
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

/* -------------------------------------------------------------------------
 * Unicode String & Large Integer & GUID
 * ------------------------------------------------------------------------- */
typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    WCHAR *Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

static inline void RtlInitUnicodeString(UNICODE_STRING *dest, const WCHAR *src) {
    if (!src) {
        dest->Length = 0;
        dest->MaximumLength = 0;
        dest->Buffer = NULL;
        return;
    }
    size_t len = 0;
    while (src[len]) len++;
    dest->Length = (USHORT)(len * sizeof(WCHAR));
    dest->MaximumLength = (USHORT)((len + 1) * sizeof(WCHAR));
    dest->Buffer = (WCHAR *)src;
}

typedef union _LARGE_INTEGER {
    struct {
        ULONG LowPart;
        LONG  HighPart;
    } DUMMYSTRUCTNAME;
    struct {
        ULONG LowPart;
        LONG  HighPart;
    } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} GUID, *PGUID;

/* -------------------------------------------------------------------------
 * IOCTL Macros & Codes
 * ------------------------------------------------------------------------- */
#ifndef CTL_CODE
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif

#define FILE_DEVICE_DISK               0x00000007
#define FILE_DEVICE_MASS_STORAGE       0x0000002d
#define IOCTL_STORAGE_BASE             FILE_DEVICE_MASS_STORAGE
#define IOCTL_DISK_BASE                FILE_DEVICE_DISK
#define IOCTL_VOLUME_BASE              0x00000056
#define MOUNTDEV_IOCTL                 0x0000004d
#define FILE_DEVICE_FILE_SYSTEM        0x00000009

#define METHOD_BUFFERED                0
#define METHOD_NEITHER                 3
#define FILE_ANY_ACCESS                0
#define FILE_READ_ACCESS               0x0001
#define FILE_WRITE_ACCESS              0x0002

#define IOCTL_DISK_GET_DRIVE_GEOMETRY      CTL_CODE(IOCTL_DISK_BASE, 0x0000, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DISK_GET_PARTITION_INFO      CTL_CODE(IOCTL_DISK_BASE, 0x0001, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_DISK_GET_DRIVE_LAYOUT        CTL_CODE(IOCTL_DISK_BASE, 0x0003, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_DISK_GET_DRIVE_GEOMETRY_EX   CTL_CODE(IOCTL_DISK_BASE, 0x0028, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DISK_GET_PARTITION_INFO_EX   CTL_CODE(IOCTL_DISK_BASE, 0x0012, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DISK_GET_DRIVE_LAYOUT_EX     CTL_CODE(IOCTL_DISK_BASE, 0x0014, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DISK_GET_LENGTH_INFO         CTL_CODE(IOCTL_DISK_BASE, 0x0017, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_DISK_IS_WRITABLE             CTL_CODE(IOCTL_DISK_BASE, 0x0009, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_DISK_CHECK_VERIFY            CTL_CODE(IOCTL_DISK_BASE, 0x0200, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_STORAGE_CHECK_VERIFY         CTL_CODE(IOCTL_STORAGE_BASE, 0x0200, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_STORAGE_GET_DEVICE_NUMBER    CTL_CODE(IOCTL_STORAGE_BASE, 0x0420, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_STORAGE_QUERY_PROPERTY       CTL_CODE(IOCTL_STORAGE_BASE, 0x0500, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS CTL_CODE(IOCTL_VOLUME_BASE, 0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VOLUME_ONLINE                CTL_CODE(IOCTL_VOLUME_BASE, 2, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VOLUME_POST_ONLINE           CTL_CODE(IOCTL_VOLUME_BASE, 4, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VOLUME_GET_GPT_ATTRIBUTES    CTL_CODE(IOCTL_VOLUME_BASE, 12, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VOLUME_IS_DYNAMIC            CTL_CODE(IOCTL_VOLUME_BASE, 14, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_MOUNTDEV_QUERY_UNIQUE_ID     CTL_CODE(MOUNTDEV_IOCTL, 0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MOUNTDEV_QUERY_DEVICE_NAME   CTL_CODE(MOUNTDEV_IOCTL, 2, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME CTL_CODE(MOUNTDEV_IOCTL, 3, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MOUNTDEV_LINK_CREATED        CTL_CODE(MOUNTDEV_IOCTL, 4, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MOUNTDEV_LINK_DELETED        CTL_CODE(MOUNTDEV_IOCTL, 5, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MOUNTDEV_QUERY_STABLE_GUID   CTL_CODE(MOUNTDEV_IOCTL, 6, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define FSCTL_LOCK_VOLUME                  CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 6, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_UNLOCK_VOLUME                CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 7, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_DISMOUNT_VOLUME              CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 8, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_ALLOW_EXTENDED_DASD_IO       CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 32, METHOD_NEITHER, FILE_ANY_ACCESS)

/* -------------------------------------------------------------------------
 * Enums
 * ------------------------------------------------------------------------- */
typedef enum _MEDIA_TYPE {
    Unknown = 0,
    FixedMedia = 12,
} MEDIA_TYPE;

typedef enum _STORAGE_BUS_TYPE {
    BusTypeUnknown = 0x00,
    BusTypeVirtual = 0x0E,
} STORAGE_BUS_TYPE;

typedef enum _STORAGE_PROPERTY_ID {
    StorageDeviceProperty = 0,
    StorageAdapterProperty = 1,
    StorageDeviceIdProperty = 2,
    StorageDeviceUniqueIdProperty = 3,
    StorageDeviceWriteCacheProperty = 4,
    StorageMiniportProperty = 5,
    StorageAccessAlignmentProperty = 6,
    StorageDeviceSeekPenaltyProperty = 7,
} STORAGE_PROPERTY_ID;

typedef enum _STORAGE_QUERY_TYPE {
    PropertyStandardQuery = 0,
    PropertyExistsQuery = 1,
} STORAGE_QUERY_TYPE;

typedef enum _PARTITION_STYLE {
    PARTITION_STYLE_MBR = 0,
    PARTITION_STYLE_GPT = 1,
    PARTITION_STYLE_RAW = 2,
} PARTITION_STYLE;

/* -------------------------------------------------------------------------
 * Storage Structures
 * ------------------------------------------------------------------------- */
typedef struct _GET_LENGTH_INFORMATION {
    LARGE_INTEGER Length;
} GET_LENGTH_INFORMATION, *PGET_LENGTH_INFORMATION;

typedef struct _DISK_GEOMETRY {
    LARGE_INTEGER Cylinders;
    MEDIA_TYPE    MediaType;
    ULONG         TracksPerCylinder;
    ULONG         SectorsPerTrack;
    ULONG         BytesPerSector;
} DISK_GEOMETRY, *PDISK_GEOMETRY;

typedef struct _DISK_GEOMETRY_EX {
    DISK_GEOMETRY Geometry;
    LARGE_INTEGER DiskSize;
    UCHAR         Data[1];
} DISK_GEOMETRY_EX, *PDISK_GEOMETRY_EX;

typedef struct _PARTITION_INFORMATION_MBR {
    UCHAR   PartitionType;
    BOOLEAN BootIndicator;
    BOOLEAN RecognizedPartition;
    ULONG   HiddenSectors;
    GUID    PartitionId;
} PARTITION_INFORMATION_MBR, *PPARTITION_INFORMATION_MBR;

typedef struct _PARTITION_INFORMATION_GPT {
    GUID      PartitionType;
    GUID      PartitionId;
    ULONGLONG Attributes;
    WCHAR     Name[36];
} PARTITION_INFORMATION_GPT, *PPARTITION_INFORMATION_GPT;

typedef struct _PARTITION_INFORMATION {
    LARGE_INTEGER StartingOffset;
    LARGE_INTEGER PartitionLength;
    ULONG         HiddenSectors;
    ULONG         PartitionNumber;
    UCHAR         PartitionType;
    BOOLEAN       BootIndicator;
    BOOLEAN       RecognizedPartition;
    BOOLEAN       RewritePartition;
} PARTITION_INFORMATION, *PPARTITION_INFORMATION;

typedef struct _PARTITION_INFORMATION_EX {
    PARTITION_STYLE PartitionStyle;
    LARGE_INTEGER   StartingOffset;
    LARGE_INTEGER   PartitionLength;
    ULONG           PartitionNumber;
    BOOLEAN         RewritePartition;
    BOOLEAN         IsServicePartition;
    union {
        PARTITION_INFORMATION_MBR Mbr;
        PARTITION_INFORMATION_GPT Gpt;
    };
} PARTITION_INFORMATION_EX, *PPARTITION_INFORMATION_EX;

typedef struct _DRIVE_LAYOUT_INFORMATION_MBR {
    ULONG Signature;
    ULONG CheckSum;
} DRIVE_LAYOUT_INFORMATION_MBR, *PDRIVE_LAYOUT_INFORMATION_MBR;

typedef struct _DRIVE_LAYOUT_INFORMATION_GPT {
    GUID          DiskId;
    LARGE_INTEGER StartingUsableOffset;
    LARGE_INTEGER UsableLength;
    ULONG         MaxPartitionCount;
} DRIVE_LAYOUT_INFORMATION_GPT, *PDRIVE_LAYOUT_INFORMATION_GPT;

typedef struct _DRIVE_LAYOUT_INFORMATION {
    ULONG                 PartitionCount;
    ULONG                 Signature;
    PARTITION_INFORMATION PartitionEntry[1];
} DRIVE_LAYOUT_INFORMATION, *PDRIVE_LAYOUT_INFORMATION;

typedef struct _DRIVE_LAYOUT_INFORMATION_EX {
    ULONG PartitionStyle;
    ULONG PartitionCount;
    union {
        DRIVE_LAYOUT_INFORMATION_MBR Mbr;
        DRIVE_LAYOUT_INFORMATION_GPT Gpt;
    };
    PARTITION_INFORMATION_EX PartitionEntry[1];
} DRIVE_LAYOUT_INFORMATION_EX, *PDRIVE_LAYOUT_INFORMATION_EX;

typedef struct _STORAGE_PROPERTY_QUERY {
    STORAGE_PROPERTY_ID PropertyId;
    STORAGE_QUERY_TYPE  QueryType;
    UCHAR               AdditionalParameters[1];
} STORAGE_PROPERTY_QUERY, *PSTORAGE_PROPERTY_QUERY;

typedef struct _STORAGE_DESCRIPTOR_HEADER {
    ULONG Version;
    ULONG Size;
} STORAGE_DESCRIPTOR_HEADER, *PSTORAGE_DESCRIPTOR_HEADER;

typedef struct _STORAGE_DEVICE_DESCRIPTOR {
    ULONG            Version;
    ULONG            Size;
    UCHAR            DeviceType;
    UCHAR            DeviceTypeModifier;
    BOOLEAN          RemovableMedia;
    BOOLEAN          CommandQueueing;
    ULONG            VendorIdOffset;
    ULONG            ProductIdOffset;
    ULONG            ProductRevisionOffset;
    ULONG            SerialNumberOffset;
    STORAGE_BUS_TYPE BusType;
    ULONG            RawPropertiesLength;
    UCHAR            RawDeviceProperties[1];
} STORAGE_DEVICE_DESCRIPTOR, *PSTORAGE_DEVICE_DESCRIPTOR;

typedef struct _STORAGE_ADAPTER_DESCRIPTOR {
    ULONG   Version;
    ULONG   Size;
    ULONG   MaximumTransferLength;
    ULONG   MaximumPhysicalPages;
    ULONG   AlignmentMask;
    BOOLEAN AdapterUsesPio;
    BOOLEAN AdapterScansDown;
    BOOLEAN CommandQueueing;
    BOOLEAN AcceleratedTransfer;
    UCHAR   BusType;
    USHORT  BusMajorVersion;
    USHORT  BusMinorVersion;
} STORAGE_ADAPTER_DESCRIPTOR, *PSTORAGE_ADAPTER_DESCRIPTOR;

typedef struct _STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR {
    ULONG Version;
    ULONG Size;
    ULONG BytesPerCacheLine;
    ULONG BytesOffsetForCacheAlignment;
    ULONG BytesPerLogicalSector;
    ULONG BytesPerPhysicalSector;
    ULONG BytesOffsetForSectorAlignment;
} STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR, *PSTORAGE_ACCESS_ALIGNMENT_DESCRIPTOR;

typedef struct _DEVICE_SEEK_PENALTY_DESCRIPTOR {
    ULONG   Version;
    ULONG   Size;
    BOOLEAN IncursSeekPenalty;
} DEVICE_SEEK_PENALTY_DESCRIPTOR, *PDEVICE_SEEK_PENALTY_DESCRIPTOR;

typedef struct _STORAGE_DEVICE_NUMBER {
    DEVICE_TYPE DeviceType;
    ULONG       DeviceNumber;
    ULONG       PartitionNumber;
} STORAGE_DEVICE_NUMBER, *PSTORAGE_DEVICE_NUMBER;

typedef struct _DISK_EXTENT {
    ULONG         DiskNumber;
    LARGE_INTEGER StartingOffset;
    LARGE_INTEGER ExtentLength;
} DISK_EXTENT, *PDISK_EXTENT;

typedef struct _VOLUME_DISK_EXTENTS {
    ULONG       NumberOfDiskExtents;
    DISK_EXTENT Extents[1];
} VOLUME_DISK_EXTENTS, *PVOLUME_DISK_EXTENTS;

typedef struct _VOLUME_GET_GPT_ATTRIBUTES_INFORMATION {
    ULONGLONG GptAttributes;
} VOLUME_GET_GPT_ATTRIBUTES_INFORMATION, *PVOLUME_GET_GPT_ATTRIBUTES_INFORMATION;

typedef struct _MOUNTDEV_NAME {
    USHORT NameLength;
    WCHAR  Name[1];
} MOUNTDEV_NAME, *PMOUNTDEV_NAME;

typedef struct _MOUNTDEV_UNIQUE_ID {
    USHORT UniqueIdLength;
    UCHAR  UniqueId[1];
} MOUNTDEV_UNIQUE_ID, *PMOUNTDEV_UNIQUE_ID;

typedef struct _MOUNTDEV_SUGGESTED_LINK_NAME {
    BOOLEAN UseOnlyIfThereAreNoOtherLinks;
    USHORT  NameLength;
    WCHAR   Name[1];
} MOUNTDEV_SUGGESTED_LINK_NAME, *PMOUNTDEV_SUGGESTED_LINK_NAME;

typedef struct _MOUNTDEV_STABLE_GUID {
    GUID StableGuid;
} MOUNTDEV_STABLE_GUID, *PMOUNTDEV_STABLE_GUID;

/* -------------------------------------------------------------------------
 * Power Management Types & Constants
 * ------------------------------------------------------------------------- */
#define IRP_MN_WAIT_WAKE       0x00
#define IRP_MN_POWER_SEQUENCE  0x01
#define IRP_MN_SET_POWER       0x02
#define IRP_MN_QUERY_POWER     0x03

#define IO_NO_INCREMENT        0

typedef UCHAR KIRQL;
#define PASSIVE_LEVEL          0
#define APC_LEVEL              1
#define DISPATCH_LEVEL         2

typedef enum _SYSTEM_POWER_STATE {
    PowerSystemUnspecified = 0,
    PowerSystemWorking     = 1,
    PowerSystemSleeping1   = 2,
    PowerSystemSleeping2   = 3,
    PowerSystemSleeping3   = 4,
    PowerSystemHibernate   = 5,
    PowerSystemShutdown    = 6,
    PowerSystemMaximum     = 7
} SYSTEM_POWER_STATE, *PSYSTEM_POWER_STATE;

typedef enum _DEVICE_POWER_STATE {
    PowerDeviceUnspecified = 0,
    PowerDeviceD0          = 1,
    PowerDeviceD1          = 2,
    PowerDeviceD2          = 3,
    PowerDeviceD3          = 4,
    PowerDeviceMaximum     = 5
} DEVICE_POWER_STATE, *PDEVICE_POWER_STATE;

typedef enum _POWER_STATE_TYPE {
    SystemPowerState = 0,
    DevicePowerState = 1
} POWER_STATE_TYPE, *PPOWER_STATE_TYPE;

typedef union _POWER_STATE {
    SYSTEM_POWER_STATE SystemState;
    DEVICE_POWER_STATE DeviceState;
} POWER_STATE, *PPOWER_STATE;

typedef enum _POWER_ACTION {
    PowerActionNone = 0,
    PowerActionReserved,
    PowerActionSleep,
    PowerActionHibernate,
    PowerActionShutdown,
    PowerActionShutdownReset,
    PowerActionShutdownOff,
    PowerActionWarmEject
} POWER_ACTION, *PPOWER_ACTION;

/* -------------------------------------------------------------------------
 * I/O Manager Structures
 * ------------------------------------------------------------------------- */
typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID    Pointer;
    };
    ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _IO_STACK_LOCATION {
    UCHAR MajorFunction;
    UCHAR MinorFunction;
    UCHAR Flags;
    UCHAR Control;
    union {
        struct {
            ULONG OutputBufferLength;
            ULONG InputBufferLength;
            ULONG IoControlCode;
            PVOID Type3InputBuffer;
        } DeviceIoControl;
        struct {
            ULONG            SystemContext;
            POWER_STATE_TYPE Type;
            POWER_STATE      State;
            POWER_ACTION     ShutdownType;
        } Power;
        struct {
            PVOID FileObject;
        };
    } Parameters;
    PVOID DeviceObject;
    PVOID FileObject;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;

typedef struct _IRP {
    CSHORT Type;
    USHORT Size;
    union {
        struct _IRP *MasterIrp;
        LONG        IrpCount;
        PVOID       SystemBuffer;
    } AssociatedIrp;
    IO_STATUS_BLOCK IoStatus;
    BOOLEAN         PendingReturned;
    PIO_STACK_LOCATION CurrentStackLocation;
} IRP, *PIRP;

#define IoGetCurrentIrpStackLocation(Irp) ((Irp)->CurrentStackLocation)

typedef struct _DEVICE_OBJECT {
    PVOID DeviceExtension;
    ULONG Flags;
    CCHAR StackSize;
    USHORT SectorSize;
    ULONG AlignmentRequirement;
} DEVICE_OBJECT, *PDEVICE_OBJECT;

/* -------------------------------------------------------------------------
 * Driver Extension Structure
 * ------------------------------------------------------------------------- */
typedef enum {
    WL_EXT_BUS = 1,
    WL_EXT_LV = 2,
    WL_EXT_CONTROL = 3,
} WL_EXT_KIND;

typedef struct _WINLVM_BUS_EXT {
    WL_EXT_KIND        kind;
    PDEVICE_OBJECT     fdo;
    PDEVICE_OBJECT     pdo;
    PDEVICE_OBJECT     bus_pdo;
    SYSTEM_POWER_STATE system_power_state;
    DEVICE_POWER_STATE device_power_state;
} WINLVM_BUS_EXT, *PWINLVM_BUS_EXT;

typedef struct _WINLVM_LV_EXT {
    WL_EXT_KIND         kind;
    PDEVICE_OBJECT      pdo;
    struct wl_vg_record *record;
    size_t              lv_index;
    PDEVICE_OBJECT      backing_pdo[WL_VG_MAX_PVS];
    PVOID               backing_file[WL_VG_MAX_PVS];
    ULONG               backing_logical[WL_VG_MAX_PVS];
    ULONG               backing_physical[WL_VG_MAX_PVS];
    uint32_t            pv_slot[WL_VG_MAX_PVS];
    size_t              backing_count;
    ULONG               logical_sector;
    ULONG               physical_sector;
    ULONGLONG           byte_size;
    BOOLEAN             read_only;
    UNICODE_STRING      ifname;
    WCHAR               devname[64];
    WCHAR               drive_letter;
    SYSTEM_POWER_STATE  system_power_state;
    DEVICE_POWER_STATE  device_power_state;
} WINLVM_LV_EXT, *PWINLVM_LV_EXT;

/* -------------------------------------------------------------------------
 * Function Signatures
 * ------------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

NTSTATUS WinLvmStorageIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp, WINLVM_LV_EXT *ext);
NTSTATUS WinLvmMountIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp, WINLVM_LV_EXT *ext);
NTSTATUS WinLvmPower(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS WinLvmFlush(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS WinLvmShutdown(PDEVICE_OBJECT DeviceObject, PIRP Irp);

#ifdef __cplusplus
}
#endif

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif /* HOST_WDM_TYPES_H */
