#include "lvmfmt/commit.h"
#include "lvmfmt/crc32.h"
#include "lvmfmt/label.h"
#include "lvmfmt/metadata.h"
#include <fstream>
#include <sstream>
#include <random>
#include <iomanip>
#include <cstring>
#include <ctime>

namespace lvmfmt {

namespace {

std::string generate_random_uuid() {
    static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(chars) - 2);

    std::string s;
    s.reserve(32);
    for (int i = 0; i < 32; i++) {
        s.push_back(chars[dis(gen)]);
    }
    return s;
}

void write_uint32_le(uint8_t* p, uint32_t val) {
    p[0] = static_cast<uint8_t>(val & 0xff);
    p[1] = static_cast<uint8_t>((val >> 8) & 0xff);
    p[2] = static_cast<uint8_t>((val >> 16) & 0xff);
    p[3] = static_cast<uint8_t>((val >> 24) & 0xff);
}

void write_uint64_le(uint8_t* p, uint64_t val) {
    for (int i = 0; i < 8; i++) {
        p[i] = static_cast<uint8_t>((val >> (i * 8)) & 0xff);
    }
}

uint64_t read_uint64_le(const uint8_t* p) {
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val |= (static_cast<uint64_t>(p[i]) << (i * 8));
    }
    return val;
}

} // namespace

std::string serialize_vg_to_text(const VolumeGroup& vg) {
    std::ostringstream ss;
    ss << vg.name << " {\n";
    ss << "id = \"" << format_uuid(vg.id) << "\"\n";
    ss << "seqno = " << vg.seqno << "\n";
    ss << "format = \"lvm2\"\n";
    ss << "status = [\"RESIZEABLE\", \"READ\", \"WRITE\"]\n";
    ss << "flags = []\n";
    ss << "extent_size = " << vg.extent_size_sectors << "\n";
    ss << "max_lv = 0\n";
    ss << "max_pv = 0\n";
    ss << "metadata_copies = 0\n\n";

    ss << "physical_volumes {\n";
    for (const auto& pv : vg.pvs) {
        ss << "\n" << pv.key << " {\n";
        ss << "id = \"" << format_uuid(pv.id) << "\"\n";
        ss << "device = \"" << (pv.device.empty() ? "/dev/unknown" : pv.device) << "\"\n";
        ss << "status = [\"ALLOCATABLE\"]\n";
        ss << "flags = []\n";
        ss << "dev_size = " << pv.dev_size_sectors << "\n";
        ss << "pe_start = " << pv.pe_start_sectors << "\n";
        ss << "pe_count = " << pv.pe_count << "\n";
        ss << "}\n";
    }
    ss << "}\n\n";

    ss << "logical_volumes {\n";
    for (const auto& lv : vg.lvs) {
        ss << "\n" << lv.name << " {\n";
        ss << "id = \"" << format_uuid(lv.id) << "\"\n";
        ss << "status = [\"VISIBLE\", \"READ\", \"WRITE\"]\n";
        ss << "flags = []\n";
        ss << "creation_time = " << std::time(nullptr) << "\n";
        ss << "creation_host = \"winlvm\"\n";
        ss << "segment_count = " << lv.segments.size() << "\n\n";

        size_t seg_idx = 1;
        for (const auto& seg : lv.segments) {
            ss << "segment" << seg_idx++ << " {\n";
            ss << "start_extent = " << seg.start_extent << "\n";
            ss << "extent_count = " << seg.extent_count << "\n";
            std::string seg_type = seg.type;
            if (seg_type == "linear") seg_type = "striped";
            ss << "type = \"" << seg_type << "\"\n";
            ss << "stripe_count = " << seg.stripe_count << "\n";
            if (seg.stripe_size_sectors > 0) {
                ss << "stripe_size = " << seg.stripe_size_sectors << "\n";
            }
            ss << "stripes = [\n";
            for (size_t s = 0; s < seg.stripes.size(); s++) {
                ss << "\"" << seg.stripes[s].target << "\", " << seg.stripes[s].start_extent;
                if (s + 1 < seg.stripes.size()) ss << ",\n";
                else ss << "\n";
            }
            ss << "]\n";
            ss << "}\n";
        }
        ss << "}\n";
    }
    ss << "}\n";
    ss << "}\n\n";
    ss << "contents = \"Text Format Volume Group\"\n";
    ss << "version = 1\n\n";
    ss << "description = \"Created by WinLVM\"\n\n";
    ss << "creation_host = \"winlvm\"\n";
    ss << "creation_time = " << std::time(nullptr) << "\n";
    return ss.str();
}

