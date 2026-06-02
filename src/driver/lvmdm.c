/*
 * lvmdm.c - Windows Kernel Block-Mapping Driver for LVM Logical Volumes
 *
 * This is a SKELETON/PROTOTYPE driver demonstrating the architecture.
 * Production implementation requires:
 * - Full Storport miniport integration
 * - Proper PnP/Power management
 * - Robust error handling
 * - Performance optimization
 */

#include <ntddk.h>
#include <ntdddisk.h>
#include <ntddstor.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include "lvmdm_ioctl.h"

// ============================================================================
// Driver Globals
// ============================================================================

typedef struct _LVMDM_DEVICE_EXTENSION {
  ULONG DeviceKind;
  PDEVICE_OBJECT DeviceObject;
  UNICODE_STRING DeviceName;
  UNICODE_STRING SymbolicLink;
  KSPIN_LOCK MappingLock;
  LIST_ENTRY MappingList;
  ULONGLONG NextMappingId;
  struct _LVMDM_MAPPING_ENTRY* Mapping;
} LVMDM_DEVICE_EXTENSION, *PLVMDM_DEVICE_EXTENSION;

#define LVMDM_DEVICE_KIND_CONTROL 1UL
#define LVMDM_DEVICE_KIND_VOLUME 2UL

typedef struct _LVMDM_MAPPING_ENTRY {
  LIST_ENTRY ListEntry;
  ULONGLONG MappingId;
  PVOID MappingTableBuffer;
  SIZE_T MappingTableSize;
  PDEVICE_OBJECT VirtualDiskDevice;
  WCHAR VirtualDeviceNameBuffer[64];
  WCHAR VirtualSymbolicLinkBuffer[64];
  UNICODE_STRING VirtualDeviceName;
  UNICODE_STRING VirtualSymbolicLink;
  ULONGLONG IoCount;
} LVMDM_MAPPING_ENTRY, *PLVMDM_MAPPING_ENTRY;

typedef struct _LVMDM_TRANSLATED_RANGE {
  const LVMDM_MAPPING_PV_HANDLE* PvHandle;
  ULONGLONG SourceOffsetBytes;
  ULONGLONG PvOffsetBytes;
  ULONGLONG LengthBytes;
} LVMDM_TRANSLATED_RANGE, *PLVMDM_TRANSLATED_RANGE;

// ============================================================================
// Forward Declarations
// ============================================================================

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD LvmdmUnload;

_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLOSE)
DRIVER_DISPATCH LvmdmCreateClose;

_Dispatch_type_(IRP_MJ_READ)
_Dispatch_type_(IRP_MJ_WRITE)
DRIVER_DISPATCH LvmdmUnsupportedReadWrite;

_Dispatch_type_(IRP_MJ_READ)
DRIVER_DISPATCH LvmdmRead;

_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH LvmdmDeviceControl;

NTSTATUS LvmdmCreateMapping(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  PVOID InputBuffer,
  ULONG InputBufferLength,
  PULONGLONG MappingId
);

NTSTATUS LvmdmDeleteMapping(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  ULONGLONG MappingId
);

NTSTATUS LvmdmQueryMapping(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  ULONGLONG MappingId,
  PLVMDM_QUERY_MAPPING_OUTPUT OutputBuffer,
  ULONG OutputBufferLength,
  PULONG Information
);

NTSTATUS LvmdmListMappings(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  PVOID OutputBuffer,
  ULONG OutputBufferLength,
  PULONG Information
);

NTSTATUS LvmdmTranslateRange(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  const LVMDM_TRANSLATE_RANGE_INPUT* Input,
  PLVMDM_TRANSLATE_RANGE_OUTPUT OutputBuffer,
  ULONG OutputBufferLength,
  PULONG Information
);

NTSTATUS LvmdmCreateVirtualDisk(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  PLVMDM_MAPPING_ENTRY Mapping
);

VOID LvmdmDeleteVirtualDisk(
  PLVMDM_MAPPING_ENTRY Mapping
);

NTSTATUS LvmdmVirtualDeviceControl(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  PIRP Irp,
  PIO_STACK_LOCATION IrpStack
);

NTSTATUS LvmdmReadVirtualDisk(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  PIRP Irp,
  PIO_STACK_LOCATION IrpStack
);

NTSTATUS LvmdmReadBackingSource(
  const CHAR* SourcePath,
  ULONGLONG SourceOffsetBytes,
  PVOID Buffer,
  ULONG Length
);

// ============================================================================
// Driver Entry Point
// ============================================================================

NTSTATUS DriverEntry(
  _In_ PDRIVER_OBJECT DriverObject,
  _In_ PUNICODE_STRING RegistryPath
) {
  NTSTATUS status;
  PDEVICE_OBJECT deviceObject = NULL;
  PLVMDM_DEVICE_EXTENSION deviceExtension;
  UNICODE_STRING deviceName;
  UNICODE_STRING symbolicLink;

  UNREFERENCED_PARAMETER(RegistryPath);

  KdPrint(("LVMDM: DriverEntry called\n"));

  // Initialize device name
  RtlInitUnicodeString(&deviceName, L"\\Device\\lvmdm");
  RtlInitUnicodeString(&symbolicLink, L"\\DosDevices\\lvmdm");

  // Create control device
  status = IoCreateDevice(
    DriverObject,
    sizeof(LVMDM_DEVICE_EXTENSION),
    &deviceName,
    FILE_DEVICE_UNKNOWN,
    FILE_DEVICE_SECURE_OPEN,
    FALSE,
    &deviceObject
  );

  if (!NT_SUCCESS(status)) {
    KdPrint(("LVMDM: IoCreateDevice failed: 0x%08X\n", status));
    return status;
  }

  // Initialize device extension
  deviceExtension = (PLVMDM_DEVICE_EXTENSION)deviceObject->DeviceExtension;
  RtlZeroMemory(deviceExtension, sizeof(LVMDM_DEVICE_EXTENSION));
  deviceExtension->DeviceKind = LVMDM_DEVICE_KIND_CONTROL;
  deviceExtension->DeviceObject = deviceObject;
  deviceExtension->DeviceName = deviceName;
  deviceExtension->SymbolicLink = symbolicLink;
  KeInitializeSpinLock(&deviceExtension->MappingLock);
  InitializeListHead(&deviceExtension->MappingList);
  deviceExtension->NextMappingId = 1;

  // Create symbolic link
  status = IoCreateSymbolicLink(&symbolicLink, &deviceName);
  if (!NT_SUCCESS(status)) {
    KdPrint(("LVMDM: IoCreateSymbolicLink failed: 0x%08X\n", status));
    IoDeleteDevice(deviceObject);
    return status;
  }

  // Set dispatch routines
  DriverObject->MajorFunction[IRP_MJ_CREATE] = LvmdmCreateClose;
  DriverObject->MajorFunction[IRP_MJ_CLOSE] = LvmdmCreateClose;
  DriverObject->MajorFunction[IRP_MJ_READ] = LvmdmRead;
  DriverObject->MajorFunction[IRP_MJ_WRITE] = LvmdmUnsupportedReadWrite;
  DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = LvmdmDeviceControl;
  DriverObject->DriverUnload = LvmdmUnload;

  deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

  KdPrint(("LVMDM: Driver loaded successfully\n"));
  return STATUS_SUCCESS;
}

