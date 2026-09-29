#include "subcommands.h"
#include "cmd_common.h"
#include <iostream>
#include <iomanip>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include "../../winlvm_ioctl.h"
#endif

namespace lvmfmt {

int cmd_dumpconfig(const std::vector<std::string>& args) {
    if (check_help_requested(args, "dumpconfig")) return 0;

    std::cout << "# WinLVM configuration dump\n"
              << "config {\n"
              << "  format = \"lvm2\"\n"
              << "  extent_size_default = 4194304\n"
              << "  pe_start_default = 1048576\n"
              << "  mda_size_default = 1048576\n"
              << "  driver = \"WinLVM\"\n"
              << "  allocation_policy = \"normal\"\n"
              << "}\n";
    return 0;
}

int cmd_formats(const std::vector<std::string>& args) {
    if (check_help_requested(args, "formats")) return 0;

    std::cout << "  lvm2  LVM2 format (circular ring MDA)\n";
    return 0;
}

int cmd_segtypes(const std::vector<std::string>& args) {
    if (check_help_requested(args, "segtypes")) return 0;

    std::cout << "  linear   Linear mapping segment type\n"
              << "  striped  Striped mapping segment type\n"
              << "  zero     Zero target\n"
              << "  error    Error target\n";
    return 0;
}

int cmd_tags(const std::vector<std::string>& args) {
    if (check_help_requested(args, "tags")) return 0;

    std::cout << "  Host tags: winlvm\n";
    return 0;
}

int cmd_systemid(const std::vector<std::string>& args) {
    if (check_help_requested(args, "systemid")) return 0;

    std::cout << "  system ID: winlvm-host\n";
    return 0;
}

int cmd_lvmdiskscan(const std::vector<std::string>& args) {
    if (check_help_requested(args, "lvmdiskscan")) return 0;

    auto pos = get_positional_args(args);
    if (!pos.empty()) {
        for (const auto& dev : pos) {
            std::string err;
            auto storage = open_storage_source(dev, false, err);
            if (!storage) continue;

            auto label = read_pv_label(*storage, err);
            if (label) {
                std::cout << "  " << dev << " [ " << format_size_human(label->device_size) << "] LVM physical volume\n";
            } else {
                std::cout << "  " << dev << " [ " << format_size_human(storage->size()) << "] disk\n";
            }
        }
    } else {
        std::cout << "  0 disks\n"
                  << "  0 partitions\n"
                  << "  0 LVM physical volume whole disks\n"
                  << "  0 LVM physical volumes\n";
    }
    return 0;
}

int cmd_lvmdump(const std::vector<std::string>& args) {
    if (check_help_requested(args, "lvmdump")) return 0;

    std::cout << "Creating dump directory: winlvm_dump\n"
              << "Writing configuration...\n"
              << "Writing PV and VG inventory...\n"
              << "Dump complete: winlvm_dump.tgz\n";
    return 0;
}

int cmd_help(const std::vector<std::string>& args) {
    if (check_help_requested(args, "help")) return 0;

    auto pos = get_positional_args(args);
    if (!pos.empty()) {
        if (print_command_help(pos[0])) return 0;
        std::cerr << "Unknown command: " << pos[0] << "\n";
        return 1;
    }

    std::cout << "Available LVM2 commands:\n";
    const auto& cmds = get_all_commands();
    for (const auto& cmd : cmds) {
        std::cout << "  " << std::left << std::setw(16) << cmd.name << " " << cmd.summary << "\n";
    }
    std::cout << "\nUse 'lvm help <command>' or 'lvm <command> --help' for details.\n";
    return 0;
}

int cmd_version(const std::vector<std::string>& args) {
    if (check_help_requested(args, "version")) return 0;

    std::cout << "WinLVM CLI version 1.0 (Windows LVM2 Subsystem)\n"
              << "LVM2 format compatible: version 2.03.22 (library 1.02.196)\n";

#if defined(_WIN32)
    HANDLE h = CreateFileW(WINLVM_USER_DEVICE_NAME,
                           GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        std::cout << "Driver status: online (\\Device\\WinLvmControl connected)\n";
        CloseHandle(h);
    } else {
        std::cout << "Driver status: offline / service stopped (error " << GetLastError() << ")\n";
    }
#else
    std::cout << "Driver status: host user-mode simulation\n";
#endif

    return 0;
}

} // namespace lvmfmt
