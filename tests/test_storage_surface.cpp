#include "testing.h"
#include "host_wdm_types.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct IoctlTestFixture {
    WINLVM_LV_EXT ext{};
    wl_vg_record record{};
    lvmcore_volume_group view{};
    lvmcore_logical_volume lv{};

    IoctlTestFixture() {
        std::memset(&ext, 0, sizeof(ext));
        std::memset(&record, 0, sizeof(record));
        std::memset(&view, 0, sizeof(view));
        std::memset(&lv, 0, sizeof(lv));

        ext.kind = WL_EXT_LV;
        ext.byte_size = 10ULL * 1021024 * 1024; // 10 GiB
        ext.logical_sector = 512;
        ext.physical_sector = 4096;
        ext.lv_index = 0;
        ext.backing_count = 1;
        ext.pv_slot[0] = 0;

        const WCHAR devNameStr[] = {
            '\\', 'D', 'e', 'v', 'i', 'c', 'e', '\\', 'W', 'i', 'n', 'L', 'v', 'm', 'V', 'o', 'l', '0', 0
        };
        std::memcpy(ext.devname, devNameStr, sizeof(devNameStr));

        std::memcpy(lv.name, "lv_test", 8);
        std::memcpy(lv.id, "1234567890abcdef1234567890abcdef", 32);
        lv.id[32] = '\0';
        view.lv_count = 1;
        view.lvs = &lv;
        record.view = &view;
        ext.record = &record;
    }

    NTSTATUS runStorageIoctl(ULONG code, size_t inLen, size_t outLen,
                            const void *inData, std::vector<uint8_t> &outBuf,
                            uint64_t &outInfo) {
        size_t sysBufSize = std::max(inLen, outLen);
        std::vector<uint8_t> sysBuf(sysBufSize > 0 ? sysBufSize : 1, 0);
        if (inData && inLen > 0) {
            std::memcpy(sysBuf.data(), inData, inLen);
        }

        DEVICE_OBJECT devObj{};
        IRP irp{};
        IO_STACK_LOCATION stack{};
        stack.Parameters.DeviceIoControl.IoControlCode = code;
        stack.Parameters.DeviceIoControl.InputBufferLength = (ULONG)inLen;
        stack.Parameters.DeviceIoControl.OutputBufferLength = (ULONG)outLen;
        irp.CurrentStackLocation = &stack;
        irp.AssociatedIrp.SystemBuffer = sysBuf.data();

        NTSTATUS status = WinLvmStorageIoctl(&devObj, &irp, &ext);
        outInfo = irp.IoStatus.Information;
        if (outLen > 0) {
            outBuf.assign(sysBuf.begin(), sysBuf.begin() + static_cast<ptrdiff_t>(outLen));
        } else {
            outBuf.clear();
        }
        return status;
    }

    NTSTATUS runMountIoctl(ULONG code, size_t inLen, size_t outLen,
                          const void *inData, std::vector<uint8_t> &outBuf,
                          uint64_t &outInfo) {
        size_t sysBufSize = std::max(inLen, outLen);
        std::vector<uint8_t> sysBuf(sysBufSize > 0 ? sysBufSize : 1, 0);
        if (inData && inLen > 0) {
            std::memcpy(sysBuf.data(), inData, inLen);
        }

        DEVICE_OBJECT devObj{};
        IRP irp{};
        IO_STACK_LOCATION stack{};
        stack.Parameters.DeviceIoControl.IoControlCode = code;
        stack.Parameters.DeviceIoControl.InputBufferLength = (ULONG)inLen;
        stack.Parameters.DeviceIoControl.OutputBufferLength = (ULONG)outLen;
        irp.CurrentStackLocation = &stack;
        irp.AssociatedIrp.SystemBuffer = sysBuf.data();

        NTSTATUS status = WinLvmMountIoctl(&devObj, &irp, &ext);
        outInfo = irp.IoStatus.Information;
        if (outLen > 0) {
            outBuf.assign(sysBuf.begin(), sysBuf.begin() + static_cast<ptrdiff_t>(outLen));
        } else {
            outBuf.clear();
        }
        return status;
    }
};

} // namespace

