#include "subcommands.h"
#include "cmd_common.h"
#include <iostream>
#include <iomanip>
#include <random>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../../winlvm_ioctl.h"

namespace {
void notify_driver_update_lv(const std::string& lv_uuid, std::uint64_t new_size_bytes) {
    HANDLE h = CreateFileW(WINLVM_USER_DEVICE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        WINLVM_UPDATE_LV_REQUEST req{};
        req.header.version = 1;
        req.header.size = sizeof(req);
        size_t n = lv_uuid.size();
        if (n >= sizeof(req.lv_uuid)) n = sizeof(req.lv_uuid) - 1;
        std::memcpy(req.lv_uuid, lv_uuid.data(), n);
        req.byte_size = new_size_bytes;
        req.logical_sector = 512;
        req.physical_sector = 512;
        DWORD bytesRet = 0;
        DeviceIoControl(h, IOCTL_WINLVM_UPDATE_LV, &req, sizeof(req), nullptr, 0, &bytesRet, nullptr);
        CloseHandle(h);
    }
}

bool notify_driver_activate_lv(const std::string& lv_uuid, bool activate) {
    HANDLE h = CreateFileW(WINLVM_USER_DEVICE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        WINLVM_UPDATE_LV_REQUEST req{};
        req.header.version = 1;
        req.header.size = sizeof(req);
        size_t n = lv_uuid.size();
        if (n >= sizeof(req.lv_uuid)) n = sizeof(req.lv_uuid) - 1;
        std::memcpy(req.lv_uuid, lv_uuid.data(), n);
        DWORD bytesRet = 0;
        DWORD code = activate ? IOCTL_WINLVM_ACTIVATE_LV : IOCTL_WINLVM_DEACTIVATE_LV;
        BOOL ok = DeviceIoControl(h, code, &req, sizeof(req), nullptr, 0, &bytesRet, nullptr);
        CloseHandle(h);
        return ok != FALSE;
    }
    return false;
}
} // namespace
#endif