// ============================================================================
// Driver Unload
// ============================================================================

VOID LvmdmUnload(_In_ PDRIVER_OBJECT DriverObject) {
  PDEVICE_OBJECT deviceObject = DriverObject->DeviceObject;
  PLVMDM_DEVICE_EXTENSION deviceExtension;

  KdPrint(("LVMDM: DriverUnload called\n"));

  if (deviceObject) {
    deviceExtension = (PLVMDM_DEVICE_EXTENSION)deviceObject->DeviceExtension;

    // Clean up symbolic link
    IoDeleteSymbolicLink(&deviceExtension->SymbolicLink);

    while (!IsListEmpty(&deviceExtension->MappingList)) {
      PLIST_ENTRY entry = RemoveHeadList(&deviceExtension->MappingList);
      PLVMDM_MAPPING_ENTRY mapping = CONTAINING_RECORD(entry, LVMDM_MAPPING_ENTRY, ListEntry);
      if (mapping->MappingTableBuffer) {
        ExFreePoolWithTag(mapping->MappingTableBuffer, 'mLvL');
      }
      LvmdmDeleteVirtualDisk(mapping);
      ExFreePoolWithTag(mapping, 'mLvL');
    }

    // Delete device
    IoDeleteDevice(deviceObject);
  }

  KdPrint(("LVMDM: Driver unloaded\n"));
}

// ============================================================================
// Create/Close Handlers
// ============================================================================

NTSTATUS LvmdmCreateClose(
  _In_ PDEVICE_OBJECT DeviceObject,
  _In_ PIRP Irp
) {
  UNREFERENCED_PARAMETER(DeviceObject);

  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return STATUS_SUCCESS;
}