/* -------------------------------------------------------------------------
 * Group 1: Storage Sizing & Geometry Contracts (Exact STATUS_BUFFER_TOO_SMALL)
 * ------------------------------------------------------------------------- */

TEST(StorageSurface_GetLengthInfo_ExactStatusContract) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;

    // Undersized buffer MUST return STATUS_BUFFER_TOO_SMALL (0xC0000023).
    // MUST NEVER return STATUS_BUFFER_OVERFLOW (0x80000005).
    NTSTATUS status = fix.runStorageIoctl(IOCTL_DISK_GET_LENGTH_INFO, 0,
                                         sizeof(GET_LENGTH_INFORMATION) - 1, nullptr, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_TOO_SMALL);
    CHECK(status != STATUS_BUFFER_OVERFLOW);
    CHECK_EQ(info, sizeof(GET_LENGTH_INFORMATION));

    // Sufficient buffer
    status = fix.runStorageIoctl(IOCTL_DISK_GET_LENGTH_INFO, 0,
                                sizeof(GET_LENGTH_INFORMATION), nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    CHECK_EQ(info, sizeof(GET_LENGTH_INFORMATION));
    auto *lenInfo = reinterpret_cast<const GET_LENGTH_INFORMATION*>(buf.data());
    CHECK_EQ((uint64_t)lenInfo->Length.QuadPart, fix.ext.byte_size);
}

TEST(StorageSurface_GetDriveGeometry_ExactStatusContract) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;

    // Undersized buffer MUST return STATUS_BUFFER_TOO_SMALL
    NTSTATUS status = fix.runStorageIoctl(IOCTL_DISK_GET_DRIVE_GEOMETRY, 0,
                                         sizeof(DISK_GEOMETRY) - 1, nullptr, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_TOO_SMALL);
    CHECK(status != STATUS_BUFFER_OVERFLOW);
    CHECK_EQ(info, sizeof(DISK_GEOMETRY));

    // Sufficient buffer
    status = fix.runStorageIoctl(IOCTL_DISK_GET_DRIVE_GEOMETRY, 0,
                                sizeof(DISK_GEOMETRY), nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    CHECK_EQ(info, sizeof(DISK_GEOMETRY));
    auto *geom = reinterpret_cast<const DISK_GEOMETRY*>(buf.data());
    CHECK_EQ((int)geom->MediaType, (int)FixedMedia);
    CHECK_EQ(geom->TracksPerCylinder, 255u);
    CHECK_EQ(geom->SectorsPerTrack, 63u);
    CHECK_EQ(geom->BytesPerSector, 512u);
}

TEST(StorageSurface_GetDriveGeometryEx_ExactStatusContract) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;

    // Undersized buffer MUST return STATUS_BUFFER_TOO_SMALL
    NTSTATUS status = fix.runStorageIoctl(IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, 0,
                                         sizeof(DISK_GEOMETRY_EX) - 1, nullptr, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_TOO_SMALL);
    CHECK(status != STATUS_BUFFER_OVERFLOW);
    CHECK_EQ(info, sizeof(DISK_GEOMETRY_EX));

    // Sufficient buffer
    status = fix.runStorageIoctl(IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, 0,
                                sizeof(DISK_GEOMETRY_EX), nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    CHECK_EQ(info, sizeof(DISK_GEOMETRY_EX));
    auto *geomEx = reinterpret_cast<const DISK_GEOMETRY_EX*>(buf.data());
    CHECK_EQ((uint64_t)geomEx->DiskSize.QuadPart, fix.ext.byte_size);
    CHECK_EQ(geomEx->Geometry.BytesPerSector, 512u);
    CHECK_EQ((int)geomEx->Geometry.MediaType, (int)FixedMedia);
}

/* -------------------------------------------------------------------------
 * Group 2: Synthetic Protective MBR Layout Spoofing & Popup Suppression
 * ------------------------------------------------------------------------- */

