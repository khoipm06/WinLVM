#include "testing.h"
#include <cstdint>
#include <cstring>
#include <vector>

// Mock WDK types for Linux build
typedef int32_t NTSTATUS;
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010L)

#define IOCTL_DISK_GET_DRIVE_LAYOUT_EX 0x00070050

typedef union _LARGE_INTEGER {
  struct {
    uint32_t LowPart;
    int32_t HighPart;
  } DUMMYSTRUCTNAME;
  struct {
    uint32_t LowPart;
    int32_t HighPart;
  } u;
  int64_t QuadPart;
} LARGE_INTEGER;

typedef enum _PARTITION_STYLE {
  PARTITION_STYLE_MBR = 0,
  PARTITION_STYLE_GPT = 1,
  PARTITION_STYLE_RAW = 2
} PARTITION_STYLE;

typedef struct _PARTITION_INFORMATION_MBR {
  uint8_t PartitionType;
  uint8_t BootIndicator;
  uint8_t RecognizedPartition;
  uint32_t HiddenSectors;
  // struct _GUID not defined, but GUID is 16 bytes
  uint8_t PartitionId[16];
} PARTITION_INFORMATION_MBR;

typedef struct _PARTITION_INFORMATION_GPT {
  uint8_t PartitionType[16]; // GUID
  uint8_t PartitionId[16];   // GUID
  uint64_t Attributes;
  uint16_t Name[36];
} PARTITION_INFORMATION_GPT;

typedef struct _PARTITION_INFORMATION_EX {
  PARTITION_STYLE PartitionStyle;
  LARGE_INTEGER StartingOffset;
  LARGE_INTEGER PartitionLength;
  uint32_t PartitionNumber;
  uint8_t RewritePartition;
  uint8_t IsServicePartition;
  union {
    PARTITION_INFORMATION_MBR Mbr;
    PARTITION_INFORMATION_GPT Gpt;
  } DUMMYUNIONNAME;
} PARTITION_INFORMATION_EX;

typedef struct _DRIVE_LAYOUT_INFORMATION_EX {
  uint32_t PartitionStyle;
  uint32_t PartitionCount;
  union {
    struct {
      uint32_t Signature;
      uint32_t CheckSum;
    } Mbr;
    struct {
      uint8_t DiskId[16]; // GUID
      LARGE_INTEGER StartingUsableOffset;
      LARGE_INTEGER UsableLength;
      uint32_t MaxPartitionCount;
    } Gpt;
  } DUMMYUNIONNAME;
  PARTITION_INFORMATION_EX PartitionEntry[1];
} DRIVE_LAYOUT_INFORMATION_EX;

// Mock IRP / IO_STACK_LOCATION
struct IO_STACK_LOCATION {
  struct {
    uint32_t OutputBufferLength;
    uint32_t InputBufferLength;
    uint32_t IoControlCode;
    void* Type3InputBuffer;
  } DeviceIoControl;
};

struct IRP {
  void* AssociatedIrp_SystemBuffer;
  struct {
    NTSTATUS Status;
    uint64_t Information;
  } IoStatus;

  IO_STACK_LOCATION* current_stack; // mock helper
};

// Target function prototype and implementation for TDD GREEN phase
NTSTATUS WinLvmMountIoctl(void* device_extension, IRP* irp, IO_STACK_LOCATION* irpsp) {
  uint64_t byte_size = *reinterpret_cast<uint64_t*>(device_extension);
  uint32_t code = irpsp->DeviceIoControl.IoControlCode;

  if (code == IOCTL_DISK_GET_DRIVE_LAYOUT_EX) {
    if (irpsp->DeviceIoControl.OutputBufferLength < sizeof(DRIVE_LAYOUT_INFORMATION_EX)) {
      irp->IoStatus.Information = sizeof(DRIVE_LAYOUT_INFORMATION_EX);
      return STATUS_BUFFER_TOO_SMALL;
    }
    auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(irp->AssociatedIrp_SystemBuffer);
    std::memset(layout, 0, sizeof(DRIVE_LAYOUT_INFORMATION_EX));
    layout->PartitionStyle = PARTITION_STYLE_MBR;
    layout->PartitionCount = 1;
    layout->DUMMYUNIONNAME.Mbr.Signature = 0x574C564D; // 'WLVM'

    PARTITION_INFORMATION_EX* entry = &layout->PartitionEntry[0];
    entry->PartitionStyle = PARTITION_STYLE_MBR;
    entry->StartingOffset.QuadPart = 0;
    entry->PartitionLength.QuadPart = (int64_t)byte_size;
    entry->PartitionNumber = 1;
    entry->RewritePartition = 0;
    entry->DUMMYUNIONNAME.Mbr.PartitionType = 0x07; // PARTITION_IFS
    entry->DUMMYUNIONNAME.Mbr.BootIndicator = 0;
    entry->DUMMYUNIONNAME.Mbr.RecognizedPartition = 1;
    entry->DUMMYUNIONNAME.Mbr.HiddenSectors = 0;

    irp->IoStatus.Information = sizeof(DRIVE_LAYOUT_INFORMATION_EX);
    return STATUS_SUCCESS;
  }

  irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
  irp->IoStatus.Information = 0;
  return STATUS_INVALID_DEVICE_REQUEST;
}

