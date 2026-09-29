#include <iostream>
#include <string>
#include <vector>

#include "lvmfmt/label.h"
#include "lvmfmt/metadata.h"
#include "lvmfmt/mapping.h"
#include "lvmfmt/source.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include "../../winlvm_ioctl.h"
#endif

namespace {
struct Image {
  lvmfmt::PvLabel label;
  std::string metadata;
  lvmfmt::VolumeGroup vg;
};

int usage() {
  std::cerr << "usage: lvm.exe --image PATH {pvs|vgs|lvs|dump}\n"
            << "       lvm.exe {version|pvs|vgs|lvs|vgscan|pvscan}\n";
  return 2;
}

bool load_image(const std::string& path, Image& image, std::string& error) {
  auto source = lvmfmt::open_file_source(path, error);
  if (!source) return false;
  auto label = lvmfmt::read_pv_label(*source, error);
  if (!label) return false;
  auto metadata = lvmfmt::read_vg_metadata(*source, *label, error);
  if (!metadata) return false;
  auto vg = lvmfmt::parse_vg_metadata(*metadata, error);
  if (!vg) return false;
  image.label = std::move(*label);
  image.metadata = std::move(*metadata);
  image.vg = std::move(*vg);
  return true;
}

int print_pvs(const Image& image) {
  std::cout << "PV UUID: " << lvmfmt::format_uuid(image.label.pv_uuid) << "\n"
            << "PV size: " << image.label.device_size << " bytes\n"
            << "VG: " << image.vg.name << "\n"
            << "PVs: " << image.vg.pvs.size() << "\n";
  for (const auto& pv : image.vg.pvs) {
    std::cout << "  " << pv.key << " " << lvmfmt::format_uuid(pv.id)
              << " pe_start=" << pv.pe_start_sectors << " pe_count=" << pv.pe_count << "\n";
  }
  return 0;
}

int print_vgs(const Image& image) {
  std::cout << "VG: " << image.vg.name << "\n"
            << "UUID: " << lvmfmt::format_uuid(image.vg.id) << "\n"
            << "seqno: " << image.vg.seqno << "\n"
            << "extent_size: " << image.vg.extent_size_sectors << " sectors\n"
            << "PVs: " << image.vg.pvs.size() << "\n"
            << "LVs: " << image.vg.lvs.size() << "\n";
  return 0;
}

int print_lvs(const Image& image) {
  std::cout << "VG: " << image.vg.name << "\n";
  for (const auto& lv : image.vg.lvs) {
    std::cout << lv.name << " extents=" << lv.extent_count()
              << " segments=" << lv.segments.size()
              << " status=" << (lv.supported ? "supported" : "unsupported") << "\n";
  }
  return 0;
}

int print_dump(const Image& image) {
  std::cout << "PV UUID: " << lvmfmt::format_uuid(image.label.pv_uuid) << "\n"
            << "label offset: " << image.label.label_offset << "\n"
            << "VG: " << image.vg.name << " seqno=" << image.vg.seqno << "\n"
            << image.metadata;
  return 0;
}

#ifdef _WIN32
HANDLE open_control_device() {
  return CreateFileW(WINLVM_USER_DEVICE_NAME,
                     GENERIC_READ | GENERIC_WRITE,
                     0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

int online_version() {
  std::cout << "WinLVM CLI version 1.0 (Windows LVM2 Subsystem)\n";
  HANDLE h = open_control_device();
  if (h != INVALID_HANDLE_VALUE) {
    std::cout << "Driver status: online (\\Device\\WinLvmControl connected)\n";
    CloseHandle(h);
  } else {
    std::cout << "Driver status: offline / service stopped (error " << GetLastError() << ")\n";
  }
  return 0;
}

int online_vgscan() {
  HANDLE h = open_control_device();
  if (h == INVALID_HANDLE_VALUE) {
    std::cerr << "error: WinLVM driver control device unavailable (error " << GetLastError() << ")\n";
    return 5;
  }
  std::cout << "Re-probing physical volume disk interfaces...\n";
  WINLVM_CONTROL_REQUEST req{1, sizeof(WINLVM_CONTROL_REQUEST)};
  DWORD ret = 0;
  if (!DeviceIoControl(h, IOCTL_WINLVM_REFRESH, &req, sizeof(req), nullptr, 0, &ret, nullptr)) {
    std::cerr << "error: failed to trigger driver refresh (error " << GetLastError() << ")\n";
    CloseHandle(h);
    return 5;
  }
  std::cout << "Disk interface re-probe completed successfully.\n";
  CloseHandle(h);
  return 0;
}

int online_inventory() {
  HANDLE h = open_control_device();
  if (h == INVALID_HANDLE_VALUE) {
    std::cerr << "error: WinLVM driver control device unavailable (error " << GetLastError() << ")\n";
    return 5;
  }
  WINLVM_CONTROL_REQUEST req{1, sizeof(WINLVM_CONTROL_REQUEST)};
  WINLVM_INVENTORY inv{};
  DWORD ret = 0;
  if (!DeviceIoControl(h, IOCTL_WINLVM_INVENTORY, &req, sizeof(req), &inv, sizeof(inv), &ret, nullptr)) {
    std::cerr << "error: failed to query inventory from driver (error " << GetLastError() << ")\n";
    CloseHandle(h);
    return 5;
  }
  std::cout << "WinLVM Inventory:\n"
            << "  Version: " << inv.version << "\n"
            << "  Active records: " << inv.record_count << "\n";
  CloseHandle(h);
  return 0;
}
#endif
}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "--image") {
    if (argc != 4) return usage();
    const std::string path = argv[2];
    const std::string command = argv[3];
    if (command != "pvs" && command != "vgs" && command != "lvs" && command != "dump") return usage();

    Image image;
    std::string error;
    if (!load_image(path, image, error)) {
      std::cerr << "error: " << error << "\n";
      return 1;
    }
    if (command == "pvs") return print_pvs(image);
    if (command == "vgs") return print_vgs(image);
    if (command == "lvs") return print_lvs(image);
    return print_dump(image);
  }

  if (argc < 2) return usage();
  const std::string command = argv[1];

  if (command == "version") {
#ifdef _WIN32
    return online_version();
#else
    std::cout << "WinLVM CLI version 1.0 (Windows LVM2 Subsystem)\n";
    return 0;
#endif
  }

#ifdef _WIN32
  if (command == "pvs" || command == "vgs" || command == "lvs") return online_inventory();
  if (command == "vgscan" || command == "pvscan" || command == "refresh") return online_vgscan();
#else
  if (command == "pvs" || command == "vgs" || command == "lvs" || command == "dump") {
    std::cerr << "error: --image is required; online device mode is unsupported\n";
    return 2;
  }
#endif

  return usage();
}