TEST(StorageSurface_DriveLayoutEx_Spoofing) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;

    // Undersized buffer MUST return STATUS_BUFFER_TOO_SMALL
    NTSTATUS status = fix.runStorageIoctl(IOCTL_DISK_GET_DRIVE_LAYOUT_EX, 0,
                                         sizeof(DRIVE_LAYOUT_INFORMATION_EX) - 1, nullptr, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_TOO_SMALL);
    CHECK(status != STATUS_BUFFER_OVERFLOW);
    CHECK_EQ(info, sizeof(DRIVE_LAYOUT_INFORMATION_EX));

    // Sufficient buffer
    status = fix.runStorageIoctl(IOCTL_DISK_GET_DRIVE_LAYOUT_EX, 0,
                                sizeof(DRIVE_LAYOUT_INFORMATION_EX), nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    CHECK_EQ(info, sizeof(DRIVE_LAYOUT_INFORMATION_EX));
    auto *layout = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());
    CHECK_EQ(layout->PartitionStyle, (ULONG)PARTITION_STYLE_MBR);
    CHECK_EQ(layout->PartitionCount, 1u);
    CHECK_EQ(layout->Mbr.Signature, 0x574C564Du); // 'WLVM'

    const auto &entry = layout->PartitionEntry[0];
    CHECK_EQ(entry.PartitionStyle, (ULONG)PARTITION_STYLE_MBR);
    CHECK_EQ(entry.PartitionNumber, 1u);
    CHECK_EQ((uint64_t)entry.PartitionLength.QuadPart, fix.ext.byte_size);
    CHECK_EQ(entry.Mbr.PartitionType, 0x07); // PARTITION_IFS
    CHECK_EQ((int)entry.Mbr.RecognizedPartition, (int)TRUE);
}

TEST(StorageSurface_DriveLayoutLegacy_Spoofing) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;

    NTSTATUS status = fix.runStorageIoctl(IOCTL_DISK_GET_DRIVE_LAYOUT, 0,
                                         sizeof(DRIVE_LAYOUT_INFORMATION) - 1, nullptr, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_TOO_SMALL);

    status = fix.runStorageIoctl(IOCTL_DISK_GET_DRIVE_LAYOUT, 0,
                                sizeof(DRIVE_LAYOUT_INFORMATION), nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    CHECK_EQ(info, sizeof(DRIVE_LAYOUT_INFORMATION));
    auto *layout = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION*>(buf.data());
    CHECK_EQ(layout->PartitionCount, 1u);
    CHECK_EQ(layout->Signature, 0x574C564Du);
    CHECK_EQ(layout->PartitionEntry[0].PartitionType, 0x07);
    CHECK_EQ((int)layout->PartitionEntry[0].RecognizedPartition, (int)TRUE);
}

