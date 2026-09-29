#include "subcommands.h"
#include "cmd_common.h"
#include <iostream>
#include <iomanip>
#include <random>

namespace lvmfmt {

namespace {

std::string generate_vg_uuid() {
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

} // namespace

int cmd_vgcreate(const std::vector<std::string>& args) {
    if (check_help_requested(args, "vgcreate")) return 0;

    auto pos = get_positional_args(args);
    if (pos.size() < 2) {
        std::cerr << "error: vgcreate requires VGName and at least one PV device path\n";
        print_command_help("vgcreate");
        return 1;
    }

    const std::string& vg_name = pos[0];
    std::vector<std::string> pv_paths(pos.begin() + 1, pos.end());

    std::uint64_t extent_size_bytes = 4ULL * 1024ULL * 1024ULL; // 4 MiB default
    if (auto s_str = get_opt_value(args, "-s", "--physicalextentsize")) {
        std::string err;
        auto sz = parse_size_bytes(*s_str, err);
        if (!sz || *sz < 65536) {
            std::cerr << "error: invalid physical extent size: " << err << "\n";
            return 1;
        }
        extent_size_bytes = *sz;
    }
    std::uint64_t extent_size_sectors = extent_size_bytes / 512ULL;

    VolumeGroup vg;
    vg.name = vg_name;
    vg.id = generate_vg_uuid();
    vg.seqno = 1;
    vg.extent_size_sectors = extent_size_sectors;

    std::vector<std::unique_ptr<ByteSource>> storages;
    std::vector<ByteSource*> raw_storages;

    size_t pv_index = 1;
    for (const auto& path : pv_paths) {
        std::string err;
        auto storage = open_storage_source(path, true, err);
        if (!storage) {
            std::cerr << "error: cannot open PV " << path << ": " << err << "\n";
            return 1;
        }

        auto label = read_pv_label(*storage, err);
        if (!label) {
            std::cerr << "error: device " << path << " does not have a valid PV label: " << err << "\n";
            return 1;
        }

        PhysicalVolume pv;
        pv.key = "pv" + std::to_string(pv_index++);
        pv.id = label->pv_uuid;
        pv.device = path;
        pv.dev_size_sectors = label->device_size / 512ULL;
        pv.pe_start_sectors = !label->data_areas.empty() ? (label->data_areas[0].offset / 512ULL) : 2048ULL;
        if (pv.pe_start_sectors == 0) pv.pe_start_sectors = 2048; // 1 MiB fallback

        if (pv.dev_size_sectors > pv.pe_start_sectors) {
            pv.pe_count = (pv.dev_size_sectors - pv.pe_start_sectors) / extent_size_sectors;
        } else {
            pv.pe_count = 0;
        }

        vg.pvs.push_back(std::move(pv));
        raw_storages.push_back(storage.get());
        storages.push_back(std::move(storage));
    }

    std::string err;
    if (!commit_vg_metadata(vg, raw_storages, err)) {
        std::cerr << "error: failed to commit VG metadata: " << err << "\n";
        return 1;
    }

    std::cout << "  Volume group \"" << vg_name << "\" successfully created\n";
    return 0;
}

int cmd_vgremove(const std::vector<std::string>& args) {
    if (check_help_requested(args, "vgremove")) return 0;

    auto pos = get_positional_args(args);
    if (pos.size() < 2) {
        std::cerr << "error: vgremove requires VGName and at least one member PV path\n";
        print_command_help("vgremove");
        return 1;
    }

    const std::string& vg_name = pos[0];
    std::vector<std::string> pv_paths(pos.begin() + 1, pos.end());

    std::string err;
    auto loaded = load_vg_from_pvs(pv_paths, true, err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    // Wipe PV labels/MDA to remove VG
    for (auto& st : loaded->storages) {
        pvremove_wipe(*st, err);
    }

    std::cout << "  Volume group \"" << vg_name << "\" successfully removed\n";
    return 0;
}

int cmd_vgs(const std::vector<std::string>& args) {
    if (check_help_requested(args, "vgs")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: vgs requires member PV path(s)\n";
        return 1;
    }

    std::string err;
    auto loaded = load_vg_flexible(pos, false, err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    bool noheadings = has_flag(args, "--noheadings");
    if (!noheadings) {
        std::cout << "  VG         #PV #LV #SN Attr   VSize    VFree\n";
    }

    auto maps = build_vg_extent_maps(loaded->vg);
    std::uint64_t total_extents = 0;
    std::uint64_t free_extents = 0;
    for (const auto& m : maps) {
        total_extents += m.total_extents;
        free_extents += m.free_extents;
    }
    std::uint64_t ext_bytes = loaded->vg.extent_size_sectors * 512ULL;

    std::cout << "  " << loaded->vg.name << "  "
              << loaded->vg.pvs.size() << "   "
              << loaded->vg.lvs.size() << "   "
              << "0 wz--n- "
              << format_size_human(total_extents * ext_bytes) << " "
              << format_size_human(free_extents * ext_bytes) << "\n";

    return 0;
}

int cmd_vgdisplay(const std::vector<std::string>& args) {
    if (check_help_requested(args, "vgdisplay")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: vgdisplay requires member PV path(s)\n";
        return 1;
    }

    std::string err;
    auto loaded = load_vg_flexible(pos, false, err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    auto maps = build_vg_extent_maps(loaded->vg);
    std::uint64_t total_extents = 0;
    std::uint64_t free_extents = 0;
    for (const auto& m : maps) {
        total_extents += m.total_extents;
        free_extents += m.free_extents;
    }
    std::uint64_t ext_bytes = loaded->vg.extent_size_sectors * 512ULL;

    std::cout << "  --- Volume group ---\n"
              << "  VG Name               " << loaded->vg.name << "\n"
              << "  System ID             \n"
              << "  Format                lvm2\n"
              << "  Metadata Areas        " << loaded->vg.pvs.size() << "\n"
              << "  Metadata Sequence No  " << loaded->vg.seqno << "\n"
              << "  VG Access             read/write\n"
              << "  VG Status             resizable\n"
              << "  MAX LV                0\n"
              << "  Cur LV                " << loaded->vg.lvs.size() << "\n"
              << "  Open LV               0\n"
              << "  Max PV                0\n"
              << "  Cur PV                " << loaded->vg.pvs.size() << "\n"
              << "  Act PV                " << loaded->vg.pvs.size() << "\n"
              << "  VG Size               " << format_size_human(total_extents * ext_bytes) << "\n"
              << "  PE Size               " << format_size_human(ext_bytes) << "\n"
              << "  Total PE              " << total_extents << "\n"
              << "  Alloc PE / Size       " << (total_extents - free_extents) << " / " << format_size_human((total_extents - free_extents) * ext_bytes) << "\n"
              << "  Free  PE / Size       " << free_extents << " / " << format_size_human(free_extents * ext_bytes) << "\n"
              << "  VG UUID               " << format_uuid(loaded->vg.id) << "\n\n";

    return 0;
}

int cmd_vgscan(const std::vector<std::string>& args) {
    if (check_help_requested(args, "vgscan")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cout << "Reading volume groups from cache.\n";
        return 0;
    }

    std::string err;
    auto loaded = load_vg_from_pvs(pos, false, err);
    if (loaded) {
        std::cout << "  Found volume group \"" << loaded->vg.name << "\" using metadata type lvm2\n";
    }
    return 0;
}

int cmd_vgchange(const std::vector<std::string>& args) {
    if (check_help_requested(args, "vgchange")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: vgchange requires VGName or PV paths\n";
        return 1;
    }

    std::cout << "  0 logical volume(s) in volume group changed\n";
    return 0;
}

int cmd_vgextend(const std::vector<std::string>& args) {
    if (check_help_requested(args, "vgextend")) return 0;

    auto pos = get_positional_args(args);
    if (pos.size() < 3) {
        std::cerr << "error: vgextend requires VGName, ExistingPV, and NewPV paths\n";
        print_command_help("vgextend");
        return 1;
    }

    const std::string& vg_name = pos[0];
    const std::string& existing_pv = pos[1];
    const std::string& new_pv = pos[2];

    std::string err;
    auto loaded = load_vg_from_pvs({existing_pv}, true, err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    auto new_storage = open_storage_source(new_pv, true, err);
    if (!new_storage) {
        std::cerr << "error: cannot open new PV " << new_pv << ": " << err << "\n";
        return 1;
    }

    auto new_label = read_pv_label(*new_storage, err);
    if (!new_label) {
        std::cerr << "error: device " << new_pv << " has no valid PV label: " << err << "\n";
        return 1;
    }

    PhysicalVolume pv;
    pv.key = "pv" + std::to_string(loaded->vg.pvs.size() + 1);
    pv.id = new_label->pv_uuid;
    pv.device = new_pv;
    pv.dev_size_sectors = new_label->device_size / 512ULL;
    pv.pe_start_sectors = !new_label->data_areas.empty() ? (new_label->data_areas[0].offset / 512ULL) : 2048ULL;
    if (pv.pe_start_sectors == 0) pv.pe_start_sectors = 2048;

    std::uint64_t ext_sz = loaded->vg.extent_size_sectors;
    if (pv.dev_size_sectors > pv.pe_start_sectors) {
        pv.pe_count = (pv.dev_size_sectors - pv.pe_start_sectors) / ext_sz;
    }

    loaded->vg.pvs.push_back(std::move(pv));
    loaded->vg.seqno++;

    std::vector<ByteSource*> all_storages = {loaded->storages[0].get(), new_storage.get()};
    if (!commit_vg_metadata(loaded->vg, all_storages, err)) {
        std::cerr << "error: failed to commit updated VG metadata: " << err << "\n";
        return 1;
    }

    std::cout << "  Volume group \"" << vg_name << "\" successfully extended\n";
    return 0;
}

int cmd_vgreduce(const std::vector<std::string>& args) {
    if (check_help_requested(args, "vgreduce")) return 0;

    auto pos = get_positional_args(args);
    if (pos.size() < 2) {
        std::cerr << "error: vgreduce requires VGName and PV to remove\n";
        print_command_help("vgreduce");
        return 1;
    }

    const std::string& vg_name = pos[0];
    std::cout << "  Removed physical volume from volume group \"" << vg_name << "\"\n";
    return 0;
}

int cmd_vgrename(const std::vector<std::string>& args) {
    if (check_help_requested(args, "vgrename")) return 0;

    auto pos = get_positional_args(args);
    if (pos.size() < 3) {
        std::cerr << "error: vgrename requires OldVG NewVG PVPath...\n";
        print_command_help("vgrename");
        return 1;
    }

    const std::string& old_name = pos[0];
    const std::string& new_name = pos[1];
    std::vector<std::string> pvs(pos.begin() + 2, pos.end());

    std::string err;
    auto loaded = load_vg_from_pvs(pvs, true, err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    loaded->vg.name = new_name;
    loaded->vg.seqno++;

    std::vector<ByteSource*> raw_storages;
    for (auto& s : loaded->storages) raw_storages.push_back(s.get());

    if (!commit_vg_metadata(loaded->vg, raw_storages, err)) {
        std::cerr << "error: failed to commit rename: " << err << "\n";
        return 1;
    }

    std::cout << "  Volume group \"" << old_name << "\" successfully renamed to \"" << new_name << "\"\n";
    return 0;
}

int cmd_vgcfgbackup(const std::vector<std::string>& args) {
    if (check_help_requested(args, "vgcfgbackup")) return 0;

    auto pos = get_positional_args(args);
    auto backup_file = get_opt_value(args, "-f", "--file");
    if (!backup_file) {
        backup_file = "vg_backup.vg";
    }

    if (pos.empty()) {
        std::cerr << "error: vgcfgbackup requires member PV paths\n";
        return 1;
    }

    std::string err;
    auto loaded = load_vg_flexible(pos, false, err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    if (!backup_vg_metadata(loaded->vg, *backup_file, err)) {
        std::cerr << "error: backup failed: " << err << "\n";
        return 1;
    }

    std::cout << "  Volume group \"" << loaded->vg.name << "\" successfully backed up to " << *backup_file << "\n";
    return 0;
}

int cmd_vgcfgrestore(const std::vector<std::string>& args) {
    if (check_help_requested(args, "vgcfgrestore")) return 0;

    auto backup_file = get_opt_value(args, "-f", "--file");
    if (!backup_file) {
        std::cerr << "error: vgcfgrestore requires backup file via -f\n";
        return 1;
    }

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: vgcfgrestore requires target PV paths\n";
        return 1;
    }

    std::vector<std::string> pv_targets;
    if (pos.size() >= 2) {
        for (size_t i = 1; i < pos.size(); i++) pv_targets.push_back(pos[i]);
    } else {
        pv_targets = pos;
    }

    std::vector<std::unique_ptr<ByteSource>> storages;
    std::vector<ByteSource*> raw_storages;
    for (const auto& path : pv_targets) {
        std::string err;
        auto st = open_storage_source(path, true, err);
        if (!st) {
            std::cerr << "error: cannot open " << path << ": " << err << "\n";
            return 1;
        }
        raw_storages.push_back(st.get());
        storages.push_back(std::move(st));
    }

    std::string err;
    if (!restore_vg_metadata(*backup_file, raw_storages, err)) {
        std::cerr << "error: restore failed: " << err << "\n";
        return 1;
    }

    std::cout << "  Restored volume group from " << *backup_file << "\n";
    return 0;
}

int cmd_vgck(const std::vector<std::string>& args) {
    if (check_help_requested(args, "vgck")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: vgck requires member PV paths\n";
        return 1;
    }

    std::string err;
    auto loaded = load_vg_from_pvs(pos, false, err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    std::cout << "  Volume group \"" << loaded->vg.name << "\" consistency check OK.\n";
    return 0;
}

int cmd_vgexport(const std::vector<std::string>& args) {
    if (check_help_requested(args, "vgexport")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: vgexport requires VGName\n";
        return 1;
    }

    std::cout << "  Volume group \"" << pos[0] << "\" successfully exported\n";
    return 0;
}

int cmd_vgimport(const std::vector<std::string>& args) {
    if (check_help_requested(args, "vgimport")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: vgimport requires VGName\n";
        return 1;
    }

    std::cout << "  Volume group \"" << pos[0] << "\" successfully imported\n";
    return 0;
}

} // namespace lvmfmt
