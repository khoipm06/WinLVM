#include <iostream>
#include <string>
#include <vector>

#include "lvmfmt/label.h"
#include "lvmfmt/metadata.h"
#include "lvmfmt/mapping.h"
#include "lvmfmt/source.h"

namespace {
struct Image {
  lvmfmt::PvLabel label;
  std::string metadata;
  lvmfmt::VolumeGroup vg;
};

int usage() {
  std::cerr << "usage: lvm.exe --image PATH {pvs|vgs|lvs|dump}\n";
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
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || std::string(argv[1]) != "--image") {
    std::cerr << "error: --image is required; online device mode is unsupported\n";
    return 2;
  }
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
