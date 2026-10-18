// lvmdump — read an LVM2 image or device and print what we make of it.
//
// This is the manual counterpart to the corpus tests: point it at an image and
// compare the printed table against Linux's `dmsetup table` by eye.

#include <iostream>
#include <string>
#include <vector>

#include "lvmfmt/label.h"
#include "lvmfmt/mapping.h"
#include "lvmfmt/metadata.h"
#include "lvmfmt/source.h"

namespace {

int usage() {
  std::cerr << "usage: lvmdump <image-or-device> [--metadata]\n"
            << "  --metadata   print the raw VG metadata text instead of the table\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();

  const std::string path = argv[1];
  bool dump_metadata = false;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--metadata") {
      dump_metadata = true;
    } else {
      return usage();
    }
  }

  std::string error;
  auto source = lvmfmt::open_storage_source(path, false, error);
  if (!source) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }

  auto label = lvmfmt::read_pv_label(*source, error);
  if (!label) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }

  std::cout << "PV UUID:      " << lvmfmt::format_uuid(label->pv_uuid) << "\n"
            << "label at:     byte " << label->label_offset << " (sector "
            << label->label_sector << ")\n"
            << "device size:  " << label->device_size << " bytes\n"
            << "data areas:   " << label->data_areas.size() << "\n"
            << "metadata areas: " << label->metadata_areas.size() << "\n";
  for (const auto& area : label->metadata_areas) {
    std::cout << "  mda at " << area.offset << " size " << area.size << "\n";
  }

  auto text = lvmfmt::read_vg_metadata(*source, *label, error);
  if (!text) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }
  if (dump_metadata) {
    std::cout << "\n" << *text << "\n";
    return 0;
  }

  auto vg = lvmfmt::parse_vg_metadata(*text, error);
  if (!vg) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }

  std::cout << "\nVG " << vg->name << "  seqno=" << vg->seqno
            << "  extent_size=" << vg->extent_size_sectors << " sectors"
            << "  pvs=" << vg->pvs.size() << "  lvs=" << vg->lvs.size();
  if (!vg->system_id.empty()) std::cout << "  system_id=" << vg->system_id;
  std::cout << "\n";

  for (const auto& pv : vg->pvs) {
    std::cout << "  " << pv.key << "  " << lvmfmt::format_uuid(pv.id)
              << "  pe_start=" << pv.pe_start_sectors << "  pe_count=" << pv.pe_count
              << "  (" << pv.device << ")\n";
  }

  std::cout << "\ndevice-mapper tables:\n";
  for (const auto& lv : vg->lvs) {
    const std::string full = vg->name + "-" + lv.name;
    if (lv.internal) {
      std::cout << "  " << full << ": (internal sub-volume, skipped)\n";
      continue;
    }
    std::string local;
    auto table = lvmfmt::build_dm_table(*vg, lv, local);
    if (!table) {
      std::cout << "  " << full << ": REFUSED — " << local << "\n";
      continue;
    }
    for (const auto& target : *table) {
      std::vector<std::string> names;
      for (const auto& dest : target.destinations) names.push_back(dest.pv_key);
      std::cout << "  " << full << ": " << lvmfmt::format_dm_target(target, names) << "\n";
    }
  }
  return 0;
}
