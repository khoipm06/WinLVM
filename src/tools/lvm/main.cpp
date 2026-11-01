#include <iostream>
#include <string>
#include <vector>

#include "lvmfmt/label.h"
#include "lvmfmt/mapping.h"
#include "lvmfmt/metadata.h"
#include "lvmfmt/source.h"
#include "subcommands.h"

namespace {

struct Image {
  lvmfmt::PvLabel label;
  std::string metadata;
  lvmfmt::VolumeGroup vg;
};

int usage() {
  std::cerr << "usage: lvm.exe --image PATH {pvs|vgs|lvs|dump}\n"
            << "       lvm.exe {version|pvs|vgs|lvs|vgscan|pvscan}\n"
            << "       lvm.exe <command> [args...]\n"
            << "       lvm.exe help [command]\n";
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

  std::vector<std::string> sub_args;
  for (int i = 2; i < argc; ++i) {
    sub_args.emplace_back(argv[i]);
  }

  if (command == "--help" || command == "-h" || command == "help") {
    return lvmfmt::cmd_help(sub_args);
  }
  if (command == "version" || command == "--version") {
    return lvmfmt::cmd_version(sub_args);
  }

#ifndef _WIN32
  if (argc == 2) {
    if (command == "pvs" || command == "vgs" || command == "lvs" || command == "dump") {
      std::cerr << "error: --image is required; online device mode is unsupported\n";
      return 2;
    }
    if (command == "vgscan") {
      return usage();
    }
  }
#else
  if (command == "refresh") {
    return lvmfmt::cmd_vgscan(sub_args);
  }
#endif

  // PV subcommands
  if (command == "pvcreate") return lvmfmt::cmd_pvcreate(sub_args);
  if (command == "pvremove") return lvmfmt::cmd_pvremove(sub_args);
  if (command == "pvs") return lvmfmt::cmd_pvs(sub_args);
  if (command == "pvdisplay") return lvmfmt::cmd_pvdisplay(sub_args);
  if (command == "pvscan") return lvmfmt::cmd_pvscan(sub_args);
  if (command == "pvresize") return lvmfmt::cmd_pvresize(sub_args);
  if (command == "pvchange") return lvmfmt::cmd_pvchange(sub_args);
  if (command == "pvmove") return lvmfmt::cmd_pvmove(sub_args);
  if (command == "pvck") return lvmfmt::cmd_pvck(sub_args);

  // VG subcommands
  if (command == "vgcreate") return lvmfmt::cmd_vgcreate(sub_args);
  if (command == "vgremove") return lvmfmt::cmd_vgremove(sub_args);
  if (command == "vgs") return lvmfmt::cmd_vgs(sub_args);
  if (command == "vgdisplay") return lvmfmt::cmd_vgdisplay(sub_args);
  if (command == "vgscan") return lvmfmt::cmd_vgscan(sub_args);
  if (command == "vgchange") return lvmfmt::cmd_vgchange(sub_args);
  if (command == "vgextend") return lvmfmt::cmd_vgextend(sub_args);
  if (command == "vgreduce") return lvmfmt::cmd_vgreduce(sub_args);
  if (command == "vgrename") return lvmfmt::cmd_vgrename(sub_args);
  if (command == "vgcfgbackup") return lvmfmt::cmd_vgcfgbackup(sub_args);
  if (command == "vgcfgrestore") return lvmfmt::cmd_vgcfgrestore(sub_args);
  if (command == "vgck") return lvmfmt::cmd_vgck(sub_args);
  if (command == "vgexport") return lvmfmt::cmd_vgexport(sub_args);
  if (command == "vgimport") return lvmfmt::cmd_vgimport(sub_args);

  // LV subcommands
  if (command == "lvcreate") return lvmfmt::cmd_lvcreate(sub_args);
  if (command == "lvremove") return lvmfmt::cmd_lvremove(sub_args);
  if (command == "lvs") return lvmfmt::cmd_lvs(sub_args);
  if (command == "lvdisplay") return lvmfmt::cmd_lvdisplay(sub_args);
  if (command == "lvscan") return lvmfmt::cmd_lvscan(sub_args);
  if (command == "lvchange") return lvmfmt::cmd_lvchange(sub_args);
  if (command == "lvextend") return lvmfmt::cmd_lvextend(sub_args);
  if (command == "lvreduce") return lvmfmt::cmd_lvreduce(sub_args);
  if (command == "lvresize") return lvmfmt::cmd_lvresize(sub_args);
  if (command == "lvrename") return lvmfmt::cmd_lvrename(sub_args);
  if (command == "lvconvert") return lvmfmt::cmd_lvconvert(sub_args);

  // Misc subcommands
  if (command == "dumpconfig") return lvmfmt::cmd_dumpconfig(sub_args);
  if (command == "formats") return lvmfmt::cmd_formats(sub_args);
  if (command == "segtypes") return lvmfmt::cmd_segtypes(sub_args);
  if (command == "tags") return lvmfmt::cmd_tags(sub_args);
  if (command == "systemid") return lvmfmt::cmd_systemid(sub_args);
  if (command == "lvmdiskscan") return lvmfmt::cmd_lvmdiskscan(sub_args);
  if (command == "lvmdump") return lvmfmt::cmd_lvmdump(sub_args);

  return usage();
}