TEST(StorageSurface_PartitionInfo_Spoofing) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;

    // IOCTL_DISK_GET_PARTITION_INFO
    NTSTATUS status = fix.runStorageIoctl(IOCTL_DISK_GET_PARTITION_INFO, 0,
                                         sizeof(PARTITION_INFORMATION) - 1, nullptr, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_TOO_SMALL);

    status = fix.runStorageIoctl(IOCTL_DISK_GET_PARTITION_INFO, 0,
                                sizeof(PARTITION_INFORMATION), nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    auto *part = reinterpret_cast<const PARTITION_INFORMATION*>(buf.data());
    CHECK_EQ(part->PartitionType, 0x07);
    CHECK_EQ((int)part->RecognizedPartition, (int)TRUE);
    CHECK_EQ((uint64_t)part->PartitionLength.QuadPart, fix.ext.byte_size);

    // IOCTL_DISK_GET_PARTITION_INFO_EX
    status = fix.runStorageIoctl(IOCTL_DISK_GET_PARTITION_INFO_EX, 0,
                                sizeof(PARTITION_INFORMATION_EX) - 1, nullptr, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_TOO_SMALL);

    status = fix.runStorageIoctl(IOCTL_DISK_GET_PARTITION_INFO_EX, 0,
                                sizeof(PARTITION_INFORMATION_EX), nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    auto *partEx = reinterpret_cast<const PARTITION_INFORMATION_EX*>(buf.data());
    CHECK_EQ(partEx->PartitionStyle, (ULONG)PARTITION_STYLE_MBR);
    CHECK_EQ(partEx->Mbr.PartitionType, 0x07);
    CHECK_EQ((int)partEx->Mbr.RecognizedPartition, (int)TRUE);
}

/* -------------------------------------------------------------------------
 * Group 3: Storage Properties (3-Stage Discovery Protocol)
 * ------------------------------------------------------------------------- */

TEST(StorageSurface_StorageDeviceProperty_3Stage) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    // Stage 1: Under-header query
    NTSTATUS status = fix.runStorageIoctl(IOCTL_STORAGE_QUERY_PROPERTY,
                                         sizeof(query), sizeof(STORAGE_DESCRIPTOR_HEADER) - 1,
                                         &query, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_TOO_SMALL);
    CHECK_EQ(info, sizeof(STORAGE_DESCRIPTOR_HEADER));

    // Stage 2: Header-only discovery query -> STATUS_BUFFER_OVERFLOW with hdr->Size populated
    status = fix.runStorageIoctl(IOCTL_STORAGE_QUERY_PROPERTY,
                                sizeof(query), sizeof(STORAGE_DESCRIPTOR_HEADER),
                                &query, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_OVERFLOW);
    CHECK_EQ(info, sizeof(STORAGE_DESCRIPTOR_HEADER));
    auto *hdr = reinterpret_cast<const STORAGE_DESCRIPTOR_HEADER*>(buf.data());
    CHECK(hdr->Size > sizeof(STORAGE_DEVICE_DESCRIPTOR));
    ULONG totalReq = hdr->Size;

    // Stage 3: Full descriptor query
    status = fix.runStorageIoctl(IOCTL_STORAGE_QUERY_PROPERTY,
                                sizeof(query), totalReq,
                                &query, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    CHECK_EQ(info, (uint64_t)totalReq);
    auto *desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
    CHECK_EQ((int)desc->BusType, (int)BusTypeVirtual);
    CHECK_EQ((int)desc->RemovableMedia, (int)FALSE);

    const char *vendor = reinterpret_cast<const char*>(buf.data() + desc->VendorIdOffset);
    CHECK(std::strcmp(vendor, "WinLVM") == 0);
}

TEST(StorageSurface_StorageAdapterProperty_3Stage) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageAdapterProperty;
    query.QueryType = PropertyStandardQuery;

    // Stage 1: Under-header query
    NTSTATUS status = fix.runStorageIoctl(IOCTL_STORAGE_QUERY_PROPERTY,
                                         sizeof(query), sizeof(STORAGE_DESCRIPTOR_HEADER) - 1,
                                         &query, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_TOO_SMALL);

    // Stage 2: Header-only discovery query
    status = fix.runStorageIoctl(IOCTL_STORAGE_QUERY_PROPERTY,
                                sizeof(query), sizeof(STORAGE_DESCRIPTOR_HEADER),
                                &query, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_OVERFLOW);
    auto *hdr = reinterpret_cast<const STORAGE_DESCRIPTOR_HEADER*>(buf.data());
    CHECK_EQ(hdr->Size, sizeof(STORAGE_ADAPTER_DESCRIPTOR));

    // Stage 3: Full query
    status = fix.runStorageIoctl(IOCTL_STORAGE_QUERY_PROPERTY,
                                sizeof(query), sizeof(STORAGE_ADAPTER_DESCRIPTOR),
                                &query, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    auto *desc = reinterpret_cast<const STORAGE_ADAPTER_DESCRIPTOR*>(buf.data());
    CHECK_EQ(desc->MaximumTransferLength, 1024u * 1024u);
    CHECK_EQ((int)desc->BusType, (int)BusTypeVirtual);
}

TEST(StorageSurface_StorageAccessAlignmentProperty_3Stage) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageAccessAlignmentProperty;
    query.QueryType = PropertyStandardQuery;

    // Stage 1: Under-header query
    NTSTATUS status = fix.runStorageIoctl(IOCTL_STORAGE_QUERY_PROPERTY,
                                         sizeof(query), sizeof(STORAGE_DESCRIPTOR_HEADER) - 1,
                                         &query, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_TOO_SMALL);
    CHECK_EQ(info, sizeof(STORAGE_DESCRIPTOR_HEADER));

    // Stage 2: Header-only query (8 bytes) -> MUST return STATUS_BUFFER_OVERFLOW
    status = fix.runStorageIoctl(IOCTL_STORAGE_QUERY_PROPERTY,
                                sizeof(query), sizeof(STORAGE_DESCRIPTOR_HEADER),
                                &query, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_OVERFLOW);
    CHECK_EQ(info, sizeof(STORAGE_DESCRIPTOR_HEADER));
    auto *hdr = reinterpret_cast<const STORAGE_DESCRIPTOR_HEADER*>(buf.data());
    CHECK_EQ(hdr->Size, sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR));

    // Stage 3: Full descriptor query
    status = fix.runStorageIoctl(IOCTL_STORAGE_QUERY_PROPERTY,
                                sizeof(query), sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR),
                                &query, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    CHECK_EQ(info, sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR));
    auto *desc = reinterpret_cast<const STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR*>(buf.data());
    CHECK_EQ(desc->BytesPerLogicalSector, 512u);
    CHECK_EQ(desc->BytesPerPhysicalSector, 4096u);
    CHECK_EQ(desc->BytesPerCacheLine, 64u);
}

