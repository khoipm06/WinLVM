#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "winlvm/activation.h"
#include "winlvm/device_enum.h"
#include "winlvm/driver_client.h"
#include "winlvm/exporter.h"
#include "winlvm/hash.h"
#include "winlvm/json.h"
#include "winlvm/scanner.h"

namespace {

struct CliOptions {
  std::string command;
  std::vector<std::string> images;
  std::vector<std::string> devices;
  std::string export_target;
  std::string output_path;
  std::uint64_t export_offset = 0;
  std::uint64_t export_length = 0;
  bool json = false;
  bool help = false;
  bool readonly = false;
  bool has_export_length = false;
  std::string expected_hash;
  bool activate = false;  // lvchange -ay flag
  bool deactivate = false;  // lvchange -an flag
  bool force = false;  // Force flag
  std::string lv_target;  // lvchange target: vg/lv
  std::uint64_t driver_mapping_id = 0;
  bool has_driver_mapping_id = false;
};

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string exe_command(const char* argv0) {
  std::filesystem::path path(argv0);
  return lower(path.stem().string());
}

bool is_mutating_command(const std::string& command) {
  static const std::vector<std::string> commands = {
      "vgchange", "lvcreate", "lvremove", "lvresize", "vgextend", "vgreduce", "pvcreate", "pvremove"};
  return std::find(commands.begin(), commands.end(), command) != commands.end();
}

bool is_known_command(const std::string& command) {
  static const std::vector<std::string> commands = {
      "pvs",       "vgs",       "lvs",       "pvscan",  "vgscan", "lvscan", "pvdisplay",
      "vgdisplay", "lvdisplay", "lvchange",  "vgchange", "lvcreate", "lvremove", "lvresize",
      "vgextend", "vgreduce", "pvcreate", "pvremove", "version", "dump", "export", "hash", "verify",
      "driver-status", "driver-delete"};
  return std::find(commands.begin(), commands.end(), command) != commands.end();
}

std::uint64_t parse_u64(const std::string& text, const std::string& option_name) {
  std::size_t consumed = 0;
  std::uint64_t value = 0;
  try {
    value = std::stoull(text, &consumed, 0);
  } catch (const std::exception&) {
    throw std::runtime_error(option_name + " requires an unsigned integer byte value");
  }
  if (consumed != text.size()) {
    throw std::runtime_error(option_name + " requires an unsigned integer byte value");
  }
  return value;
}

CliOptions parse_args(int argc, char** argv) {
  CliOptions options;
  options.command = exe_command(argv[0]);
  int index = 1;
  if (options.command == "lvm") {
    if (argc > 1 && is_known_command(lower(argv[1]))) {
      options.command = lower(argv[1]);
      index = 2;
    } else {
      options.command = "version";
    }
  }

  for (int i = index; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      options.help = true;
    } else if (arg == "--image") {
      if (i + 1 >= argc) {
        throw std::runtime_error("--image requires a path");
      }
      options.images.push_back(argv[++i]);
    } else if (arg == "--device") {
      if (i + 1 >= argc) {
        throw std::runtime_error("--device requires a native Windows device path");
      }
      options.devices.push_back(argv[++i]);
    } else if (arg == "--reportformat") {
      if (i + 1 >= argc) {
        throw std::runtime_error("--reportformat requires a value");
      }
      const std::string format = lower(argv[++i]);
      if (format == "json") {
        options.json = true;
      } else {
        throw std::runtime_error("only --reportformat json is supported");
      }
    } else if (arg == "--json") {
      options.json = true;
    } else if (arg == "--output") {
      if (options.command != "export") {
        throw std::runtime_error("--output is only supported with lvm export");
      }
      if (i + 1 >= argc) {
        throw std::runtime_error("--output requires a path");
      }
      options.output_path = argv[++i];
    } else if (arg == "--readonly") {
      if (options.command != "export" && options.command != "hash" && options.command != "verify" && options.command != "lvchange") {
        throw std::runtime_error("--readonly is only supported with lvm export/hash/verify/lvchange");
      }
      options.readonly = true;
    } else if (arg == "-ay" || arg == "-a" || arg == "--activate") {
      if (options.command != "lvchange" && options.command != "vgchange") {
        throw std::runtime_error("-ay/--activate is only supported with lvchange/vgchange");
      }
      options.activate = true;
    } else if (arg == "-an" || arg == "--deactivate") {
      if (options.command != "lvchange" && options.command != "vgchange") {
        throw std::runtime_error("-an/--deactivate is only supported with lvchange/vgchange");
      }
      options.deactivate = true;
    } else if (arg == "-f" || arg == "--force") {
      options.force = true;
    } else if (arg == "--offset") {
      if (options.command != "export" && options.command != "hash" && options.command != "verify") {
        throw std::runtime_error("--offset is only supported with lvm export/hash/verify");
      }
      if (i + 1 >= argc) {
        throw std::runtime_error("--offset requires a byte value");
      }
      options.export_offset = parse_u64(argv[++i], "--offset");
    } else if (arg == "--length") {
      if (options.command != "export" && options.command != "hash" && options.command != "verify") {
        throw std::runtime_error("--length is only supported with lvm export/hash/verify");
      }
      if (i + 1 >= argc) {
        throw std::runtime_error("--length requires a byte value");
      }
      options.export_length = parse_u64(argv[++i], "--length");
      options.has_export_length = true;
    } else if (arg == "--expected-hash") {
      if (options.command != "verify") {
        throw std::runtime_error("--expected-hash is only supported with lvm verify");
      }
      if (i + 1 >= argc) {
        throw std::runtime_error("--expected-hash requires a hex string");
      }
      options.expected_hash = lower(argv[++i]);
    } else {
      if ((options.command == "export" || options.command == "hash" || options.command == "verify") && options.export_target.empty() &&
          arg.rfind("-", 0) != 0) {
        options.export_target = arg;
      } else if ((options.command == "lvchange" || is_mutating_command(options.command)) && options.lv_target.empty() &&
                 arg.rfind("-", 0) != 0) {
        options.lv_target = arg;
      } else if (options.command == "driver-delete" && !options.has_driver_mapping_id && arg.rfind("-", 0) != 0) {
        options.driver_mapping_id = parse_u64(arg, "driver-delete mapping id");
        options.has_driver_mapping_id = true;
      } else {
        throw std::runtime_error("unsupported argument: " + arg);
      }
    }
  }
  return options;
}

