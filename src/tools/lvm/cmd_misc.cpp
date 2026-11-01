#include "subcommands.h"
#include "cmd_common.h"

#include <iomanip>
#include <iostream>

#if defined(_WIN32)
#include "win32_driver.h"
#else
#include <unistd.h>
#endif

namespace lvmfmt {

int cmd_dumpconfig(const std::vector<std::string>& args) {
  if (check_help_requested(args, "dumpconfig")) return 0;
  std::cerr << "error: dumpconfig is not supported by this version of WinLVM\n";
  return 1;
}

int cmd_formats(const std::vector<std::string>& args) {
  if (check_help_requested(args, "formats")) return 0;
  std::cout << "  lvm2  LVM2 format (circular ring MDA)\n";
  return 0;
}

int cmd_segtypes(const std::vector<std::string>& args) {
  if (check_help_requested(args, "segtypes")) return 0;
  std::cout << "  linear   Linear mapping segment type\n"
            << "  striped  Striped mapping segment type\n";
  return 0;
}

int cmd_tags(const std::vector<std::string>& args) {
  if (check_help_requested(args, "tags")) return 0;
  std::cerr << "error: tags is not supported by this version of WinLVM\n";
  return 1;
}

int cmd_systemid(const std::vector<std::string>& args) {
  if (check_help_requested(args, "systemid")) return 0;
  std::string host;
#if defined(_WIN32)
  char buf[256]{};
  DWORD sz = sizeof(buf);
  if (GetComputerNameA(buf, &sz)) host.assign(buf, sz);
#else
  char buf[256]{};
  if (gethostname(buf, sizeof(buf) - 1) == 0) host.assign(buf);
#endif
  std::cout << "  system ID: " << host << "\n";
  return 0;
}

int cmd_lvmdiskscan(const std::vector<std::string>& args) {
  if (check_help_requested(args, "lvmdiskscan")) return 0;

  auto pos = get_positional_args(args);
#if defined(_WIN32)
  if (pos.empty()) {
    for (const auto& c : discover_win32_candidate_devices()) {
      pos.push_back(c.open_path);
    }
  }
#else
  if (pos.empty()) {
    std::cerr << "error: lvmdiskscan requires device or image path(s) in offline mode\n";
    return 1;
  }
#endif

  std::uint64_t disks = 0;
  std::uint64_t lvm_pvs = 0;
  for (const auto& dev : pos) {
    std::string err;
    auto storage = open_storage_source(dev, false, err);
    if (!storage) continue;
    disks++;

    auto label = read_pv_label(*storage, err);
    if (label) {
      lvm_pvs++;
      std::cout << "  " << dev << " [ " << format_size_human(label->device_size)
                << "] LVM physical volume\n";
    } else {
      std::cout << "  " << dev << " [ " << format_size_human(storage->size()) << "] disk\n";
    }
  }
  std::cout << "  " << disks << " disks\n"
            << "  " << lvm_pvs << " LVM physical volumes\n";
  return 0;
}

int cmd_lvmdump(const std::vector<std::string>& args) {
  if (check_help_requested(args, "lvmdump")) return 0;
  std::cerr << "error: lvmdump archive subcommand is not supported; use the standalone lvmdump tool\n";
  return 1;
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
  HANDLE h = open_winlvm_control();
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

}  // namespace lvmfmt