bool pvcreate_initialize(ByteSource& storage,
                         const PvCreateOptions& options,
                         std::string& generated_uuid,
                         std::string& error) {
    std::string uuid = options.custom_uuid.empty() ? generate_random_uuid() : options.custom_uuid;
    // Strip hyphens if present to ensure 32 chars
    std::string clean_uuid;
    for (char c : uuid) {
        if (c != '-') clean_uuid.push_back(c);
    }
    if (clean_uuid.size() != 32) {
        error = "PV UUID must be 32 characters (got " + std::to_string(clean_uuid.size()) + ")";
        return false;
    }
    generated_uuid = clean_uuid;

    std::uint64_t dev_size = options.override_device_size;
    if (dev_size == 0) {
        dev_size = storage.size();
    }
    if (dev_size < 10ULL * 1024ULL * 1024ULL) {
        error = "device size too small for LVM physical volume (minimum 10 MiB)";
        return false;
    }

    // 1. Prepare Sector 1 (Label header + PV header)
    std::vector<uint8_t> sector1(512, 0);
    // Magic "LABELONE"
    std::memcpy(&sector1[0], "LABELONE", 8);
    // Sector number 1
    write_uint64_le(&sector1[8], 1);
    // Offset 16..19: CRC32 placeholder
    // Offset 20..23: Offset to PV header: 32 bytes
    write_uint32_le(&sector1[20], 32);
    // Offset 24..31: Type: "LVM2 001"
    std::memcpy(&sector1[24], "LVM2 001", 8);

    // PV header at offset 32:
    // 32..63: PV UUID (32 bytes)
    std::memcpy(&sector1[32], clean_uuid.data(), 32);
    // 64..71: Device size in bytes
    write_uint64_le(&sector1[64], dev_size);

    // Data area descriptor at offset 72:
    // Offset 72..79: offset = pe_start_sectors * 512
    write_uint64_le(&sector1[72], options.pe_start_sectors * 512ULL);
    // Offset 80..87: size = 0 (means to end of disk)
    write_uint64_le(&sector1[80], 0);

    // Data area terminator (offset 88..103: 0, 0)
    write_uint64_le(&sector1[88], 0);
    write_uint64_le(&sector1[96], 0);

    // Metadata area descriptor at offset 104:
    // Offset 104..111: offset = 4096 (sector 8)
    write_uint64_le(&sector1[104], 4096ULL);
    // Offset 112..119: size = (pe_start_sectors * 512) - 4096
    std::uint64_t mda_size = (options.pe_start_sectors * 512ULL) - 4096ULL;
    write_uint64_le(&sector1[112], mda_size);

    // Metadata area terminator (offset 120..135: 0, 0)
    write_uint64_le(&sector1[120], 0);
    write_uint64_le(&sector1[128], 0);

    // Compute CRC32 of label header (bytes 20..511)
    uint32_t label_crc = lvm_crc32(&sector1[20], 512 - 20);
    write_uint32_le(&sector1[16], label_crc);

    // Write Sector 1
    if (!storage.write(512, sector1.data(), 512, error)) {
        error = "failed to write PV label to sector 1: " + error;
        return false;
    }

    // 2. Prepare and write empty MDA header at offset 4096
    std::vector<uint8_t> mda_header(512, 0);
    // Offset 4..19: Magic " LVM2 x[5A%r0N*>"
    std::memcpy(&mda_header[4], " LVM2 x[5A%r0N*>", 16);
    // Offset 20..23: Version 1
    write_uint32_le(&mda_header[20], 1);
    // Offset 24..31: Start offset 4096
    write_uint64_le(&mda_header[24], 4096ULL);
    // Offset 32..39: Size mda_size
    write_uint64_le(&mda_header[32], mda_size);

    // Compute CRC32 of MDA header (bytes 4..511)
    uint32_t mda_crc = lvm_crc32(&mda_header[4], 512 - 4);
    write_uint32_le(&mda_header[0], mda_crc);

    if (!storage.write(4096, mda_header.data(), 512, error)) {
        error = "failed to write MDA header at offset 4096: " + error;
        return false;
    }

    // Zero out first sector (sector 0) to remove old partition tables or signatures
    std::vector<uint8_t> zero_sec(512, 0);
    storage.write(0, zero_sec.data(), 512, error);

    return true;
}