void print_help(const std::string& command) {
  if (command == "export") {
    std::cout << "Usage: lvm export <vg/lv> (--image <path> | --device <path>) --output <path> --readonly\n";
    std::cout << "                  [--offset <bytes>] [--length <bytes>]\n";
    std::cout << "       export reads supplied sources only and refuses to overwrite output files.\n";
    return;
  }
  if (command == "hash") {
    std::cout << "Usage: lvm hash <vg/lv> (--image <path> | --device <path>) --offset <bytes> --length <bytes>\n";
    std::cout << "       hash reads supplied sources only and prints a SHA-256 digest for the bounded LV range.\n";
    return;
  }
  if (command == "verify") {
    std::cout << "Usage: lvm verify <vg/lv> (--image <path> | --device <path>) --offset <bytes> --length <bytes>\n";
    std::cout << "                  --expected-hash <hex>\n";
    std::cout << "       verify computes SHA-256 over the bounded LV range and compares against --expected-hash.\n";
    std::cout << "       Prints PASS or FAIL with the actual and expected hashes.\n";
    return;
  }
  if (command == "dump") {
    std::cout << "Usage: lvm dump (--image <path> | --device <path>) --json\n";
    return;
  }
  if (command == "lvchange") {
    std::cout << "Usage: lvm lvchange -ay [--readonly] <vg/lv>\n";
    std::cout << "       lvm lvchange -an [-f|--force] <vg/lv>\n";
    std::cout << "       -ay: Activate logical volume (writable unless --readonly)\n";
    std::cout << "       -an: Deactivate logical volume\n";
    std::cout << "       Note: Activation service (lvmd) must be running.\n";
    return;
  }
  if (command == "driver-status") {
    std::cout << "Usage: lvm driver-status\n";
    std::cout << "       lists active lvmdm control-device mappings and queries each mapping.\n";
    return;
  }
  if (command == "driver-delete") {
    std::cout << "Usage: lvm driver-delete <mapping-id>\n";
    std::cout << "       diagnostic cleanup for loaded-driver CREATE/LIST/QUERY/DELETE smoke tests.\n";
    return;
  }
  std::cout << "Usage: " << command << " (--image <path> | --device <path>) [--reportformat json]\n";
  std::cout << "       lvm export <vg/lv> (--image <path> | --device <path>) --output <path> --readonly\n";
  std::cout << "                  [--offset <bytes>] [--length <bytes>]\n";
  std::cout << "       lvm hash <vg/lv> (--image <path> | --device <path>) --offset <bytes> --length <bytes>\n";
  std::cout << "       lvm verify <vg/lv> (--image <path> | --device <path>) --offset <bytes> --length <bytes>\n";
  std::cout << "                  --expected-hash <hex>\n";
  std::cout << "       lvm lvchange -ay [--readonly] <vg/lv>\n";
  std::cout << "       lvm lvchange -an [-f|--force] <vg/lv>\n";
  std::cout << "Sources are opened read-only in prototype. Activation requires running service.\n";
}

