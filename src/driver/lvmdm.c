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
#include <wdf.h>
#include "lvmdm_ioctl.h"

// ============================================================================
// Driver Globals
// ============================================================================

typedef struct _LVMDM_DEVICE_EXTENSION {
  PDEVICE_OBJECT DeviceObject;
  UNICODE_STRING DeviceName;
  UNICODE_STRING SymbolicLink;
  KSPIN_LOCK MappingLock;
  LIST_ENTRY MappingList;
  ULONGLONG NextMappingId;
} LVMDM_DEVICE_EXTENSION, *PLVMDM_DEVICE_EXTENSION;

typedef struct _LVMDM_MAPPING_ENTRY {
  LIST_ENTRY ListEntry;
  ULONGLONG MappingId;
  PVOID MappingTableBuffer;
  SIZE_T MappingTableSize;
  PDEVICE_OBJECT VirtualDiskDevice;
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
  DriverObject->MajorFunction[IRP_MJ_READ] = LvmdmUnsupportedReadWrite;
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
  UNREFERENCED_PARAMETER(DeviceObject);

  Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return STATUS_NOT_SUPPORTED;
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
  entry->MappingId = DeviceExtension->NextMappingId++;
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