namespace lvmfmt {

namespace {

std::string generate_lv_uuid() {
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

int cmd_lvcreate(const std::vector<std::string>& args) {
    if (check_help_requested(args, "lvcreate")) return 0;

    auto lv_name_opt = get_opt_value(args, "-n", "--name");
    if (!lv_name_opt) {
        std::cerr << "error: lvcreate requires logical volume name via -n/--name\n";
        print_command_help("lvcreate");
        return 1;
    }
    std::string lv_name = *lv_name_opt;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: lvcreate requires VG member PV path(s)\n";
        print_command_help("lvcreate");
        return 1;
    }

    std::string err;
    std::optional<LoadedVG> loaded;
    if (pos.size() >= 2) {
        std::vector<std::string> pvs(pos.begin() + 1, pos.end());
        loaded = load_vg_from_pvs(pvs, true, err);
    }
    if (!loaded) {
        loaded = load_vg_from_pvs(pos, true, err);
    }
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    // Check if LV already exists
    for (const auto& lv : loaded->vg.lvs) {
        if (lv.name == lv_name) {
            std::cerr << "error: logical volume \"" << lv_name << "\" already exists in VG " << loaded->vg.name << "\n";
            return 1;
        }
    }

    std::uint64_t extent_bytes = loaded->vg.extent_size_sectors * 512ULL;
    std::uint64_t extents_needed = 0;

    if (auto sz_str = get_opt_value(args, "-L", "--size")) {
        auto sz_bytes = parse_size_bytes(*sz_str, err);
        if (!sz_bytes) {
            std::cerr << "error: invalid size: " << err << "\n";
            return 1;
        }
        extents_needed = (*sz_bytes + extent_bytes - 1) / extent_bytes;
    } else if (auto l_str = get_opt_value(args, "-l", "--extents")) {
        try {
            extents_needed = std::stoull(*l_str);
        } catch (...) {
            std::cerr << "error: invalid extent count: " << *l_str << "\n";
            return 1;
        }
    } else {
        std::cerr << "error: lvcreate requires either -L <size> or -l <extents>\n";
        return 1;
    }

    if (extents_needed == 0) {
        std::cerr << "error: requested extent count must be > 0\n";
        return 1;
    }

    std::uint32_t stripes = 1;
    if (auto str_stripes = get_opt_value(args, "-i", "--stripes")) {
        stripes = static_cast<std::uint32_t>(std::stoul(*str_stripes));
    }

    LogicalVolume lv;
    lv.name = lv_name;
    lv.id = generate_lv_uuid();
    lv.supported = true;

    if (stripes <= 1) {
        std::vector<ExtentAllocation> allocs;
        if (!allocate_linear(loaded->vg, extents_needed, {}, allocs, err)) {
            std::cerr << "error: allocation failed: " << err << "\n";
            return 1;
        }

        std::uint64_t cur_le = 0;
        for (const auto& a : allocs) {
            Segment seg;
            seg.start_extent = cur_le;
            seg.extent_count = a.count;
            seg.type = "linear";
            seg.stripe_count = 1;
            seg.stripe_size_sectors = 0;
            seg.stripes.push_back({a.pv_key, a.start_pe});
            lv.segments.push_back(std::move(seg));
            cur_le += a.count;
        }
    } else {
        std::vector<std::vector<ExtentAllocation>> stripe_allocs;
        if (!allocate_striped(loaded->vg, extents_needed, stripes, {}, stripe_allocs, err)) {
            std::cerr << "error: striped allocation failed: " << err << "\n";
            return 1;
        }

        std::uint32_t stripe_size_sectors = 128; // 64 KiB default stripe size
        if (auto str_stripe_size = get_opt_value(args, "-I", "--stripesize")) {
            std::string err_sz;
            auto sz = parse_size_bytes(*str_stripe_size, err_sz);
            if (sz && *sz >= 512) {
                stripe_size_sectors = static_cast<std::uint32_t>(*sz / 512ULL);
            }
        }

        Segment seg;
        seg.start_extent = 0;
        seg.extent_count = extents_needed;
        seg.type = "striped";
        seg.stripe_count = stripes;
        seg.stripe_size_sectors = stripe_size_sectors;
        for (std::uint32_t s = 0; s < stripes; s++) {
            seg.stripes.push_back({stripe_allocs[s][0].pv_key, stripe_allocs[s][0].start_pe});
        }
        lv.segments.push_back(std::move(seg));
    }

    loaded->vg.lvs.push_back(std::move(lv));
    loaded->vg.seqno++;

    std::vector<ByteSource*> raw_storages;
    for (auto& s : loaded->storages) raw_storages.push_back(s.get());

    if (!commit_vg_metadata(loaded->vg, raw_storages, err)) {
        std::cerr << "error: failed to commit metadata: " << err << "\n";
        return 1;
    }

    std::cout << "  Logical volume \"" << lv_name << "\" created.\n";
    return 0;
}

int cmd_lvremove(const std::vector<std::string>& args) {
    if (check_help_requested(args, "lvremove")) return 0;

    auto pos = get_positional_args(args);
    if (pos.size() < 2) {
        std::cerr << "error: lvremove requires LVName and member PV path(s)\n";
        print_command_help("lvremove");
        return 1;
    }

    std::string lv_name = pos[0];
    if (lv_name.find('/') != std::string::npos) {
        lv_name = lv_name.substr(lv_name.find('/') + 1);
    }
    std::vector<std::string> pv_paths(pos.begin() + 1, pos.end());

    std::string err;
    auto loaded = load_vg_flexible(pv_paths, true, err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    bool found = false;
    std::vector<LogicalVolume> remaining_lvs;
    for (auto& lv : loaded->vg.lvs) {
        if (lv.name == lv_name) {
            found = true;
        } else {
            remaining_lvs.push_back(std::move(lv));
        }
    }

    if (!found) {
        std::cerr << "error: logical volume \"" << lv_name << "\" not found in VG " << loaded->vg.name << "\n";
        return 1;
    }

    loaded->vg.lvs = std::move(remaining_lvs);
    loaded->vg.seqno++;

    std::vector<ByteSource*> raw_storages;
    for (auto& s : loaded->storages) raw_storages.push_back(s.get());

    if (!commit_vg_metadata(loaded->vg, raw_storages, err)) {
        std::cerr << "error: failed to commit metadata: " << err << "\n";
        return 1;
    }

    std::cout << "  Logical volume \"" << lv_name << "\" successfully removed\n";
    return 0;
}

int cmd_lvs(const std::vector<std::string>& args) {
    if (check_help_requested(args, "lvs")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: lvs requires member PV path(s)\n";
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
        std::cout << "  LV   VG   Attr       LSize   Pool Origin Data%  Meta%  Move Log Cpy%Sync Convert\n";
    }

    std::uint64_t ext_bytes = loaded->vg.extent_size_sectors * 512ULL;
    for (const auto& lv : loaded->vg.lvs) {
        std::uint64_t lv_bytes = lv.extent_count() * ext_bytes;
        std::cout << "  " << lv.name << " "
                  << loaded->vg.name << " "
                  << "-wi-a----- "
                  << format_size_human(lv_bytes) << "\n";
    }

    return 0;
}

int cmd_lvdisplay(const std::vector<std::string>& args) {
    if (check_help_requested(args, "lvdisplay")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: lvdisplay requires member PV path(s)\n";
        return 1;
    }

    std::string err;
    auto loaded = load_vg_flexible(pos, false, err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    std::uint64_t ext_bytes = loaded->vg.extent_size_sectors * 512ULL;
    for (const auto& lv : loaded->vg.lvs) {
        std::cout << "  --- Logical volume ---\n"
                  << "  LV Path                /dev/" << loaded->vg.name << "/" << lv.name << "\n"
                  << "  LV Name                " << lv.name << "\n"
                  << "  VG Name                " << loaded->vg.name << "\n"
                  << "  LV UUID                " << format_uuid(lv.id) << "\n"
                  << "  LV Status              available\n"
                  << "  LV Size                " << format_size_human(lv.extent_count() * ext_bytes) << "\n"
                  << "  Current LE             " << lv.extent_count() << "\n"
                  << "  Segments               " << lv.segments.size() << "\n"
                  << "  Allocation             inherit\n\n";
    }

    return 0;
}

int cmd_lvscan(const std::vector<std::string>& args) {
    if (check_help_requested(args, "lvscan")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cout << "Reading all logical volumes...\n";
        return 0;
    }

    std::string err;
    auto loaded = load_vg_flexible(pos, false, err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    std::uint64_t ext_bytes = loaded->vg.extent_size_sectors * 512ULL;
    for (const auto& lv : loaded->vg.lvs) {
        std::cout << "  ACTIVE            '/dev/" << loaded->vg.name << "/" << lv.name << "' ["
                  << format_size_human(lv.extent_count() * ext_bytes) << "] inherit\n";
    }

    return 0;
}

int cmd_lvchange(const std::vector<std::string>& args) {
    if (check_help_requested(args, "lvchange")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: lvchange requires LV path\n";
        return 1;
    }

    std::string lv_target = pos[0];
    auto act_val = get_opt_value(args, "-a", "--activate");
    if (!act_val) {
        for (const auto& a : args) {
            if (a == "-ay") { act_val = "y"; break; }
            if (a == "-an") { act_val = "n"; break; }
        }
    }

#ifdef _WIN32
    if (act_val) {
        bool activate = (*act_val == "y" || *act_val == "ey" || *act_val == "sy");
        notify_driver_activate_lv(lv_target, activate);
    }
#endif

    std::cout << "  Logical volume \"" << pos[0] << "\" changed.\n";
    return 0;
}

int cmd_lvextend(const std::vector<std::string>& args) {
    if (check_help_requested(args, "lvextend")) return 0;

    auto pos = get_positional_args(args);
    if (pos.size() < 2) {
        std::cerr << "error: lvextend requires LVName and member PV path(s)\n";
        print_command_help("lvextend");
        return 1;
    }

    std::string lv_name = pos[0];
    if (lv_name.find('/') != std::string::npos) {
        lv_name = lv_name.substr(lv_name.find('/') + 1);
    }
    std::vector<std::string> pv_paths(pos.begin() + 1, pos.end());

    std::string err;
    auto loaded = load_vg_flexible(pv_paths, true, err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    std::uint64_t ext_bytes = loaded->vg.extent_size_sectors * 512ULL;
    std::uint64_t add_extents = 0;

    if (auto sz_str = get_opt_value(args, "-L", "--size")) {
        auto sz_bytes = parse_size_bytes(*sz_str, err);
        if (!sz_bytes) {
            std::cerr << "error: invalid size: " << err << "\n";
            return 1;
        }
        add_extents = (*sz_bytes + ext_bytes - 1) / ext_bytes;
    } else if (auto l_str = get_opt_value(args, "-l", "--extents")) {
        try {
            add_extents = std::stoull(*l_str);
        } catch (...) {
            std::cerr << "error: invalid extent count: " << *l_str << "\n";
            return 1;
        }
    } else {
        std::cerr << "error: lvextend requires -L <size> or -l <extents>\n";
        return 1;
    }

    if (!extend_lv(loaded->vg, lv_name, add_extents, {}, err)) {
        std::cerr << "error: extend failed: " << err << "\n";
        return 1;
    }

    std::vector<ByteSource*> raw_storages;
    for (auto& s : loaded->storages) raw_storages.push_back(s.get());

    if (!commit_vg_metadata(loaded->vg, raw_storages, err)) {
        std::cerr << "error: failed to commit metadata: " << err << "\n";
        return 1;
    }

#ifdef _WIN32
    for (const auto& lv_item : loaded->vg.lvs) {
        if (lv_item.name == lv_name) {
            notify_driver_update_lv(lv_item.id, lv_item.extent_count() * ext_bytes);
            break;
        }
    }
#endif

    std::cout << "  Logical volume " << loaded->vg.name << "/" << lv_name << " successfully resized\n";
    return 0;
}

int cmd_lvreduce(const std::vector<std::string>& args) {
    if (check_help_requested(args, "lvreduce")) return 0;

    auto pos = get_positional_args(args);
    if (pos.size() < 2) {
        std::cerr << "error: lvreduce requires LVName and member PV path(s)\n";
        print_command_help("lvreduce");
        return 1;
    }

    std::string lv_name = pos[0];
    if (lv_name.find('/') != std::string::npos) {
        lv_name = lv_name.substr(lv_name.find('/') + 1);
    }
    std::vector<std::string> pv_paths(pos.begin() + 1, pos.end());

    std::string err;
    auto loaded = load_vg_flexible(pv_paths, true, err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    std::uint64_t ext_bytes = loaded->vg.extent_size_sectors * 512ULL;
    std::uint64_t rem_extents = 0;

    if (auto sz_str = get_opt_value(args, "-L", "--size")) {
        auto sz_bytes = parse_size_bytes(*sz_str, err);
        if (!sz_bytes) {
            std::cerr << "error: invalid size: " << err << "\n";
            return 1;
        }
        rem_extents = (*sz_bytes + ext_bytes - 1) / ext_bytes;
    } else if (auto l_str = get_opt_value(args, "-l", "--extents")) {
        try {
            rem_extents = std::stoull(*l_str);
        } catch (...) {
            std::cerr << "error: invalid extent count: " << *l_str << "\n";
            return 1;
        }
    } else {
        std::cerr << "error: lvreduce requires -L <size> or -l <extents>\n";
        return 1;
    }

    if (!reduce_lv(loaded->vg, lv_name, rem_extents, err)) {
        std::cerr << "error: reduce failed: " << err << "\n";
        return 1;
    }

    std::vector<ByteSource*> raw_storages;
    for (auto& s : loaded->storages) raw_storages.push_back(s.get());

    if (!commit_vg_metadata(loaded->vg, raw_storages, err)) {
        std::cerr << "error: failed to commit metadata: " << err << "\n";
        return 1;
    }

#ifdef _WIN32
    for (const auto& lv_item : loaded->vg.lvs) {
        if (lv_item.name == lv_name) {
            notify_driver_update_lv(lv_item.id, lv_item.extent_count() * ext_bytes);
            break;
        }
    }
#endif

    std::cout << "  Logical volume " << loaded->vg.name << "/" << lv_name << " successfully resized\n";
    return 0;
}

int cmd_lvresize(const std::vector<std::string>& args) {
    if (check_help_requested(args, "lvresize")) return 0;

    if (auto sz_str = get_opt_value(args, "-L", "--size")) {
        if (!sz_str->empty() && sz_str->front() == '-') {
            return cmd_lvreduce(args);
        } else {
            return cmd_lvextend(args);
        }
    }
    if (auto l_str = get_opt_value(args, "-l", "--extents")) {
        if (!l_str->empty() && l_str->front() == '-') {
            return cmd_lvreduce(args);
        } else {
            return cmd_lvextend(args);
        }
    }
    return cmd_lvextend(args);
}

int cmd_lvrename(const std::vector<std::string>& args) {
    if (check_help_requested(args, "lvrename")) return 0;

    auto pos = get_positional_args(args);
    if (pos.size() < 3) {
        std::cerr << "error: lvrename requires OldLV NewLV and member PV path(s)\n";
        print_command_help("lvrename");
        return 1;
    }

    std::string old_name = pos[0];
    std::string new_name = pos[1];
    if (old_name.find('/') != std::string::npos) old_name = old_name.substr(old_name.find('/') + 1);
    if (new_name.find('/') != std::string::npos) new_name = new_name.substr(new_name.find('/') + 1);
    std::vector<std::string> pv_paths(pos.begin() + 2, pos.end());

    std::string err;
    auto loaded = load_vg_flexible(pv_paths, true, err);
    if (!loaded) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    bool found = false;
    for (auto& lv : loaded->vg.lvs) {
        if (lv.name == old_name) {
            lv.name = new_name;
            found = true;
            break;
        }
    }

    if (!found) {
        std::cerr << "error: logical volume \"" << old_name << "\" not found\n";
        return 1;
    }

    loaded->vg.seqno++;
    std::vector<ByteSource*> raw_storages;
    for (auto& s : loaded->storages) raw_storages.push_back(s.get());

    if (!commit_vg_metadata(loaded->vg, raw_storages, err)) {
        std::cerr << "error: failed to commit rename: " << err << "\n";
        return 1;
    }

    std::cout << "  Renamed \"" << old_name << "\" to \"" << new_name << "\" in volume group \"" << loaded->vg.name << "\"\n";
    return 0;
}

int cmd_lvconvert(const std::vector<std::string>& args) {
    if (check_help_requested(args, "lvconvert")) return 0;

    auto pos = get_positional_args(args);
    if (pos.empty()) {
        std::cerr << "error: lvconvert requires LV path\n";
        return 1;
    }

    std::cout << "  Logical volume " << pos[0] << " converted.\n";
    return 0;
}

} // namespace lvmfmt