int print_version() {
  std::cout << "  LVM version:     winlvm 0.1.0 metadata/image prototype\n";
  std::cout << "  Library version: LVM2 user-mode source vendored as semantic baseline\n";
  std::cout << "  Driver version:  not implemented\n";
  return 0;
}

bool has_error(const winlvm::ScanResult& result) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const winlvm::Diagnostic& diagnostic) {
    return diagnostic.severity == "error";
  });
}

int run_scan(const CliOptions& options, winlvm::ScanResult& result) {
  winlvm::ScanOptions scan_options;
  scan_options.image_paths = options.images;
  scan_options.device_paths = options.devices;

  // Auto-enumerate Windows devices if no sources are specified
  if (scan_options.image_paths.empty() && scan_options.device_paths.empty()) {
#ifdef _WIN32
    auto disks = winlvm::enumerate_physical_disks();
    if (disks) {
      for (const auto& disk : disks.value) {
        const auto skip_reasons = winlvm::implicit_scan_skip_reasons(disk);
        if (!skip_reasons.empty()) {
          std::cerr << "warning: skipping PhysicalDrive" << disk.disk_number << " during implicit scan: ";
          for (std::size_t reason_index = 0; reason_index < skip_reasons.size(); ++reason_index) {
            if (reason_index != 0) {
              std::cerr << ", ";
            }
            std::cerr << skip_reasons[reason_index];
          }
          std::cerr << " (use --device explicitly for bounded read-only validation)\n";
          continue;
        }
        scan_options.device_paths.push_back(
          "\\\\.\\PhysicalDrive" + std::to_string(disk.disk_number));
      }
    } else {
      std::cerr << "warning: cannot enumerate physical disks for implicit scan: " << disks.status.message << '\n';
    }
#endif
  }

  auto scanned = winlvm::scan_images(scan_options);
  if (!scanned) {
    std::cerr << scanned.status.message << '\n';
    return 2;
  }
  result = std::move(scanned.value);
  return has_error(result) ? 3 : 0;
}

void print_pvs(const winlvm::ScanResult& result) {
  std::cout << "  PV UUID                          PV Base    PSize\n";
  for (const winlvm::PhysicalVolume& pv : result.pvs) {
    std::cout << "  " << pv.uuid << "  " << pv.pv_base_offset_bytes << "  " << pv.device_size_bytes << '\n';
  }
}

void print_vgs(const winlvm::ScanResult& result) {
  std::cout << "  VG   VG UUID                          Seqno  ExtSize\n";
  for (const winlvm::VolumeGroup& vg : result.vgs) {
    std::cout << "  " << vg.name << "  " << vg.uuid << "  " << vg.seqno << "  " << vg.extent_size_bytes << '\n';
  }
}