TEST(StorageSurface_StorageDeviceSeekPenaltyProperty_3Stage) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;

    // Stage 1: Under-header query
    NTSTATUS status = fix.runStorageIoctl(IOCTL_STORAGE_QUERY_PROPERTY,
                                         sizeof(query), sizeof(STORAGE_DESCRIPTOR_HEADER) - 1,
                                         &query, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_TOO_SMALL);

    // Stage 2: Header-only query -> MUST return STATUS_BUFFER_OVERFLOW
    status = fix.runStorageIoctl(IOCTL_STORAGE_QUERY_PROPERTY,
                                sizeof(query), sizeof(STORAGE_DESCRIPTOR_HEADER),
                                &query, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_OVERFLOW);
    auto *hdr = reinterpret_cast<const STORAGE_DESCRIPTOR_HEADER*>(buf.data());
    CHECK_EQ(hdr->Size, sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR));

    // Stage 3: Full descriptor query
    status = fix.runStorageIoctl(IOCTL_STORAGE_QUERY_PROPERTY,
                                sizeof(query), sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR),
                                &query, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    auto *desc = reinterpret_cast<const DEVICE_SEEK_PENALTY_DESCRIPTOR*>(buf.data());
    CHECK_EQ((int)desc->IncursSeekPenalty, (int)FALSE); // Virtual SSD hint
}

/* -------------------------------------------------------------------------
 * Group 4: Volume & Extent Management
 * ------------------------------------------------------------------------- */

TEST(StorageSurface_GetDeviceNumber) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;

    NTSTATUS status = fix.runStorageIoctl(IOCTL_STORAGE_GET_DEVICE_NUMBER, 0,
                                         sizeof(STORAGE_DEVICE_NUMBER) - 1, nullptr, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_TOO_SMALL);

    status = fix.runStorageIoctl(IOCTL_STORAGE_GET_DEVICE_NUMBER, 0,
                                sizeof(STORAGE_DEVICE_NUMBER), nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    auto *sdn = reinterpret_cast<const STORAGE_DEVICE_NUMBER*>(buf.data());
    CHECK_EQ(sdn->DeviceType, (DEVICE_TYPE)FILE_DEVICE_DISK);
    CHECK_EQ(sdn->DeviceNumber, (ULONG)(fix.ext.lv_index + 1));
    CHECK_EQ(sdn->PartitionNumber, 1u);
}