bool pvremove_wipe(ByteSource& storage, std::string& error) {
    std::vector<uint8_t> zeros(8192, 0);
    if (!storage.write(0, zeros.data(), zeros.size(), error)) {
        error = "failed to wipe PV label and MDA header: " + error;
        return false;
    }
    return true;
}

bool commit_vg_metadata(const VolumeGroup& vg,
                        const std::vector<ByteSource*>& pv_storages,
                        std::string& error) {
    if (pv_storages.empty()) {
        error = "no physical volumes provided to commit metadata";
        return false;
    }

    std::string text = serialize_vg_to_text(vg);
    uint32_t text_crc = lvm_crc32(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    std::uint64_t text_size = text.size();

    // Commit to each PV's MDA
    for (auto* storage : pv_storages) {
        if (!storage) continue;

        // Read existing MDA header at 4096
        std::vector<uint8_t> mdah(512, 0);
        if (!storage->read(4096, mdah.data(), 512, error)) {
            error = "failed to read MDA header at 4096: " + error;
            return false;
        }

        uint64_t mda_size = read_uint64_le(&mdah[32]);
        if (mda_size < 65536) {
            mda_size = (2048ULL * 512ULL) - 4096ULL;
        }

        // Circular ring offset:
        // Slot 0 descriptor at 40: offset (8 bytes), size (8 bytes), checksum (4 bytes), flags (4 bytes)
        uint64_t prev_off = read_uint64_le(&mdah[40]);
        uint64_t prev_sz = read_uint64_le(&mdah[48]);

        uint64_t next_off = 512; // relative to MDA start (4096)
        if (prev_off >= 512 && prev_sz > 0) {
            next_off = (prev_off + prev_sz + 511ULL) & ~511ULL;
        }
        if (next_off + text_size + 512 > mda_size) {
            next_off = 512; // wrap around to beginning of ring
        }

        std::uint64_t write_pos = 4096ULL + next_off;

        // Step 1: Write serialized metadata text
        if (!storage->write(write_pos, text.data(), text_size, error)) {
            error = "failed to write metadata text at offset " + std::to_string(write_pos) + ": " + error;
            return false;
        }

        // Step 2: Update slot 0 descriptor in MDA header
        // Slot 0: offset = next_off, size = text_size, checksum = text_crc, flags = 0
        write_uint64_le(&mdah[40], next_off);
        write_uint64_le(&mdah[48], text_size);
        write_uint32_le(&mdah[56], text_crc);
        write_uint32_le(&mdah[60], 0);

        // Clear precommit slot 1 (offset 64..87)
        std::memset(&mdah[64], 0, 24);

        // Recompute MDA header checksum (bytes 4..511)
        uint32_t header_crc = lvm_crc32(&mdah[4], 512 - 4);
        write_uint32_le(&mdah[0], header_crc);

        // Write updated MDA header
        if (!storage->write(4096, mdah.data(), 512, error)) {
            error = "failed to update MDA header: " + error;
            return false;
        }
    }

    return true;
}

bool backup_vg_metadata(const VolumeGroup& vg,
                        std::string_view filepath,
                        std::string& error) {
    std::string text = serialize_vg_to_text(vg);
    std::ofstream out(std::string(filepath), std::ios::binary);
    if (!out) {
        error = "failed to open backup file for writing: " + std::string(filepath);
        return false;
    }
    out << "# Generated by WinLVM (LVM2 metadata backup)\n\n";
    out << text;
    out.close();
    return true;
}

bool restore_vg_metadata(std::string_view filepath,
                         const std::vector<ByteSource*>& pv_storages,
                         std::string& error) {
    std::ifstream in(std::string(filepath), std::ios::binary);
    if (!in) {
        error = "failed to open backup file for reading: " + std::string(filepath);
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    auto vg_opt = parse_vg_metadata(text, error);
    if (!vg_opt) {
        error = "failed to parse metadata backup: " + error;
        return false;
    }

    return commit_vg_metadata(*vg_opt, pv_storages, error);
}

} // namespace lvmfmt