void print_lvs(const winlvm::ScanResult& result) {
  std::cout << "  LV    VG   Attr       LSize   Segment\n";
  for (const winlvm::VolumeGroup& vg : result.vgs) {
    for (const winlvm::LogicalVolume& lv : vg.lvs) {
      const std::string attr = lv.supported ? "-wi-------" : "unsupported";
      const std::string segment = lv.segments.empty() ? "" : lv.segments.front().type;
      std::cout << "  " << lv.name << "  " << vg.name << "  " << attr << "  " << lv.size_bytes << "  " << segment
                << '\n';
    }
  }
}

void print_scan_messages(const winlvm::ScanResult& result, const std::string& command) {
  if (command == "pvscan") {
    for (const winlvm::PhysicalVolume& pv : result.pvs) {
      std::cout << "  PV " << pv.uuid << " found at image offset " << pv.pv_base_offset_bytes << '\n';
    }
  } else if (command == "vgscan") {
    for (const winlvm::VolumeGroup& vg : result.vgs) {
      std::cout << "  Found volume group \"" << vg.name << "\" using metadata type lvm2\n";
    }
  } else if (command == "lvscan") {
    for (const winlvm::VolumeGroup& vg : result.vgs) {
      for (const winlvm::LogicalVolume& lv : vg.lvs) {
        std::cout << "  inactive          '/dev/" << vg.name << "/" << lv.name << "' [" << lv.size_bytes << " bytes]\n";
      }
    }
  }
}

void print_display(const winlvm::ScanResult& result, const std::string& command) {
  if (command == "pvdisplay") {
    for (const winlvm::PhysicalVolume& pv : result.pvs) {
      std::cout << "  --- Physical volume ---\n";
      std::cout << "  PV UUID               " << pv.uuid << '\n';
      std::cout << "  PV Base Offset        " << pv.pv_base_offset_bytes << '\n';
      std::cout << "  PV Size               " << pv.device_size_bytes << '\n';
    }
  } else if (command == "vgdisplay") {
    for (const winlvm::VolumeGroup& vg : result.vgs) {
      std::cout << "  --- Volume group ---\n";
      std::cout << "  VG Name               " << vg.name << '\n';
      std::cout << "  VG UUID               " << vg.uuid << '\n';
      std::cout << "  VG Seqno              " << vg.seqno << '\n';
    }
  } else if (command == "lvdisplay") {
    for (const winlvm::VolumeGroup& vg : result.vgs) {
      for (const winlvm::LogicalVolume& lv : vg.lvs) {
        std::cout << "  --- Logical volume ---\n";
        std::cout << "  LV Path               /dev/" << vg.name << "/" << lv.name << '\n';
        std::cout << "  LV UUID               " << lv.uuid << '\n';
        std::cout << "  LV Size               " << lv.size_bytes << '\n';
      }
    }
  }
}