TEST(StorageSurface_VolumeDiskExtents_SingleAndMultiPV) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;

    // Buffer < sizeof(VOLUME_DISK_EXTENTS) -> STATUS_INVALID_PARAMETER
    NTSTATUS status = fix.runStorageIoctl(IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, 0,
                                         sizeof(VOLUME_DISK_EXTENTS) - 1, nullptr, buf, info);
    CHECK_EQ(status, STATUS_INVALID_PARAMETER);

    // Single-PV case: backing_count = 1
    fix.ext.backing_count = 1;
    fix.ext.pv_slot[0] = 3;
    status = fix.runStorageIoctl(IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, 0,
                                sizeof(VOLUME_DISK_EXTENTS), nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    auto *extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buf.data());
    CHECK_EQ(extents->NumberOfDiskExtents, 1u);
    CHECK_EQ(extents->Extents[0].DiskNumber, 4u); // pv_slot[0] + 1
    CHECK_EQ((uint64_t)extents->Extents[0].ExtentLength.QuadPart, fix.ext.byte_size);

    // Multi-PV case: backing_count = 2
    fix.ext.backing_count = 2;
    fix.ext.pv_slot[0] = 1;
    fix.ext.pv_slot[1] = 2;

    // Provide buffer only large enough for 1 extent -> STATUS_BUFFER_OVERFLOW with NumberOfDiskExtents = 2
    status = fix.runStorageIoctl(IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, 0,
                                sizeof(VOLUME_DISK_EXTENTS), nullptr, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_OVERFLOW);
    extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buf.data());
    CHECK_EQ(extents->NumberOfDiskExtents, 2u);

    // Provide full buffer for 2 extents
    size_t reqSize = FIELD_OFFSET(VOLUME_DISK_EXTENTS, Extents[0]) + 2 * sizeof(DISK_EXTENT);
    status = fix.runStorageIoctl(IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, 0,
                                reqSize, nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    CHECK_EQ(info, (uint64_t)reqSize);
    extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buf.data());
    CHECK_EQ(extents->NumberOfDiskExtents, 2u);
    CHECK_EQ(extents->Extents[0].DiskNumber, 2u); // pv_slot[0] + 1
    CHECK_EQ(extents->Extents[1].DiskNumber, 3u); // pv_slot[1] + 1
    CHECK_EQ((uint64_t)extents->Extents[0].ExtentLength.QuadPart +
             (uint64_t)extents->Extents[1].ExtentLength.QuadPart, fix.ext.byte_size);
}