TEST(MountIoctl_GetDriveLayoutEx_BufferTooSmall) {
  uint64_t volume_size = 10ULL * 1024 * 1024 * 1024; // 10GB
  void* ext = &volume_size; // mock extension just holds size

  IRP irp{};
  IO_STACK_LOCATION irpsp{};
  irpsp.DeviceIoControl.IoControlCode = IOCTL_DISK_GET_DRIVE_LAYOUT_EX;
  irpsp.DeviceIoControl.OutputBufferLength = sizeof(DRIVE_LAYOUT_INFORMATION_EX) - sizeof(PARTITION_INFORMATION_EX);

  std::vector<uint8_t> buffer(irpsp.DeviceIoControl.OutputBufferLength);
  irp.AssociatedIrp_SystemBuffer = buffer.data();
  irp.current_stack = &irpsp;

  NTSTATUS status = WinLvmMountIoctl(ext, &irp, &irpsp);

  CHECK(status == STATUS_BUFFER_TOO_SMALL || status == STATUS_BUFFER_OVERFLOW);
  CHECK_EQ(irp.IoStatus.Information, sizeof(DRIVE_LAYOUT_INFORMATION_EX));
}

TEST(MountIoctl_GetDriveLayoutEx_Success) {
  uint64_t volume_size = 10ULL * 1024 * 1024 * 1024; // 10GB
  void* ext = &volume_size;

  IRP irp{};
  IO_STACK_LOCATION irpsp{};
  irpsp.DeviceIoControl.IoControlCode = IOCTL_DISK_GET_DRIVE_LAYOUT_EX;
  irpsp.DeviceIoControl.OutputBufferLength = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + sizeof(PARTITION_INFORMATION_EX);

  std::vector<uint8_t> buffer(irpsp.DeviceIoControl.OutputBufferLength, 0);
  irp.AssociatedIrp_SystemBuffer = buffer.data();
  irp.current_stack = &irpsp;

  NTSTATUS status = WinLvmMountIoctl(ext, &irp, &irpsp);

  CHECK_EQ(status, STATUS_SUCCESS);
  CHECK_EQ(irp.IoStatus.Information, sizeof(DRIVE_LAYOUT_INFORMATION_EX));

  auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buffer.data());
  CHECK(layout->PartitionCount >= 1);
  CHECK_EQ(layout->PartitionEntry[0].PartitionLength.QuadPart, (int64_t)volume_size);

  // Must be marked as recognized so Windows Disk Management doesn't prompt "Initialize Disk"
  bool recognized = false;
  if (layout->PartitionStyle == PARTITION_STYLE_MBR) {
    recognized = (layout->PartitionEntry[0].DUMMYUNIONNAME.Mbr.RecognizedPartition != 0);
  } else if (layout->PartitionStyle == PARTITION_STYLE_GPT) {
    // Basic data partition GUID check (not all zeroes)
    const uint8_t* guid = layout->PartitionEntry[0].DUMMYUNIONNAME.Gpt.PartitionType;
    for (int i = 0; i < 16; i++) {
        if (guid[i] != 0) {
            recognized = true;
            break;
        }
    }
  }
  CHECK_MSG(recognized, "Partition must be recognized (RecognizedPartition=TRUE or valid GPT GUID)");
}