void print_diagnostics(const winlvm::ScanResult& result) {
  for (const winlvm::Diagnostic& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.severity << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

int unsupported_mutating_command(const std::string& command) {
  std::cerr << command << ": activation and metadata mutation are not supported in the image-mode prototype\n";
  return 4;
}

int run_export(const CliOptions& options) {
  if (!options.readonly) {
    std::cerr << "export requires --readonly in the image-mode prototype\n";
    return 2;
  }
  winlvm::ExportOptions export_options;
  export_options.scan_options.image_paths = options.images;
  export_options.scan_options.device_paths = options.devices;
  export_options.target = options.export_target;
  export_options.output_path = options.output_path;
  export_options.offset_bytes = options.export_offset;
  if (options.has_export_length) {
    export_options.length_bytes = options.export_length;
  }
  export_options.readonly = options.readonly;
  auto exported = winlvm::export_logical_volume(export_options);
  if (!exported) {
    std::cerr << exported.status.message << '\n';
    return 3;
  }
  std::cout << "  Exported " << exported.value << " bytes to " << options.output_path << '\n';
  return 0;
}

int run_hash(const CliOptions& options) {
  if (!options.has_export_length) {
    std::cerr << "hash requires --length so validation reads stay explicitly bounded\n";
    return 2;
  }
  winlvm::ExportOptions hash_options;
  hash_options.scan_options.image_paths = options.images;
  hash_options.scan_options.device_paths = options.devices;
  hash_options.target = options.export_target;
  hash_options.offset_bytes = options.export_offset;
  hash_options.length_bytes = options.export_length;
  hash_options.readonly = true;
  auto hashed = winlvm::hash_logical_volume(hash_options);
  if (!hashed) {
    std::cerr << hashed.status.message << '\n';
    return 3;
  }
  std::cout << "  " << hashed.value.algorithm << " " << hashed.value.hex << "  " << options.export_target
            << " offset=" << options.export_offset << " length=" << hashed.value.bytes_hashed << '\n';
  return 0;
}

int run_verify(const CliOptions& options) {
  if (!options.has_export_length) {
    std::cerr << "verify requires --length so validation reads stay explicitly bounded\n";
    return 2;
  }
  if (options.expected_hash.empty()) {
    std::cerr << "verify requires --expected-hash <hex>\n";
    return 2;
  }
  winlvm::ExportOptions verify_options;
  verify_options.scan_options.image_paths = options.images;
  verify_options.scan_options.device_paths = options.devices;
  verify_options.target = options.export_target;
  verify_options.offset_bytes = options.export_offset;
  verify_options.length_bytes = options.export_length;
  verify_options.readonly = true;
  auto hashed = winlvm::hash_logical_volume(verify_options);
  if (!hashed) {
    std::cerr << hashed.status.message << '\n';
    return 3;
  }
  const bool match = hashed.value.hex == options.expected_hash;
  std::cout << (match ? "PASS" : "FAIL") << "  " << options.export_target
            << " offset=" << options.export_offset << " length=" << hashed.value.bytes_hashed << '\n';
  std::cout << "  actual:   " << hashed.value.hex << '\n';
  std::cout << "  expected: " << options.expected_hash << '\n';
  return match ? 0 : 1;
}

int run_lvchange(const CliOptions& options) {
  // Parse vg/lv target
  const std::string& target = options.lv_target;
  if (target.empty()) {
    std::cerr << "lvchange requires a logical volume name in format vg/lv or vg:lv\n";
    return 2;
  }

  // Split VG/LV
  std::string vg_name, lv_name;
  std::size_t sep_pos = target.find('/');
  if (sep_pos == std::string::npos) {
    sep_pos = target.find(':');
  }
  if (sep_pos == std::string::npos) {
    std::cerr << "Invalid logical volume format (use vg/lv)\n";
    return 2;
  }
  vg_name = target.substr(0, sep_pos);
  lv_name = target.substr(sep_pos + 1);

  // Determine action
  if (options.activate && options.deactivate) {
    std::cerr << "Cannot specify both -ay and -an\n";
    return 2;
  }

  if (!options.activate && !options.deactivate) {
    std::cerr << "lvchange requires -ay (activate) or -an (deactivate)\n";
    return 2;
  }

  auto manager = winlvm::create_activation_manager();

  if (options.activate) {
    // Scan to get topology
    winlvm::ScanResult result;
    const int scan_code = run_scan(options, result);
    if (scan_code != 0 && scan_code != 3) {
      return scan_code;
    }

    // Create activation request
    winlvm::ActivationRequest req{};
    req.vg_name = vg_name;
    req.lv_name = lv_name;
    req.readonly = options.readonly;
    req.force = options.force;

    auto activated = manager->activate_logical_volume(result, req);
    if (!activated) {
      std::cerr << activated.status.message << '\n';
      return 3;
    }
    std::cout << "  Activated " << vg_name << "/" << lv_name << " (activation_id="
              << activated.value.activation_id << ")\n";
    return 0;
  } else if (options.deactivate) {
    // Deactivation request
    winlvm::DeactivationRequest req{};
    req.vg_name = vg_name;
    req.lv_name = lv_name;
    req.force = options.force;

    auto deactivated = manager->deactivate_logical_volume(req);
    if (!deactivated) {
      std::cerr << deactivated.status.message << '\n';
      return 3;
    }
    std::cout << "  Deactivated " << vg_name << "/" << lv_name << "\n";
    return 0;
  }

  return 2;
}

int run_driver_status(const CliOptions& options) {
  (void)options;
#ifdef _WIN32
  auto client = winlvm::DriverClient::open_default();
  if (client) {
    std::cout << "Kernel driver (lvmdm.sys) is loaded and running.\n";

    auto mappings = client.value.list_mappings();
    if (!mappings) {
      std::cerr << "Failed to list driver mappings: " << mappings.status.message << '\n';
      return 2;
    }

    std::cout << "Active logical volume mappings: " << mappings.value.size() << "\n";
    for (std::uint64_t mapping_id : mappings.value) {
      auto mapping = client.value.query_mapping(mapping_id);
      if (!mapping) {
        std::cout << "  - Mapping ID: " << mapping_id
                  << " (query failed: " << mapping.status.message << ")\n";
        continue;
      }
      std::cout << "  - Mapping ID: " << mapping.value.mapping_id
                << " status=" << mapping.value.status
                << " io_count=" << mapping.value.io_count
                << " mapping_table_size=" << mapping.value.mapping_table_size << "\n";
    }
    return 0;
  }
#endif

  std::cout << "Kernel driver (lvmdm.sys) is NOT running or not loaded.\n\n";
  std::cout << "To build and install the driver on a WDK-enabled machine:\n";
  std::cout << "  1. Open WDK Developer Command Prompt\n";
  std::cout << "  2. Run: msbuild src\\driver\\lvmdm.vcxproj /p:Configuration=Debug /p:Platform=x64\n";
  std::cout << "  3. To install and start:\n";
  std::cout << "     sc create lvmdm binPath= C:\\path\\to\\lvmdm.sys type= kernel\n";
  std::cout << "     sc start lvmdm\n";
  return 1;
}

int run_driver_delete(const CliOptions& options) {
  if (!options.has_driver_mapping_id) {
    std::cerr << "driver-delete requires a mapping id\n";
    return 2;
  }
#ifdef _WIN32
  auto client = winlvm::DriverClient::open_default();
  if (!client) {
    std::cerr << "Kernel driver (lvmdm.sys) is NOT running or not loaded: "
              << client.status.message << '\n';
    return 1;
  }

  const auto deleted = client.value.delete_mapping(options.driver_mapping_id);
  if (!deleted.ok) {
    std::cerr << "Failed to delete mapping " << options.driver_mapping_id
              << ": " << deleted.message << '\n';
    return 3;
  }
  std::cout << "Deleted mapping " << options.driver_mapping_id << "\n";
  return 0;
#else
  std::cerr << "driver-delete is only available on Windows\n";
  return 1;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const CliOptions options = parse_args(argc, argv);
    if (options.help) {
      print_help(options.command);
      return 0;
    }
    if (options.command == "version") {
      return print_version();
    }
    if (options.command == "export") {
      return run_export(options);
    }
    if (options.command == "hash") {
      return run_hash(options);
    }
    if (options.command == "verify") {
      return run_verify(options);
    }
    if (options.command == "lvchange") {
      return run_lvchange(options);
    }
    if (options.command == "driver-status") {
      return run_driver_status(options);
    }
    if (options.command == "driver-delete") {
      return run_driver_delete(options);
    }
    if (is_mutating_command(options.command)) {
      return unsupported_mutating_command(options.command);
    }

    winlvm::ScanResult result;
    const int scan_code = run_scan(options, result);
    if (options.json || options.command == "dump") {
      std::cout << winlvm::to_json(result);
    } else if (options.command == "pvs") {
      print_pvs(result);
    } else if (options.command == "vgs") {
      print_vgs(result);
    } else if (options.command == "lvs") {
      print_lvs(result);
    } else if (options.command == "pvscan" || options.command == "vgscan" || options.command == "lvscan") {
      print_scan_messages(result, options.command);
    } else if (options.command == "pvdisplay" || options.command == "vgdisplay" || options.command == "lvdisplay") {
      print_display(result, options.command);
    } else {
      std::cerr << "unsupported command: " << options.command << '\n';
      return 4;
    }
    print_diagnostics(result);
    return scan_code;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