TEST(StorageSurface_VolumeAttributesAndOnline) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;

    // IOCTL_VOLUME_IS_DYNAMIC
    NTSTATUS status = fix.runStorageIoctl(IOCTL_VOLUME_IS_DYNAMIC, 0,
                                         sizeof(BOOLEAN), nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    CHECK_EQ((int)buf[0], (int)FALSE);

    // IOCTL_VOLUME_ONLINE and POST_ONLINE
    status = fix.runStorageIoctl(IOCTL_VOLUME_ONLINE, 0, 0, nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    status = fix.runStorageIoctl(IOCTL_VOLUME_POST_ONLINE, 0, 0, nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);

    // IOCTL_VOLUME_GET_GPT_ATTRIBUTES
    status = fix.runStorageIoctl(IOCTL_VOLUME_GET_GPT_ATTRIBUTES, 0,
                                sizeof(VOLUME_GET_GPT_ATTRIBUTES_INFORMATION), nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    auto *gpt = reinterpret_cast<const VOLUME_GET_GPT_ATTRIBUTES_INFORMATION*>(buf.data());
    CHECK_EQ(gpt->GptAttributes, 0ull);
}

/* -------------------------------------------------------------------------
 * Group 5: Mount Manager Integration Contracts
 * ------------------------------------------------------------------------- */

TEST(StorageSurface_MountDevQueryDeviceName) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;

    // Stage 1: Under-header query
    NTSTATUS status = fix.runMountIoctl(IOCTL_MOUNTDEV_QUERY_DEVICE_NAME, 0,
                                       sizeof(MOUNTDEV_NAME) - 1, nullptr, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_TOO_SMALL);
    CHECK_EQ(info, sizeof(MOUNTDEV_NAME));

    // Stage 2: Header-only query -> STATUS_BUFFER_OVERFLOW with NameLength populated
    status = fix.runMountIoctl(IOCTL_MOUNTDEV_QUERY_DEVICE_NAME, 0,
                              sizeof(MOUNTDEV_NAME), nullptr, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_OVERFLOW);
    CHECK_EQ(info, sizeof(MOUNTDEV_NAME));
    auto *mountName = reinterpret_cast<const MOUNTDEV_NAME*>(buf.data());
    CHECK(mountName->NameLength > 0);

    // Stage 3: Full buffer
    size_t fullReq = offsetof(MOUNTDEV_NAME, Name) + mountName->NameLength;
    status = fix.runMountIoctl(IOCTL_MOUNTDEV_QUERY_DEVICE_NAME, 0,
                              fullReq, nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    CHECK_EQ(info, (uint64_t)fullReq);
    mountName = reinterpret_cast<const MOUNTDEV_NAME*>(buf.data());
    CHECK(std::memcmp(mountName->Name, fix.ext.devname, mountName->NameLength) == 0);
}

TEST(StorageSurface_MountDevQueryUniqueId) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;

    // Stage 1: Under-header query
    NTSTATUS status = fix.runMountIoctl(IOCTL_MOUNTDEV_QUERY_UNIQUE_ID, 0,
                                       sizeof(MOUNTDEV_UNIQUE_ID) - 1, nullptr, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_TOO_SMALL);
    CHECK_EQ(info, sizeof(MOUNTDEV_UNIQUE_ID));

    // Stage 2: Header-only query -> STATUS_BUFFER_OVERFLOW with UniqueIdLength = 32
    status = fix.runMountIoctl(IOCTL_MOUNTDEV_QUERY_UNIQUE_ID, 0,
                              sizeof(MOUNTDEV_UNIQUE_ID), nullptr, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_OVERFLOW);
    auto *uid = reinterpret_cast<const MOUNTDEV_UNIQUE_ID*>(buf.data());
    CHECK_EQ(uid->UniqueIdLength, 32u);

    // Stage 3: Full buffer
    size_t fullReq = offsetof(MOUNTDEV_UNIQUE_ID, UniqueId) + 32;
    status = fix.runMountIoctl(IOCTL_MOUNTDEV_QUERY_UNIQUE_ID, 0,
                              fullReq, nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    CHECK_EQ(info, (uint64_t)fullReq);
    uid = reinterpret_cast<const MOUNTDEV_UNIQUE_ID*>(buf.data());
    CHECK(std::memcmp(uid->UniqueId, fix.lv.id, 32) == 0);
}

TEST(StorageSurface_MountDevSuggestedLinkAndStableGuid) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;

    // Suggested link name is optional -> returns STATUS_NOT_IMPLEMENTED
    NTSTATUS status = fix.runMountIoctl(IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME, 0,
                                       64, nullptr, buf, info);
    CHECK_EQ(status, STATUS_NOT_IMPLEMENTED);

    // Stable GUID
    status = fix.runMountIoctl(IOCTL_MOUNTDEV_QUERY_STABLE_GUID, 0,
                              sizeof(MOUNTDEV_STABLE_GUID) - 1, nullptr, buf, info);
    CHECK_EQ(status, STATUS_BUFFER_TOO_SMALL);

    status = fix.runMountIoctl(IOCTL_MOUNTDEV_QUERY_STABLE_GUID, 0,
                              sizeof(MOUNTDEV_STABLE_GUID), nullptr, buf, info);
    CHECK_EQ(status, STATUS_SUCCESS);
    CHECK_EQ(info, sizeof(MOUNTDEV_STABLE_GUID));
    auto *mdsg = reinterpret_cast<const MOUNTDEV_STABLE_GUID*>(buf.data());
    // Verify non-zero deterministic GUID
    CHECK(mdsg->StableGuid.Data1 != 0 || mdsg->StableGuid.Data2 != 0);
}

/* -------------------------------------------------------------------------
 * Group 6: Volume File System Locking & DASD Control Codes (FSCTLs)
 * ------------------------------------------------------------------------- */

TEST(StorageSurface_VolumeFSCTLs_LockUnlockDismountDasd) {
    IoctlTestFixture fix;
    std::vector<uint8_t> buf;
    uint64_t info = 0;

    CHECK_EQ(fix.runMountIoctl(FSCTL_LOCK_VOLUME, 0, 0, nullptr, buf, info), STATUS_SUCCESS);
    CHECK_EQ(fix.runMountIoctl(FSCTL_UNLOCK_VOLUME, 0, 0, nullptr, buf, info), STATUS_SUCCESS);
    CHECK_EQ(fix.runMountIoctl(FSCTL_DISMOUNT_VOLUME, 0, 0, nullptr, buf, info), STATUS_SUCCESS);
    CHECK_EQ(fix.runMountIoctl(FSCTL_ALLOW_EXTENDED_DASD_IO, 0, 0, nullptr, buf, info), STATUS_SUCCESS);
}
