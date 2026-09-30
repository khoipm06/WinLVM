#pragma once

#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <memory>
#include "cmd_table.h"
#include "lvmfmt/source.h"
#include "lvmfmt/label.h"
#include "lvmfmt/metadata.h"
#include "lvmfmt/commit.h"
#include "lvmfmt/allocator.h"

namespace lvmfmt {

inline bool print_command_help(std::string_view cmd_name) {
    const auto& cmds = get_all_commands();
    for (const auto& cmd : cmds) {
        if (cmd.name == cmd_name) {
            std::cout << "Usage:\n  " << cmd.syntax << "\n\n";
            std::cout << cmd.description << "\n\n";
            if (!cmd.options.empty()) {
                std::cout << "Options:\n";
                for (const auto& opt : cmd.options) {
                    std::cout << "  " << opt << "\n";
                }
                std::cout << "\n";
            }
            return true;
        }
    }
    return false;
}

inline bool has_flag(const std::vector<std::string>& args, const std::string& opt, const std::string& long_opt = "") {
    for (const auto& a : args) {
        if (a == opt || (!long_opt.empty() && a == long_opt)) return true;
        if (a == "--help" || a == "-h") return false; // Handled separately
    }
    return false;
}

inline bool check_help_requested(const std::vector<std::string>& args, std::string_view cmd_name) {
    for (const auto& a : args) {
        if (a == "--help" || a == "-h") {
            print_command_help(cmd_name);
            return true;
        }
    }
    return false;
}

inline std::optional<std::string> get_opt_value(const std::vector<std::string>& args,
                                                const std::string& opt,
                                                const std::string& long_opt = "") {
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == opt || (!long_opt.empty() && args[i] == long_opt)) {
            if (i + 1 < args.size()) return args[i + 1];
        }
        if (!long_opt.empty() && args[i].rfind(long_opt + "=", 0) == 0) {
            return args[i].substr(long_opt.size() + 1);
        }
    }
    return std::nullopt;
}

inline std::vector<std::string> get_positional_args(const std::vector<std::string>& args, bool f_takes_value = false) {
    std::vector<std::string> pos;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].rfind("-", 0) == 0) {
            // Check if it's an option that takes an argument
            if (args[i] == "-L" || args[i] == "-l" || args[i] == "-n" ||
                args[i] == "-s" || args[i] == "-u" ||
                (f_takes_value && args[i] == "-f" && i + 1 < args.size() && args[i+1][0] != '-') ||
                args[i] == "--file" ||
                args[i] == "--uuid" || args[i] == "--setphysicalvolumesize" ||
                args[i] == "--metadatasize" || args[i] == "--dataalignment" ||
                args[i] == "--separator" || args[i] == "-o" || args[i] == "--options" ||
                args[i] == "-i" || args[i] == "--stripes" ||
                args[i] == "-I" || args[i] == "--stripesize") {
                i++; // skip value
            }
            continue;
        }
        pos.push_back(args[i]);
    }
    return pos;
}

// Loads VolumeGroup and opened storage sources from a list of PV device paths.
struct LoadedVG {
    VolumeGroup vg;
    std::vector<std::unique_ptr<ByteSource>> storages;
    std::vector<std::string> pv_paths;
};

inline std::optional<LoadedVG> load_vg_from_pvs(const std::vector<std::string>& pv_paths,
                                                bool read_write,
                                                std::string& error) {
    if (pv_paths.empty()) {
        error = "no physical volume paths specified";
        return std::nullopt;
    }

    LoadedVG loaded;
    std::optional<VolumeGroup> vg_opt;

    for (const auto& path : pv_paths) {
        std::string err;
        auto src = open_storage_source(path, read_write, err);
        if (!src) {
            error = "cannot open " + path + ": " + err;
            return std::nullopt;
        }

        auto label = read_pv_label(*src, err);
        if (!label) {
            error = "cannot read PV label from " + path + ": " + err;
            return std::nullopt;
        }

        if (!vg_opt) {
            auto meta = read_vg_metadata(*src, *label, err);
            if (!meta) {
                error = "cannot read VG metadata from " + path + ": " + err;
                return std::nullopt;
            }
            vg_opt = parse_vg_metadata(*meta, err);
            if (!vg_opt) {
                error = "cannot parse VG metadata from " + path + ": " + err;
                return std::nullopt;
            }
        }

        loaded.storages.push_back(std::move(src));
        loaded.pv_paths.push_back(path);
    }

    if (!vg_opt) {
        error = "failed to find valid volume group metadata";
        return std::nullopt;
    }

    loaded.vg = std::move(*vg_opt);
    return loaded;
}

inline std::optional<LoadedVG> load_vg_flexible(const std::vector<std::string>& pos,
                                                bool read_write,
                                                std::string& error) {
    if (pos.empty()) {
        error = "no physical volume arguments provided";
        return std::nullopt;
    }
    // First, try loading with all arguments as PV paths
    std::string err1;
    auto loaded = load_vg_from_pvs(pos, read_write, err1);
    if (loaded) return loaded;

    // If that failed (e.g. pos[0] was a VGName or LVName, not a PV), try pos[1..end]
    if (pos.size() >= 2) {
        std::vector<std::string> pvs(pos.begin() + 1, pos.end());
        return load_vg_from_pvs(pvs, read_write, error);
    }
    error = err1;
    return std::nullopt;
}

} // namespace lvmfmt
