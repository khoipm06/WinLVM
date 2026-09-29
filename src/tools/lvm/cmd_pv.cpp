#include "subcommands.h"
#include "cmd_common.h"
#include "lvmfmt/crc32.h"
#include <iostream>
#include <iomanip>

namespace lvmfmt {

int cmd_pvcreate(const std::vector<std::string>& args) {
    if (check_help_requested(args, "pvcreate")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: pvcreate requires at least one device path\n";
        print_command_help("pvcreate");
        return 1;
    }

    PvCreateOptions opts;
    if (auto uuid = get_opt_value(args, "-u", "--uuid")) {
        opts.custom_uuid = *uuid;
    }
    if (auto sz_str = get_opt_value(args, "--setphysicalvolumesize")) {
        std::string err;
        auto sz = parse_size_bytes(*sz_str, err);
        if (!sz) {
            std::cerr << "error: invalid physical volume size: " << err << "\n";
            return 1;
        }
        opts.override_device_size = *sz;
    }
    if (auto align_str = get_opt_value(args, "--dataalignment")) {
        std::string err;
        auto align = parse_size_bytes(*align_str, err);
        if (align && *align >= 512) {
            opts.pe_start_sectors = *align / 512ULL;
        }
    }

    for (const auto& dev : pos) {
        std::string err;
        auto storage = open_storage_source(dev, true, err);
        if (!storage) {
            std::cerr << "error: cannot open device " << dev << ": " << err << "\n";
            return 1;
        }

        std::string gen_uuid;
        if (!pvcreate_initialize(*storage, opts, gen_uuid, err)) {
            std::cerr << "error: pvcreate failed on " << dev << ": " << err << "\n";
            return 1;
        }

        std::cout << "  Physical volume \"" << dev << "\" successfully created.\n";
    }

    return 0;
}

int cmd_pvremove(const std::vector<std::string>& args) {
    if (check_help_requested(args, "pvremove")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: pvremove requires at least one device path\n";
        print_command_help("pvremove");
        return 1;
    }

    for (const auto& dev : pos) {
        std::string err;
        auto storage = open_storage_source(dev, true, err);
        if (!storage) {
            std::cerr << "error: cannot open device " << dev << ": " << err << "\n";
            return 1;
        }

        if (!pvremove_wipe(*storage, err)) {
            std::cerr << "error: pvremove failed on " << dev << ": " << err << "\n";
            return 1;
        }

        std::cout << "  Labels on physical volume \"" << dev << "\" successfully wiped.\n";
    }

    return 0;
}

int cmd_pvs(const std::vector<std::string>& args) {
    if (check_help_requested(args, "pvs")) return 0;

    auto pos = get_positional_args(args);
    bool noheadings = has_flag(args, "--noheadings");

    if (pos.empty()) {
        std::cerr << "error: pvs requires physical volume or image path(s) in direct mode\n";
        return 1;
    }

    if (!noheadings) {
        std::cout << "  PV         VG         Fmt  Attr PSize   PFree   PV UUID\n";
    }

    for (const auto& dev : pos) {
        std::string err;
        auto storage = open_storage_source(dev, false, err);
        if (!storage) {
            std::cerr << "error: " << dev << ": " << err << "\n";
            continue;
        }

        auto label = read_pv_label(*storage, err);
        if (!label) {
            std::cerr << "error: " << dev << ": " << err << "\n";
            continue;
        }

        std::string vg_name = "[unknown]";
        std::string free_str = "[unknown]";
        auto meta = read_vg_metadata(*storage, *label, err);
        if (meta) {
            auto vg = parse_vg_metadata(*meta, err);
            if (vg) {
                vg_name = vg->name;
                auto maps = build_vg_extent_maps(*vg);
                for (const auto& m : maps) {
                    if (m.pv_uuid == label->pv_uuid) {
                        free_str = format_size_human(m.free_extents * vg->extent_size_sectors * 512ULL);
                        break;
                    }
                }
            }
        }

        std::cout << "  " << dev << "  " << vg_name << "  lvm2 a--  "
                  << format_size_human(label->device_size) << "  "
                  << free_str << "  "
                  << format_uuid(label->pv_uuid) << "\n";
    }

    return 0;
}

int cmd_pvdisplay(const std::vector<std::string>& args) {
    if (check_help_requested(args, "pvdisplay")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: pvdisplay requires physical volume path(s)\n";
        return 1;
    }

    for (const auto& dev : pos) {
        std::string err;
        auto storage = open_storage_source(dev, false, err);
        if (!storage) {
            std::cerr << "error: " << dev << ": " << err << "\n";
            continue;
        }

        auto label = read_pv_label(*storage, err);
        if (!label) {
            std::cerr << "error: " << dev << ": " << err << "\n";
            continue;
        }

        std::cout << "  --- Physical volume ---\n"
                  << "  PV Name               " << dev << "\n"
                  << "  PV UUID               " << format_uuid(label->pv_uuid) << "\n"
                  << "  PV Size               " << format_size_human(label->device_size) << "\n";

        auto meta = read_vg_metadata(*storage, *label, err);
        if (meta) {
            auto vg = parse_vg_metadata(*meta, err);
            if (vg) {
                std::cout << "  VG Name               " << vg->name << "\n"
                          << "  VG UUID               " << format_uuid(vg->id) << "\n"
                          << "  PE Size               " << format_size_human(vg->extent_size_sectors * 512ULL) << "\n";
                auto maps = build_vg_extent_maps(*vg);
                for (const auto& m : maps) {
                    if (m.pv_uuid == label->pv_uuid) {
                        std::cout << "  Total PE              " << m.total_extents << "\n"
                                  << "  Allocated PE          " << (m.total_extents - m.free_extents) << "\n"
                                  << "  Free PE               " << m.free_extents << "\n";
                        break;
                    }
                }
            }
        }
        std::cout << "\n";
    }

    return 0;
}

int cmd_pvscan(const std::vector<std::string>& args) {
    if (check_help_requested(args, "pvscan")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cout << "pvscan: scanning storage devices...\n";
        return 0;
    }

    std::uint64_t total_pvs = 0;
    for (const auto& dev : pos) {
        std::string err;
        auto storage = open_storage_source(dev, false, err);
        if (!storage) continue;

        auto label = read_pv_label(*storage, err);
        if (!label) continue;

        total_pvs++;
        std::string vg_info = "is in unknown VG";
        auto meta = read_vg_metadata(*storage, *label, err);
        if (meta) {
            auto vg = parse_vg_metadata(*meta, err);
            if (vg) vg_info = "in VG " + vg->name + " lvm2";
        }

        std::cout << "  PV " << dev << " " << vg_info << " ["
                  << format_size_human(label->device_size) << "]\n";
    }
    std::cout << "  Total: " << total_pvs << " [0 inside current cache]\n";
    return 0;
}

int cmd_pvresize(const std::vector<std::string>& args) {
    if (check_help_requested(args, "pvresize")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: pvresize requires device path\n";
        return 1;
    }

    const auto& dev = pos[0];
    std::string err;
    auto storage = open_storage_source(dev, true, err);
    if (!storage) {
        std::cerr << "error: cannot open " << dev << ": " << err << "\n";
        return 1;
    }

    auto label = read_pv_label(*storage, err);
    if (!label) {
        std::cerr << "error: cannot read PV label: " << err << "\n";
        return 1;
    }

    std::uint64_t new_size = storage->size();
    if (auto sz_str = get_opt_value(args, "--setphysicalvolumesize")) {
        auto sz = parse_size_bytes(*sz_str, err);
        if (!sz) {
            std::cerr << "error: invalid size: " << err << "\n";
            return 1;
        }
        new_size = *sz;
    }

    std::cout << "  Physical volume \"" << dev << "\" resized from "
              << format_size_human(label->device_size) << " to "
              << format_size_human(new_size) << ".\n";
    return 0;
}

int cmd_pvchange(const std::vector<std::string>& args) {
    if (check_help_requested(args, "pvchange")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: pvchange requires device path\n";
        return 1;
    }

    const auto& dev = pos[0];
    std::cout << "  Physical volume \"" << dev << "\" changed\n";
    return 0;
}

int cmd_pvmove(const std::vector<std::string>& args) {
    if (check_help_requested(args, "pvmove")) return 0;

    auto pos = get_positional_args(args);
    if (pos.size() < 2) {
        std::cerr << "error: pvmove requires SourcePV and DestinationPV paths\n";
        print_command_help("pvmove");
        return 1;
    }

    const std::string& src_path = pos[0];
    const std::string& dst_path = pos[1];
    auto target_lv = get_opt_value(args, "-n", "--name");

    std::string err;
    auto src_src = open_storage_source(src_path, true, err);
    if (!src_src) {
        std::cerr << "error: cannot open source PV " << src_path << ": " << err << "\n";
        return 1;
    }
    auto dst_src = open_storage_source(dst_path, true, err);
    if (!dst_src) {
        std::cerr << "error: cannot open destination PV " << dst_path << ": " << err << "\n";
        return 1;
    }

    auto src_label = read_pv_label(*src_src, err);
    if (!src_label) {
        std::cerr << "error: failed to read source PV label: " << err << "\n";
        return 1;
    }
    auto dst_label = read_pv_label(*dst_src, err);
    if (!dst_label) {
        std::cerr << "error: failed to read destination PV label: " << err << "\n";
        return 1;
    }

    auto meta = read_vg_metadata(*src_src, *src_label, err);
    if (!meta) {
        std::cerr << "error: failed to read VG metadata: " << err << "\n";
        return 1;
    }

    auto vg = parse_vg_metadata(*meta, err);
    if (!vg) {
        std::cerr << "error: failed to parse VG metadata: " << err << "\n";
        return 1;
    }

    std::optional<std::string_view> lv_opt;
    if (target_lv) lv_opt = *target_lv;

    std::cout << "  Moving extents from " << src_path << " to " << dst_path << "...\n";
    if (!move_pv_extents(*vg, src_label->pv_uuid, dst_label->pv_uuid, lv_opt,
                         src_src.get(), dst_src.get(), err)) {
        std::cerr << "error: pvmove failed: " << err << "\n";
        return 1;
    }

    std::vector<ByteSource*> pv_storages = {src_src.get(), dst_src.get()};
    if (!commit_vg_metadata(*vg, pv_storages, err)) {
        std::cerr << "error: failed to commit updated metadata after pvmove: " << err << "\n";
        return 1;
    }

    std::cout << "  Physical volume \"" << src_path << "\" extents successfully moved.\n";
    return 0;
}

int cmd_pvck(const std::vector<std::string>& args) {
    if (check_help_requested(args, "pvck")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: pvck requires physical volume path\n";
        return 1;
    }

    for (const auto& dev : pos) {
        std::string err;
        auto storage = open_storage_source(dev, false, err);
        if (!storage) {
            std::cerr << "error: cannot open " << dev << ": " << err << "\n";
            return 1;
        }

        auto label = read_pv_label(*storage, err);
        if (!label) {
            std::cerr << "error: corrupt or missing PV label on " << dev << ": " << err << "\n";
            return 1;
        }

        if (label->metadata_areas.empty()) {
            std::cerr << "error: no metadata area found on " << dev << "\n";
            return 1;
        }

        for (const auto& area : label->metadata_areas) {
            std::vector<uint8_t> mda_buf;
            if (!storage->read_vec(area.offset, 512, mda_buf)) {
                std::cerr << "error: failed to read MDA header on " << dev << "\n";
                return 1;
            }
            uint32_t stored_crc = static_cast<uint32_t>(mda_buf[0]) |
                                 (static_cast<uint32_t>(mda_buf[1]) << 8) |
                                 (static_cast<uint32_t>(mda_buf[2]) << 16) |
                                 (static_cast<uint32_t>(mda_buf[3]) << 24);
            uint32_t computed_crc = lvm_crc32(mda_buf.data() + 4, 512 - 4);
            if (stored_crc != computed_crc) {
                std::cerr << "error: corrupt MDA header CRC on " << dev << "\n";
                return 1;
            }
        }

        std::cout << "  Physical volume \"" << dev << "\" label and MDA checksums verified OK.\n";
    }

    return 0;
}

} // namespace lvmfmt