NTSTATUS LvmdmUnsupportedReadWrite(
  _In_ PDEVICE_OBJECT DeviceObject,
  _In_ PIRP Irp
) {
  PLVMDM_DEVICE_EXTENSION deviceExtension =
    (PLVMDM_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
  NTSTATUS status = STATUS_NOT_SUPPORTED;

  if (deviceExtension->DeviceKind == LVMDM_DEVICE_KIND_VOLUME) {
    status = STATUS_MEDIA_WRITE_PROTECTED;
  }

  Irp->IoStatus.Status = status;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return status;
}

NTSTATUS LvmdmRead(
  _In_ PDEVICE_OBJECT DeviceObject,
  _In_ PIRP Irp
) {
  PLVMDM_DEVICE_EXTENSION deviceExtension =
    (PLVMDM_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
  PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(Irp);
  NTSTATUS status;

  if (deviceExtension->DeviceKind != LVMDM_DEVICE_KIND_VOLUME) {
    status = STATUS_NOT_SUPPORTED;
  } else {
    status = LvmdmReadVirtualDisk(deviceExtension, Irp, irpStack);
    return status;
  }

  Irp->IoStatus.Status = status;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return status;
}

// ============================================================================
// Device Control Handler (IOCTL dispatcher)
// ============================================================================

NTSTATUS LvmdmDeviceControl(
  _In_ PDEVICE_OBJECT DeviceObject,
  _In_ PIRP Irp
) {
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
  PIO_STACK_LOCATION irpStack;
  PLVMDM_DEVICE_EXTENSION deviceExtension;
  ULONG ioControlCode;
  PVOID inputBuffer;
  PVOID outputBuffer;
  ULONG inputBufferLength;
  ULONG outputBufferLength;
  ULONG information = 0;

  deviceExtension = (PLVMDM_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
  irpStack = IoGetCurrentIrpStackLocation(Irp);
  ioControlCode = irpStack->Parameters.DeviceIoControl.IoControlCode;
  inputBuffer = Irp->AssociatedIrp.SystemBuffer;
  outputBuffer = Irp->AssociatedIrp.SystemBuffer;
  inputBufferLength = irpStack->Parameters.DeviceIoControl.InputBufferLength;
  outputBufferLength = irpStack->Parameters.DeviceIoControl.OutputBufferLength;

  if (deviceExtension->DeviceKind == LVMDM_DEVICE_KIND_VOLUME) {
    return LvmdmVirtualDeviceControl(deviceExtension, Irp, irpStack);
  }

  switch (ioControlCode) {
    case IOCTL_LVMDM_CREATE_MAPPING: {
      ULONGLONG mappingId = 0;
      status = LvmdmCreateMapping(
        deviceExtension,
        inputBuffer,
        inputBufferLength,
        &mappingId
      );
      if (NT_SUCCESS(status) && outputBufferLength >= sizeof(ULONGLONG)) {
        *(PULONGLONG)outputBuffer = mappingId;
        information = sizeof(ULONGLONG);
      } else if (NT_SUCCESS(status)) {
        status = STATUS_BUFFER_TOO_SMALL;
      }
      break;
    }

    case IOCTL_LVMDM_DELETE_MAPPING: {
      if (inputBufferLength >= sizeof(LVMDM_DELETE_MAPPING_INPUT)) {
        PLVMDM_DELETE_MAPPING_INPUT input = (PLVMDM_DELETE_MAPPING_INPUT)inputBuffer;
        status = LvmdmDeleteMapping(deviceExtension, input->mapping_id);
      } else {
        status = STATUS_BUFFER_TOO_SMALL;
      }
      break;
    }

    case IOCTL_LVMDM_QUERY_MAPPING: {
      if (inputBufferLength >= sizeof(LVMDM_QUERY_MAPPING_INPUT) &&
          outputBufferLength >= sizeof(LVMDM_QUERY_MAPPING_OUTPUT)) {
        PLVMDM_QUERY_MAPPING_INPUT input = (PLVMDM_QUERY_MAPPING_INPUT)inputBuffer;
        status = LvmdmQueryMapping(
          deviceExtension,
          input->mapping_id,
          (PLVMDM_QUERY_MAPPING_OUTPUT)outputBuffer,
          outputBufferLength,
          &information
        );
      } else {
        status = STATUS_BUFFER_TOO_SMALL;
      }
      break;
    }

    case IOCTL_LVMDM_LIST_MAPPINGS: {
      status = LvmdmListMappings(
        deviceExtension,
        outputBuffer,
        outputBufferLength,
        &information
      );
      break;
    }

    case IOCTL_LVMDM_TRANSLATE_RANGE: {
      if (inputBufferLength >= sizeof(LVMDM_TRANSLATE_RANGE_INPUT) &&
          outputBufferLength >= sizeof(LVMDM_TRANSLATE_RANGE_OUTPUT)) {
        LVMDM_TRANSLATE_RANGE_INPUT input;
        RtlCopyMemory(&input, inputBuffer, sizeof(input));
        status = LvmdmTranslateRange(
          deviceExtension,
          &input,
          (PLVMDM_TRANSLATE_RANGE_OUTPUT)outputBuffer,
          outputBufferLength,
          &information
        );
      } else {
        status = STATUS_BUFFER_TOO_SMALL;
      }
      break;
    }

    default:
      status = STATUS_INVALID_DEVICE_REQUEST;
      break;
  }

  Irp->IoStatus.Status = status;
  Irp->IoStatus.Information = information;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return status;
}

// ============================================================================
// Mapping Management
// ============================================================================

BOOLEAN LvmdmAddOverflows(ULONGLONG a, ULONGLONG b) {
  return b > (MAXULONGLONG - a);
}

BOOLEAN LvmdmRangeFits(ULONGLONG offset, ULONGLONG length, ULONGLONG limit) {
  if (LvmdmAddOverflows(offset, length)) {
    return FALSE;
  }
  return offset + length <= limit;
}

BOOLEAN LvmdmUuidEquals(
  const LVMDM_MAPPING_UUID* Left,
  const LVMDM_MAPPING_UUID* Right
) {
  return RtlCompareMemory(Left->bytes, Right->bytes, sizeof(Left->bytes)) == sizeof(Left->bytes);
}

const LVMDM_MAPPING_PV_HANDLE* LvmdmFindPvHandle(
  const LVMDM_MAPPING_PV_HANDLE* PvHandles,
  ULONG PvHandleCount,
  const LVMDM_MAPPING_UUID* PvUuid
) {
  ULONG i;
  for (i = 0; i < PvHandleCount; ++i) {
    if (LvmdmUuidEquals(&PvHandles[i].pv_uuid, PvUuid)) {
      return &PvHandles[i];
    }
  }
  return NULL;
}

NTSTATUS LvmdmTranslateLogicalRange(
  PVOID MappingTableBuffer,
  ULONGLONG LvOffsetBytes,
  ULONGLONG LengthBytes,
  PLVMDM_TRANSLATED_RANGE Translation
) {
  const LVMDM_MAPPING_TABLE_HEADER* header;
  const LVMDM_MAPPING_SEGMENT* segments;
  const LVMDM_MAPPING_PV_HANDLE* pvHandles;
  ULONG i;

  if (!MappingTableBuffer || !Translation || LengthBytes == 0 ||
      LvmdmAddOverflows(LvOffsetBytes, LengthBytes)) {
    return STATUS_INVALID_PARAMETER;
  }

  header = (const LVMDM_MAPPING_TABLE_HEADER*)MappingTableBuffer;
  if (LvOffsetBytes >= header->lv_size_bytes ||
      LvOffsetBytes + LengthBytes > header->lv_size_bytes) {
    return STATUS_INVALID_PARAMETER;
  }

  segments = (const LVMDM_MAPPING_SEGMENT*)((const UCHAR*)MappingTableBuffer + header->segments_offset);
  pvHandles = (const LVMDM_MAPPING_PV_HANDLE*)((const UCHAR*)MappingTableBuffer + header->pv_handles_offset);

  for (i = 0; i < header->segment_count; ++i) {
    const LVMDM_MAPPING_SEGMENT* segment = &segments[i];
    const LVMDM_MAPPING_LINEAR_SEGMENT* linear = &segment->data.linear;
    ULONGLONG segmentEnd;
    ULONGLONG withinSegment;
    ULONGLONG pvOffset;
    ULONGLONG sourceOffset;
    ULONGLONG available;
    ULONGLONG translatedLength;
    const LVMDM_MAPPING_PV_HANDLE* pv;

    if (segment->type != LvmdmSegmentLinear ||
        LvmdmAddOverflows(linear->lv_start_bytes, linear->length_bytes)) {
      return STATUS_INVALID_PARAMETER;
    }

    segmentEnd = linear->lv_start_bytes + linear->length_bytes;
    if (LvOffsetBytes < linear->lv_start_bytes ||
        LvOffsetBytes >= segmentEnd) {
      continue;
    }

    withinSegment = LvOffsetBytes - linear->lv_start_bytes;
    if (LvmdmAddOverflows(linear->pv_start_bytes, withinSegment)) {
      return STATUS_INTEGER_OVERFLOW;
    }
    pvOffset = linear->pv_start_bytes + withinSegment;

    pv = LvmdmFindPvHandle(pvHandles, header->pv_handle_count, &linear->pv_uuid);
    if (!pv || !LvmdmRangeFits(pvOffset, 1, pv->pv_size_bytes) ||
        LvmdmAddOverflows(pv->pv_base_offset_bytes, pvOffset)) {
      return STATUS_INVALID_PARAMETER;
    }
    sourceOffset = pv->pv_base_offset_bytes + pvOffset;

    available = segmentEnd - LvOffsetBytes;
    translatedLength = LengthBytes < available ? LengthBytes : available;
    if (!LvmdmRangeFits(sourceOffset, translatedLength, pv->source_size_bytes)) {
      return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Translation, sizeof(*Translation));
    Translation->PvHandle = pv;
    Translation->SourceOffsetBytes = sourceOffset;
    Translation->PvOffsetBytes = pvOffset;
    Translation->LengthBytes = translatedLength;
    return STATUS_SUCCESS;
  }

  return STATUS_INVALID_PARAMETER;
}

NTSTATUS LvmdmValidateMappingTable(
  PVOID InputBuffer,
  ULONG InputBufferLength
) {
  const LVMDM_MAPPING_TABLE_HEADER* header;
  const LVMDM_MAPPING_SEGMENT* segments;
  const LVMDM_MAPPING_PV_HANDLE* pvHandles;
  ULONGLONG segmentsSize;
  ULONGLONG pvHandlesSize;
  ULONGLONG segmentsEnd;
  ULONGLONG pvHandlesEnd;
  ULONGLONG expectedLvOffset = 0;
  ULONG i;

  if (!InputBuffer || InputBufferLength < sizeof(LVMDM_MAPPING_TABLE_HEADER)) {
    return STATUS_BUFFER_TOO_SMALL;
  }

  header = (const LVMDM_MAPPING_TABLE_HEADER*)InputBuffer;
  if (header->magic != LVMDM_MAPPING_TABLE_MAGIC ||
      header->version != LVMDM_MAPPING_TABLE_VERSION) {
    return STATUS_INVALID_PARAMETER;
  }
  if (header->total_size_bytes != InputBufferLength ||
      header->total_size_bytes < sizeof(LVMDM_MAPPING_TABLE_HEADER)) {
    return STATUS_INVALID_BUFFER_SIZE;
  }
  if (header->segment_count == 0 ||
      header->segment_count > LVMDM_MAX_SEGMENT_COUNT ||
      header->pv_handle_count == 0 ||
      header->pv_handle_count > LVMDM_MAX_PV_HANDLE_COUNT ||
      header->lv_size_bytes == 0) {
    return STATUS_INVALID_PARAMETER;
  }
  if ((header->flags & ~(LVMDM_MAPPING_FLAG_READONLY |
                         LVMDM_MAPPING_FLAG_FORCE |
                         LVMDM_MAPPING_FLAG_SHARED)) != 0) {
    return STATUS_INVALID_PARAMETER;
  }
  if (header->exposed_logical_sector_size != 512 &&
      header->exposed_logical_sector_size != 4096) {
    return STATUS_INVALID_PARAMETER;
  }
  if (header->exposed_physical_sector_size != 512 &&
      header->exposed_physical_sector_size != 4096) {
    return STATUS_INVALID_PARAMETER;
  }
  if (header->exposed_logical_sector_size > header->exposed_physical_sector_size) {
    return STATUS_INVALID_PARAMETER;
  }
  if (header->vg_name[LVMDM_MAX_VG_NAME_LENGTH] != '\0' ||
      header->lv_name[LVMDM_MAX_LV_NAME_LENGTH] != '\0') {
    return STATUS_INVALID_PARAMETER;
  }

  segmentsSize = (ULONGLONG)header->segment_count * sizeof(LVMDM_MAPPING_SEGMENT);
  pvHandlesSize = (ULONGLONG)header->pv_handle_count * sizeof(LVMDM_MAPPING_PV_HANDLE);
  if (LvmdmAddOverflows(header->segments_offset, segmentsSize) ||
      LvmdmAddOverflows(header->pv_handles_offset, pvHandlesSize)) {
    return STATUS_INTEGER_OVERFLOW;
  }
  segmentsEnd = header->segments_offset + segmentsSize;
  pvHandlesEnd = header->pv_handles_offset + pvHandlesSize;
  if (header->segments_offset < sizeof(LVMDM_MAPPING_TABLE_HEADER) ||
      header->pv_handles_offset < sizeof(LVMDM_MAPPING_TABLE_HEADER) ||
      segmentsEnd > header->total_size_bytes ||
      pvHandlesEnd > header->total_size_bytes) {
    return STATUS_INVALID_BUFFER_SIZE;
  }
  if (header->segments_offset != sizeof(LVMDM_MAPPING_TABLE_HEADER) ||
      header->pv_handles_offset != segmentsEnd) {
    return STATUS_INVALID_PARAMETER;
  }

  segments = (const LVMDM_MAPPING_SEGMENT*)((const UCHAR*)InputBuffer + header->segments_offset);
  pvHandles = (const LVMDM_MAPPING_PV_HANDLE*)((const UCHAR*)InputBuffer + header->pv_handles_offset);

  for (i = 0; i < header->pv_handle_count; ++i) {
    if (pvHandles[i].source_path[0] == '\0' ||
        pvHandles[i].source_path[LVMDM_MAX_SOURCE_PATH_LENGTH] != '\0' ||
        pvHandles[i].source_size_bytes == 0 ||
        pvHandles[i].pv_size_bytes == 0 ||
        pvHandles[i].logical_sector_size == 0 ||
        pvHandles[i].physical_sector_size == 0) {
      return STATUS_INVALID_PARAMETER;
    }
    if (!LvmdmRangeFits(
          pvHandles[i].pv_base_offset_bytes,
          pvHandles[i].pv_size_bytes,
          pvHandles[i].source_size_bytes)) {
      return STATUS_INVALID_PARAMETER;
    }
  }

  for (i = 0; i < header->segment_count; ++i) {
    const LVMDM_MAPPING_SEGMENT* segment = &segments[i];
    const LVMDM_MAPPING_LINEAR_SEGMENT* linear = &segment->data.linear;
    const LVMDM_MAPPING_PV_HANDLE* pv;

    if (segment->type != LvmdmSegmentLinear) {
      return STATUS_NOT_SUPPORTED;
    }
    if (linear->length_bytes == 0 ||
        linear->lv_start_bytes != expectedLvOffset ||
        !LvmdmRangeFits(linear->lv_start_bytes, linear->length_bytes, header->lv_size_bytes) ||
        LvmdmAddOverflows(linear->pv_start_bytes, linear->length_bytes)) {
      return STATUS_INVALID_PARAMETER;
    }
    pv = LvmdmFindPvHandle(pvHandles, header->pv_handle_count, &linear->pv_uuid);
    if (!pv || !LvmdmRangeFits(linear->pv_start_bytes, linear->length_bytes, pv->pv_size_bytes)) {
      return STATUS_INVALID_PARAMETER;
    }
    expectedLvOffset += linear->length_bytes;
  }

  if (expectedLvOffset != header->lv_size_bytes) {
    return STATUS_INVALID_PARAMETER;
  }

  return STATUS_SUCCESS;
}

PLVMDM_MAPPING_ENTRY LvmdmFindMappingLocked(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  ULONGLONG MappingId
) {
  PLIST_ENTRY current;
  for (current = DeviceExtension->MappingList.Flink;
       current != &DeviceExtension->MappingList;
       current = current->Flink) {
    PLVMDM_MAPPING_ENTRY mapping = CONTAINING_RECORD(current, LVMDM_MAPPING_ENTRY, ListEntry);
    if (mapping->MappingId == MappingId) {
      return mapping;
    }
  }
  return NULL;
}

NTSTATUS LvmdmCreateMapping(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  PVOID InputBuffer,
  ULONG InputBufferLength,
  PULONGLONG MappingId
) {
  NTSTATUS status;
  PLVMDM_MAPPING_ENTRY entry;
  PVOID tableCopy;
  KIRQL oldIrql;
  ULONGLONG mappingId;

  if (!MappingId) {
    return STATUS_INVALID_PARAMETER;
  }
  status = LvmdmValidateMappingTable(InputBuffer, InputBufferLength);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  // TODO: Create virtual disk device
  entry = (PLVMDM_MAPPING_ENTRY)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(LVMDM_MAPPING_ENTRY), 'mLvL');
  if (!entry) {
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  RtlZeroMemory(entry, sizeof(*entry));

  tableCopy = ExAllocatePool2(POOL_FLAG_NON_PAGED, InputBufferLength, 'mLvL');
  if (!tableCopy) {
    ExFreePoolWithTag(entry, 'mLvL');
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  RtlCopyMemory(tableCopy, InputBuffer, InputBufferLength);

  KeAcquireSpinLock(&DeviceExtension->MappingLock, &oldIrql);
  mappingId = DeviceExtension->NextMappingId++;
  entry->MappingId = mappingId;
  KeReleaseSpinLock(&DeviceExtension->MappingLock, oldIrql);

  status = LvmdmCreateVirtualDisk(DeviceExtension, entry);
  if (!NT_SUCCESS(status)) {
    ExFreePoolWithTag(tableCopy, 'mLvL');
    ExFreePoolWithTag(entry, 'mLvL');
    return status;
  }

  KeAcquireSpinLock(&DeviceExtension->MappingLock, &oldIrql);
  entry->MappingTableBuffer = tableCopy;
  entry->MappingTableSize = InputBufferLength;
  InsertTailList(&DeviceExtension->MappingList, &entry->ListEntry);
  *MappingId = entry->MappingId;
  KeReleaseSpinLock(&DeviceExtension->MappingLock, oldIrql);

  KdPrint(("LVMDM: Created mapping ID %I64u\n", *MappingId));
  return STATUS_SUCCESS;
}

NTSTATUS LvmdmDeleteMapping(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  ULONGLONG MappingId
) {
  PLVMDM_MAPPING_ENTRY mapping;
  KIRQL oldIrql;

  // TODO: Delete virtual disk device
  KeAcquireSpinLock(&DeviceExtension->MappingLock, &oldIrql);
  mapping = LvmdmFindMappingLocked(DeviceExtension, MappingId);
  if (!mapping) {
    KeReleaseSpinLock(&DeviceExtension->MappingLock, oldIrql);
    return STATUS_NOT_FOUND;
  }
  RemoveEntryList(&mapping->ListEntry);
  KeReleaseSpinLock(&DeviceExtension->MappingLock, oldIrql);

  LvmdmDeleteVirtualDisk(mapping);

  if (mapping->MappingTableBuffer) {
    ExFreePoolWithTag(mapping->MappingTableBuffer, 'mLvL');
  }
  ExFreePoolWithTag(mapping, 'mLvL');

  KdPrint(("LVMDM: Deleted mapping ID %I64u\n", MappingId));
  return STATUS_SUCCESS;
}

NTSTATUS LvmdmQueryMapping(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  ULONGLONG MappingId,
  PLVMDM_QUERY_MAPPING_OUTPUT OutputBuffer,
  ULONG OutputBufferLength,
  PULONG Information
) {
  PLVMDM_MAPPING_ENTRY mapping;
  KIRQL oldIrql;

  if (!OutputBuffer || OutputBufferLength < sizeof(LVMDM_QUERY_MAPPING_OUTPUT) || !Information) {
    return STATUS_BUFFER_TOO_SMALL;
  }

  KeAcquireSpinLock(&DeviceExtension->MappingLock, &oldIrql);
  mapping = LvmdmFindMappingLocked(DeviceExtension, MappingId);
  if (!mapping) {
    KeReleaseSpinLock(&DeviceExtension->MappingLock, oldIrql);
    return STATUS_NOT_FOUND;
  }
  RtlZeroMemory(OutputBuffer, sizeof(*OutputBuffer));
  OutputBuffer->mapping_id = mapping->MappingId;
  OutputBuffer->status = 0;
  OutputBuffer->io_count = mapping->IoCount;
  OutputBuffer->mapping_table_size = mapping->MappingTableSize;
  KeReleaseSpinLock(&DeviceExtension->MappingLock, oldIrql);

  *Information = sizeof(LVMDM_QUERY_MAPPING_OUTPUT);
  return STATUS_SUCCESS;
}

NTSTATUS LvmdmListMappings(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  PVOID OutputBuffer,
  ULONG OutputBufferLength,
  PULONG Information
) {
  PLVMDM_LIST_MAPPINGS_OUTPUT output;
  PLIST_ENTRY current;
  ULONG count = 0;
  ULONG capacity;
  ULONGLONG* ids;
  KIRQL oldIrql;

  if (!OutputBuffer || OutputBufferLength < sizeof(LVMDM_LIST_MAPPINGS_OUTPUT) || !Information) {
    return STATUS_BUFFER_TOO_SMALL;
  }

  RtlZeroMemory(OutputBuffer, OutputBufferLength);
  output = (PLVMDM_LIST_MAPPINGS_OUTPUT)OutputBuffer;
  capacity = (OutputBufferLength - sizeof(LVMDM_LIST_MAPPINGS_OUTPUT)) / sizeof(ULONGLONG);
  ids = (ULONGLONG*)((UCHAR*)OutputBuffer + sizeof(LVMDM_LIST_MAPPINGS_OUTPUT));

  KeAcquireSpinLock(&DeviceExtension->MappingLock, &oldIrql);
  for (current = DeviceExtension->MappingList.Flink;
       current != &DeviceExtension->MappingList;
       current = current->Flink) {
    PLVMDM_MAPPING_ENTRY mapping = CONTAINING_RECORD(current, LVMDM_MAPPING_ENTRY, ListEntry);
    if (count < capacity) {
      ids[count] = mapping->MappingId;
    }
    ++count;
  }
  KeReleaseSpinLock(&DeviceExtension->MappingLock, oldIrql);

  output->count = count;
  if (count > capacity) {
    *Information = sizeof(LVMDM_LIST_MAPPINGS_OUTPUT) + (capacity * sizeof(ULONGLONG));
    return STATUS_BUFFER_TOO_SMALL;
  }

  *Information = sizeof(LVMDM_LIST_MAPPINGS_OUTPUT) + (count * sizeof(ULONGLONG));
  return STATUS_SUCCESS;
}

NTSTATUS LvmdmTranslateRange(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  const LVMDM_TRANSLATE_RANGE_INPUT* Input,
  PLVMDM_TRANSLATE_RANGE_OUTPUT OutputBuffer,
  ULONG OutputBufferLength,
  PULONG Information
) {
  PLVMDM_MAPPING_ENTRY mapping;
  LVMDM_TRANSLATED_RANGE translation;
  NTSTATUS status;
  KIRQL oldIrql;

  if (!Input || !OutputBuffer || OutputBufferLength < sizeof(LVMDM_TRANSLATE_RANGE_OUTPUT) || !Information) {
    return STATUS_BUFFER_TOO_SMALL;
  }

  KeAcquireSpinLock(&DeviceExtension->MappingLock, &oldIrql);
  mapping = LvmdmFindMappingLocked(DeviceExtension, Input->mapping_id);
  if (!mapping) {
    KeReleaseSpinLock(&DeviceExtension->MappingLock, oldIrql);
    return STATUS_NOT_FOUND;
  }

  RtlZeroMemory(&translation, sizeof(translation));
  status = LvmdmTranslateLogicalRange(
    mapping->MappingTableBuffer,
    Input->lv_offset_bytes,
    Input->length_bytes,
    &translation
  );
  if (!NT_SUCCESS(status)) {
    KeReleaseSpinLock(&DeviceExtension->MappingLock, oldIrql);
    return status;
  }

  RtlZeroMemory(OutputBuffer, sizeof(*OutputBuffer));
  OutputBuffer->mapping_id = Input->mapping_id;
  RtlCopyMemory(OutputBuffer->source_path, translation.PvHandle->source_path, sizeof(OutputBuffer->source_path));
  OutputBuffer->pv_uuid = translation.PvHandle->pv_uuid;
  OutputBuffer->source_offset_bytes = translation.SourceOffsetBytes;
  OutputBuffer->pv_offset_bytes = translation.PvOffsetBytes;
  OutputBuffer->length_bytes = translation.LengthBytes;
  mapping->IoCount++;

  KeReleaseSpinLock(&DeviceExtension->MappingLock, oldIrql);
  *Information = sizeof(LVMDM_TRANSLATE_RANGE_OUTPUT);
  return STATUS_SUCCESS;
}

NTSTATUS LvmdmCreateVirtualDisk(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  PLVMDM_MAPPING_ENTRY Mapping
) {
  NTSTATUS status;
  PDEVICE_OBJECT deviceObject = NULL;
  PLVMDM_DEVICE_EXTENSION volumeExtension;
  const LVMDM_MAPPING_TABLE_HEADER* header;

  if (!DeviceExtension || !Mapping) {
    return STATUS_INVALID_PARAMETER;
  }
  if (!Mapping->MappingTableBuffer) {
    return STATUS_INVALID_PARAMETER;
  }
  header = (const LVMDM_MAPPING_TABLE_HEADER*)Mapping->MappingTableBuffer;

  status = RtlStringCchPrintfW(
    Mapping->VirtualDeviceNameBuffer,
    RTL_NUMBER_OF(Mapping->VirtualDeviceNameBuffer),
    L"\\Device\\lvmdmVolume%I64u",
    Mapping->MappingId
  );
  if (!NT_SUCCESS(status)) {
    return status;
  }
  status = RtlStringCchPrintfW(
    Mapping->VirtualSymbolicLinkBuffer,
    RTL_NUMBER_OF(Mapping->VirtualSymbolicLinkBuffer),
    L"\\DosDevices\\Global\\lvmdmVolume%I64u",
    Mapping->MappingId
  );
  if (!NT_SUCCESS(status)) {
    return status;
  }

  RtlInitUnicodeString(&Mapping->VirtualDeviceName, Mapping->VirtualDeviceNameBuffer);
  RtlInitUnicodeString(&Mapping->VirtualSymbolicLink, Mapping->VirtualSymbolicLinkBuffer);

  status = IoCreateDevice(
    DeviceExtension->DeviceObject->DriverObject,
    sizeof(LVMDM_DEVICE_EXTENSION),
    &Mapping->VirtualDeviceName,
    FILE_DEVICE_DISK,
    FILE_DEVICE_SECURE_OPEN,
    FALSE,
    &deviceObject
  );
  if (!NT_SUCCESS(status)) {
    return status;
  }

  volumeExtension = (PLVMDM_DEVICE_EXTENSION)deviceObject->DeviceExtension;
  RtlZeroMemory(volumeExtension, sizeof(*volumeExtension));
  volumeExtension->DeviceKind = LVMDM_DEVICE_KIND_VOLUME;
  volumeExtension->DeviceObject = deviceObject;
  volumeExtension->DeviceName = Mapping->VirtualDeviceName;
  volumeExtension->SymbolicLink = Mapping->VirtualSymbolicLink;
  volumeExtension->Mapping = Mapping;

  deviceObject->Flags |= DO_DIRECT_IO;
  deviceObject->SectorSize = (USHORT)header->exposed_logical_sector_size;
  deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

  status = IoCreateSymbolicLink(&Mapping->VirtualSymbolicLink, &Mapping->VirtualDeviceName);
  if (!NT_SUCCESS(status)) {
    IoDeleteDevice(deviceObject);
    return status;
  }

  Mapping->VirtualDiskDevice = deviceObject;
  KdPrint(("LVMDM: Created virtual disk %wZ\n", &Mapping->VirtualSymbolicLink));
  return STATUS_SUCCESS;
}

VOID LvmdmDeleteVirtualDisk(
  PLVMDM_MAPPING_ENTRY Mapping
) {
  if (!Mapping || !Mapping->VirtualDiskDevice) {
    return;
  }

  IoDeleteSymbolicLink(&Mapping->VirtualSymbolicLink);
  IoDeleteDevice(Mapping->VirtualDiskDevice);
  Mapping->VirtualDiskDevice = NULL;
}

NTSTATUS LvmdmVirtualDeviceControl(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  PIRP Irp,
  PIO_STACK_LOCATION IrpStack
) {
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
  ULONG information = 0;
  ULONG ioControlCode = IrpStack->Parameters.DeviceIoControl.IoControlCode;
  PVOID outputBuffer = Irp->AssociatedIrp.SystemBuffer;
  ULONG outputBufferLength = IrpStack->Parameters.DeviceIoControl.OutputBufferLength;
  PLVMDM_MAPPING_ENTRY mapping = DeviceExtension->Mapping;
  const LVMDM_MAPPING_TABLE_HEADER* header;

  if (!mapping || !mapping->MappingTableBuffer) {
    status = STATUS_DEVICE_NOT_READY;
    goto Complete;
  }

  header = (const LVMDM_MAPPING_TABLE_HEADER*)mapping->MappingTableBuffer;

  switch (ioControlCode) {
    case IOCTL_DISK_GET_LENGTH_INFO: {
      PGET_LENGTH_INFORMATION lengthInfo;
      if (!outputBuffer || outputBufferLength < sizeof(GET_LENGTH_INFORMATION)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      lengthInfo = (PGET_LENGTH_INFORMATION)outputBuffer;
      RtlZeroMemory(lengthInfo, sizeof(*lengthInfo));
      lengthInfo->Length.QuadPart = (LONGLONG)header->lv_size_bytes;
      information = sizeof(*lengthInfo);
      status = STATUS_SUCCESS;
      break;
    }

    case IOCTL_DISK_GET_DRIVE_GEOMETRY: {
      PDISK_GEOMETRY geometry;
      ULONGLONG bytesPerCylinder;
      ULONGLONG cylinders;
      if (!outputBuffer || outputBufferLength < sizeof(DISK_GEOMETRY)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      geometry = (PDISK_GEOMETRY)outputBuffer;
      RtlZeroMemory(geometry, sizeof(*geometry));
      geometry->MediaType = FixedMedia;
      geometry->TracksPerCylinder = 255;
      geometry->SectorsPerTrack = 63;
      geometry->BytesPerSector = header->exposed_logical_sector_size;
      bytesPerCylinder = (ULONGLONG)geometry->TracksPerCylinder *
                         geometry->SectorsPerTrack *
                         geometry->BytesPerSector;
      cylinders = bytesPerCylinder ? header->lv_size_bytes / bytesPerCylinder : 0;
      if (cylinders == 0) {
        cylinders = 1;
      }
      geometry->Cylinders.QuadPart = (LONGLONG)cylinders;
      information = sizeof(*geometry);
      status = STATUS_SUCCESS;
      break;
    }

    case IOCTL_DISK_GET_DRIVE_GEOMETRY_EX: {
      PDISK_GEOMETRY_EX geometryEx;
      ULONGLONG bytesPerCylinder;
      ULONGLONG cylinders;
      if (!outputBuffer || outputBufferLength < sizeof(DISK_GEOMETRY_EX)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      geometryEx = (PDISK_GEOMETRY_EX)outputBuffer;
      RtlZeroMemory(geometryEx, sizeof(*geometryEx));
      geometryEx->Geometry.MediaType = FixedMedia;
      geometryEx->Geometry.TracksPerCylinder = 255;
      geometryEx->Geometry.SectorsPerTrack = 63;
      geometryEx->Geometry.BytesPerSector = header->exposed_logical_sector_size;
      bytesPerCylinder = (ULONGLONG)geometryEx->Geometry.TracksPerCylinder *
                         geometryEx->Geometry.SectorsPerTrack *
                         geometryEx->Geometry.BytesPerSector;
      cylinders = bytesPerCylinder ? header->lv_size_bytes / bytesPerCylinder : 0;
      if (cylinders == 0) {
        cylinders = 1;
      }
      geometryEx->Geometry.Cylinders.QuadPart = (LONGLONG)cylinders;
      geometryEx->DiskSize.QuadPart = (LONGLONG)header->lv_size_bytes;
      information = sizeof(*geometryEx);
      status = STATUS_SUCCESS;
      break;
    }

    case IOCTL_DISK_GET_PARTITION_INFO_EX: {
      PPARTITION_INFORMATION_EX partition;
      if (!outputBuffer || outputBufferLength < sizeof(PARTITION_INFORMATION_EX)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      partition = (PPARTITION_INFORMATION_EX)outputBuffer;
      RtlZeroMemory(partition, sizeof(*partition));
      partition->PartitionStyle = PARTITION_STYLE_RAW;
      partition->StartingOffset.QuadPart = 0;
      partition->PartitionLength.QuadPart = (LONGLONG)header->lv_size_bytes;
      partition->PartitionNumber = 0;
      partition->RewritePartition = FALSE;
      information = sizeof(*partition);
      status = STATUS_SUCCESS;
      break;
    }

    case IOCTL_DISK_GET_DRIVE_LAYOUT_EX: {
      PDRIVE_LAYOUT_INFORMATION_EX layout;
      ULONG required = FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry);
      if (!outputBuffer || outputBufferLength < required) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      layout = (PDRIVE_LAYOUT_INFORMATION_EX)outputBuffer;
      RtlZeroMemory(layout, required);
      layout->PartitionStyle = PARTITION_STYLE_RAW;
      layout->PartitionCount = 0;
      information = required;
      status = STATUS_SUCCESS;
      break;
    }

    case IOCTL_DISK_GET_MEDIA_TYPES: {
      PDISK_GEOMETRY geometry;
      ULONGLONG bytesPerCylinder;
      ULONGLONG cylinders;
      if (!outputBuffer || outputBufferLength < sizeof(DISK_GEOMETRY)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      geometry = (PDISK_GEOMETRY)outputBuffer;
      RtlZeroMemory(geometry, sizeof(*geometry));
      geometry->MediaType = FixedMedia;
      geometry->TracksPerCylinder = 255;
      geometry->SectorsPerTrack = 63;
      geometry->BytesPerSector = header->exposed_logical_sector_size;
      bytesPerCylinder = (ULONGLONG)geometry->TracksPerCylinder *
                         geometry->SectorsPerTrack *
                         geometry->BytesPerSector;
      cylinders = bytesPerCylinder ? header->lv_size_bytes / bytesPerCylinder : 0;
      if (cylinders == 0) {
        cylinders = 1;
      }
      geometry->Cylinders.QuadPart = (LONGLONG)cylinders;
      information = sizeof(*geometry);
      status = STATUS_SUCCESS;
      break;
    }

    case IOCTL_STORAGE_GET_DEVICE_NUMBER: {
      PSTORAGE_DEVICE_NUMBER deviceNumber;
      if (!outputBuffer || outputBufferLength < sizeof(STORAGE_DEVICE_NUMBER)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      deviceNumber = (PSTORAGE_DEVICE_NUMBER)outputBuffer;
      RtlZeroMemory(deviceNumber, sizeof(*deviceNumber));
      deviceNumber->DeviceType = FILE_DEVICE_DISK;
      deviceNumber->DeviceNumber = (ULONG)mapping->MappingId;
      deviceNumber->PartitionNumber = (ULONG)-1;
      information = sizeof(*deviceNumber);
      status = STATUS_SUCCESS;
      break;
    }

    case IOCTL_STORAGE_QUERY_PROPERTY: {
      PSTORAGE_PROPERTY_QUERY query = (PSTORAGE_PROPERTY_QUERY)Irp->AssociatedIrp.SystemBuffer;
      if (!query || IrpStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(STORAGE_PROPERTY_QUERY)) {
        status = STATUS_INVALID_PARAMETER;
        break;
      }
      if (query->PropertyId == StorageDeviceProperty &&
          query->QueryType == PropertyStandardQuery) {
        PSTORAGE_DEVICE_DESCRIPTOR descriptor;
        const CHAR vendor[] = "WINLVM";
        const CHAR product[] = "LogicalVolume";
        const CHAR revision[] = "0001";
        ULONG vendorOffset = sizeof(STORAGE_DEVICE_DESCRIPTOR);
        ULONG productOffset = vendorOffset + sizeof(vendor);
        ULONG revisionOffset = productOffset + sizeof(product);
        ULONG required = revisionOffset + sizeof(revision);
        if (!outputBuffer || outputBufferLength < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
          status = STATUS_BUFFER_TOO_SMALL;
          break;
        }
        descriptor = (PSTORAGE_DEVICE_DESCRIPTOR)outputBuffer;
        RtlZeroMemory(outputBuffer, outputBufferLength);
        descriptor->Version = sizeof(STORAGE_DEVICE_DESCRIPTOR);
        descriptor->Size = required;
        descriptor->DeviceType = 0;
        descriptor->DeviceTypeModifier = 0;
        descriptor->RemovableMedia = FALSE;
        descriptor->CommandQueueing = FALSE;
        descriptor->VendorIdOffset = vendorOffset;
        descriptor->ProductIdOffset = productOffset;
        descriptor->ProductRevisionOffset = revisionOffset;
        descriptor->SerialNumberOffset = 0;
        descriptor->BusType = BusTypeVirtual;
        if (outputBufferLength < required) {
          information = sizeof(STORAGE_DEVICE_DESCRIPTOR);
          status = STATUS_SUCCESS;
          break;
        }
        RtlCopyMemory((PUCHAR)outputBuffer + vendorOffset, vendor, sizeof(vendor));
        RtlCopyMemory((PUCHAR)outputBuffer + productOffset, product, sizeof(product));
        RtlCopyMemory((PUCHAR)outputBuffer + revisionOffset, revision, sizeof(revision));
        information = required;
        status = STATUS_SUCCESS;
        break;
      }
      if (query->PropertyId == StorageAdapterProperty &&
          query->QueryType == PropertyStandardQuery) {
        PSTORAGE_ADAPTER_DESCRIPTOR descriptor;
        if (!outputBuffer || outputBufferLength < sizeof(STORAGE_ADAPTER_DESCRIPTOR)) {
          status = STATUS_BUFFER_TOO_SMALL;
          break;
        }
        descriptor = (PSTORAGE_ADAPTER_DESCRIPTOR)outputBuffer;
        RtlZeroMemory(descriptor, sizeof(*descriptor));
        descriptor->Version = sizeof(STORAGE_ADAPTER_DESCRIPTOR);
        descriptor->Size = sizeof(STORAGE_ADAPTER_DESCRIPTOR);
        descriptor->MaximumTransferLength = MAXULONG;
        descriptor->MaximumPhysicalPages = MAXULONG;
        descriptor->AlignmentMask = header->exposed_logical_sector_size - 1;
        descriptor->AdapterUsesPio = TRUE;
        descriptor->AdapterScansDown = FALSE;
        descriptor->CommandQueueing = FALSE;
        descriptor->AcceleratedTransfer = FALSE;
        descriptor->BusType = BusTypeVirtual;
        information = sizeof(*descriptor);
        status = STATUS_SUCCESS;
        break;
      }
      status = STATUS_INVALID_DEVICE_REQUEST;
      break;
    }

    case IOCTL_DISK_VERIFY:
    case IOCTL_DISK_CHECK_VERIFY:
    case IOCTL_STORAGE_CHECK_VERIFY:
      status = STATUS_SUCCESS;
      break;

    case IOCTL_DISK_IS_WRITABLE:
      status = STATUS_MEDIA_WRITE_PROTECTED;
      break;

    default:
      status = STATUS_INVALID_DEVICE_REQUEST;
      break;
  }

Complete:
  Irp->IoStatus.Status = status;
  Irp->IoStatus.Information = information;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return status;
}

NTSTATUS LvmdmReadVirtualDisk(
  PLVMDM_DEVICE_EXTENSION DeviceExtension,
  PIRP Irp,
  PIO_STACK_LOCATION IrpStack
) {
  NTSTATUS status = STATUS_SUCCESS;
  PLVMDM_MAPPING_ENTRY mapping = DeviceExtension->Mapping;
  PVOID systemBuffer = NULL;
  LARGE_INTEGER byteOffset = IrpStack->Parameters.Read.ByteOffset;
  ULONG length = IrpStack->Parameters.Read.Length;
  ULONG bytesDone = 0;

  if (!mapping || !mapping->MappingTableBuffer) {
    status = STATUS_DEVICE_NOT_READY;
    goto Complete;
  }
  if (byteOffset.QuadPart < 0) {
    status = STATUS_INVALID_PARAMETER;
    goto Complete;
  }
  if (length == 0) {
    status = STATUS_SUCCESS;
    goto Complete;
  }
  if (Irp->MdlAddress) {
    systemBuffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
  } else if (Irp->RequestorMode == KernelMode) {
    systemBuffer = Irp->UserBuffer;
  }
  if (!systemBuffer) {
    status = STATUS_INVALID_USER_BUFFER;
    goto Complete;
  }

  while (bytesDone < length) {
    LVMDM_TRANSLATED_RANGE translation;
    ULONGLONG logicalOffset = (ULONGLONG)byteOffset.QuadPart + bytesDone;
    ULONG toRead;

    RtlZeroMemory(&translation, sizeof(translation));
    status = LvmdmTranslateLogicalRange(
      mapping->MappingTableBuffer,
      logicalOffset,
      (ULONGLONG)(length - bytesDone),
      &translation
    );
    if (!NT_SUCCESS(status)) {
      break;
    }
    if (translation.LengthBytes == 0 || translation.LengthBytes > MAXULONG) {
      status = STATUS_INVALID_PARAMETER;
      break;
    }

    toRead = (ULONG)translation.LengthBytes;
    status = LvmdmReadBackingSource(
      translation.PvHandle->source_path,
      translation.SourceOffsetBytes,
      (PUCHAR)systemBuffer + bytesDone,
      toRead
    );
    if (!NT_SUCCESS(status)) {
      break;
    }
    bytesDone += toRead;
  }

  if (NT_SUCCESS(status)) {
    mapping->IoCount++;
  }

Complete:
  Irp->IoStatus.Status = status;
  Irp->IoStatus.Information = NT_SUCCESS(status) ? bytesDone : 0;
  IoCompleteRequest(Irp, IO_DISK_INCREMENT);
  return status;
}

NTSTATUS LvmdmReadBackingSource(
  const CHAR* SourcePath,
  ULONGLONG SourceOffsetBytes,
  PVOID Buffer,
  ULONG Length
) {
  WCHAR pathBuffer[LVMDM_MAX_SOURCE_PATH_LENGTH + 8];
  UNICODE_STRING path;
  OBJECT_ATTRIBUTES attributes;
  IO_STATUS_BLOCK ioStatus;
  LARGE_INTEGER offset;
  HANDLE fileHandle = NULL;
  NTSTATUS status;

  if (!SourcePath || !Buffer || Length == 0) {
    return STATUS_INVALID_PARAMETER;
  }

  if (SourcePath[0] == '\\' && SourcePath[1] == '\\' &&
      SourcePath[2] == '.' && SourcePath[3] == '\\') {
    status = RtlStringCchPrintfW(
      pathBuffer,
      RTL_NUMBER_OF(pathBuffer),
      L"\\??\\%S",
      SourcePath + 4
    );
  } else {
    status = RtlStringCchPrintfW(
      pathBuffer,
      RTL_NUMBER_OF(pathBuffer),
      L"%S",
      SourcePath
    );
  }
  if (!NT_SUCCESS(status)) {
    return status;
  }

  RtlInitUnicodeString(&path, pathBuffer);
  InitializeObjectAttributes(
    &attributes,
    &path,
    OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
    NULL,
    NULL
  );

  status = ZwCreateFile(
    &fileHandle,
    FILE_READ_DATA | SYNCHRONIZE,
    &attributes,
    &ioStatus,
    NULL,
    FILE_ATTRIBUTE_NORMAL,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    FILE_OPEN,
    FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
    NULL,
    0
  );
  if (!NT_SUCCESS(status)) {
    return status;
  }

  offset.QuadPart = (LONGLONG)SourceOffsetBytes;
  status = ZwReadFile(
    fileHandle,
    NULL,
    NULL,
    NULL,
    &ioStatus,
    Buffer,
    Length,
    &offset,
    NULL
  );
  if (NT_SUCCESS(status) && ioStatus.Information != Length) {
    status = STATUS_END_OF_FILE;
  }

  ZwClose(fileHandle);
  return status;
}
