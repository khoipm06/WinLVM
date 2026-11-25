// Oracle tests against the ground-truth corpus.
//
// These are the tests that matter. Everything else in this suite checks that
// the code agrees with itself; these check that it agrees with Linux.
//
// There are two tiers, deliberately separated:
//
//   * metadata-text tests read `vg-metadata.txt` (LVM's own vgcfgbackup output)
//     and compare our computed device-mapper table against `dmsetup table`.
//     They exercise the config parser, the object model, and the extent maths.
//
//   * image tests read the real disk images, so they additionally exercise the
//     binary layer: label discovery, PV header, mda_header, and the metadata
//     ring buffer.
//
// The split means a corrupt or stale image set cannot quietly disable the
// mapping checks.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "lvmfmt/label.h"
#include "lvmfmt/mapping.h"
#include "lvmfmt/metadata.h"
#include "lvmfmt/source.h"
#include "testing.h"

namespace fs = std::filesystem;
using namespace lvmfmt;

namespace {

const fs::path kExpectedDir = WINLVM_CORPUS_EXPECTED;
const fs::path kImagesDir = WINLVM_CORPUS_IMAGES;

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream os;
  os << in.rdbuf();
  return os.str();
}

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// dmsetup prints devices as major:minor. Loop devices are major 7, so the
// metadata's "/dev/loopN" maps to "7:N". Used only to make our output textually
// comparable to the oracle; nothing in lvmfmt trusts these paths.
std::string device_to_major_minor(const std::string& device) {
  const std::string prefix = "/dev/loop";
  if (device.rfind(prefix, 0) != 0) return device;
  const std::string n = device.substr(prefix.size());
  if (n.empty() ||
      !std::all_of(n.begin(), n.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    return device;
  }
  return "7:" + n;
}

std::map<std::string, std::vector<std::string>> parse_oracle_table(const std::string& text) {
  std::map<std::string, std::vector<std::string>> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    out[trim(line.substr(0, colon))].push_back(trim(line.substr(colon + 1)));
  }
  return out;
}

std::vector<fs::path> images_for(const std::string& case_name) {
  std::vector<fs::path> found;
  const fs::path single = kImagesDir / (case_name + ".img");
  if (fs::exists(single)) {
    found.push_back(single);
    return found;
  }
  for (char suffix = 'a'; suffix <= 'z'; ++suffix) {
    const fs::path p = kImagesDir / (case_name + "-" + suffix + ".img");
    if (!fs::exists(p)) break;
    found.push_back(p);
  }
  return found;
}

std::vector<std::string> corpus_cases() {
  std::vector<std::string> names;
  if (!fs::exists(kExpectedDir)) return names;
  for (const auto& entry : fs::directory_iterator(kExpectedDir)) {
    if (!entry.is_directory()) continue;
    const std::string name = entry.path().filename().string();
    // LUKS containers are opaque until the luksfmt layer exists (Phase 5).
    if (name.rfind("luks2-", 0) == 0) continue;
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

VolumeGroup load_vg_from_metadata_text(const std::string& case_name) {
  const std::string text = read_file(kExpectedDir / case_name / "vg-metadata.txt");
  CHECK_MSG(!text.empty(), "missing vg-metadata.txt for " + case_name);
  std::string error;
  auto vg = parse_vg_metadata(text, error);
  CHECK_MSG(vg.has_value(), case_name + ": " + error);
  return std::move(*vg);
}

// Compares every mappable LV in `vg` against the case's dmsetup oracle.
// Returns {compared, refused}.
std::pair<int, int> compare_against_oracle(const std::string& case_name, const VolumeGroup& vg) {
  const auto oracle =
      parse_oracle_table(read_file(kExpectedDir / case_name / "dmsetup-table.txt"));
  CHECK_MSG(!oracle.empty(), case_name + ": oracle dmsetup table is empty");

  int compared = 0;
  int refused = 0;

  for (const auto& lv : vg.lvs) {
    if (lv.internal || lv.is_thin_pool) continue;

    std::string error;
    auto table = build_dm_table(vg, lv, error);

    if (!lv.supported) {
      CHECK_MSG(!table.has_value(),
                case_name + "/" + lv.name + ": unsupported LV produced a table anyway");
      testing::note(case_name + "/" + lv.name + ": refused — " + error);
      ++refused;
      continue;
    }
    CHECK_MSG(table.has_value(), case_name + "/" + lv.name + ": " + error);

    const std::string dm_name = vg.name + "-" + lv.name;
    auto it = oracle.find(dm_name);
    CHECK_MSG(it != oracle.end(), case_name + ": oracle has no table for " + dm_name);
    CHECK_MSG(it->second.size() == table->size(),
              case_name + "/" + lv.name + ": expected " + std::to_string(it->second.size()) +
                  " targets, produced " + std::to_string(table->size()));

    for (std::size_t i = 0; i < table->size(); ++i) {
      const DmTarget& t = (*table)[i];
      if (t.type == "linear" || t.type == "striped") {
        std::vector<std::string> device_names;
        for (const auto& dest : t.destinations) {
          const PhysicalVolume* pv = vg.find_pv(dest.pv_key);
          device_names.push_back(device_to_major_minor(pv ? pv->device : dest.pv_key));
        }
        const std::string ours = format_dm_target(t, device_names);
        CHECK_MSG(ours == it->second[i],
                  case_name + "/" + lv.name + " target " + std::to_string(i) +
                      "\n      linux: " + it->second[i] + "\n      ours:  " + ours);
      } else {
        // Complex targets use ephemeral Linux dm major:minor numbers in dmsetup-table.txt;
        // compare start, length, target type, and semantic parameters.
        std::istringstream iss(it->second[i]);
        std::uint64_t o_start = 0, o_len = 0;
        std::string o_type;
        iss >> o_start >> o_len >> o_type;
        CHECK_EQ(t.start_sector, o_start);
        CHECK_EQ(t.length_sectors, o_len);
        if (t.type == "raid1") {
          std::string o_raid_level;
          iss >> o_raid_level;
          CHECK_EQ(o_type, std::string("raid"));
          CHECK_EQ(o_raid_level, std::string("raid1"));
          CHECK_MSG(it->second[i].find("region_size " + std::to_string(t.region_size_sectors)) != std::string::npos,
                    it->second[i]);
        } else {
          CHECK_EQ(t.type, o_type);
          if (t.type == "thin") {
            std::string pool_dev;
            std::uint64_t dev_id = 0;
            iss >> pool_dev >> dev_id;
            CHECK_EQ(t.device_id, dev_id);
          } else if (t.type == "snapshot") {
            std::string o_orig, o_cow, o_mode;
            std::uint64_t o_chunk = 0;
            iss >> o_orig >> o_cow >> o_mode >> o_chunk;
            CHECK_EQ(t.chunk_sectors, o_chunk);
          }
        }
      }
    }

    testing::note(case_name + "/" + lv.name + ": " + std::to_string(table->size()) +
                  " target(s) match Linux");
    ++compared;
  }
  return {compared, refused};
}

}  // namespace

// ===========================================================================
// Tier 1 — metadata text. Exercises parsing, the model, and extent maths.
// ===========================================================================

TEST(corpus_oracles_are_present) {
  CHECK_MSG(fs::exists(kExpectedDir),
            std::string("corpus oracles missing at ") + kExpectedDir.string());
  const auto cases = corpus_cases();
  CHECK_MSG(!cases.empty(), "no corpus cases found");
  testing::note(std::to_string(cases.size()) + " non-LUKS cases available");
}

// The headline test: our computed table must equal the kernel's, textually.
TEST(corpus_dm_tables_match_linux) {
  int compared = 0;
  int refused = 0;
  for (const auto& name : corpus_cases()) {
    const auto vg = load_vg_from_metadata_text(name);
    const auto [c, r] = compare_against_oracle(name, vg);
    compared += c;
    refused += r;
  }
  CHECK_MSG(compared >= 8, "expected to verify at least 8 logical volumes, verified " +
                               std::to_string(compared));
  testing::note(std::to_string(compared) + " LVs matched Linux, " + std::to_string(refused) +
                " correctly refused");
}

TEST(corpus_multisegment_lv_is_discontiguous) {
  const auto vg = load_vg_from_metadata_text("linear-1pv-multiseg");
  const LogicalVolume* lv = vg.find_lv("lvA");
  CHECK(lv != nullptr);
  CHECK_MSG(lv->segments.size() >= 3,
            "expected >= 3 segments, got " + std::to_string(lv->segments.size()));

  std::string error;
  auto table = build_dm_table(vg, *lv, error);
  CHECK_MSG(table.has_value(), error);

  // Physical offsets must not simply ascend in lockstep with logical ones —
  // that is the whole point of this fixture.
  bool gap_found = false;
  for (std::size_t i = 1; i < table->size(); ++i) {
    const auto& prev = (*table)[i - 1];
    const auto& cur = (*table)[i];
    if (prev.destinations.front().offset_sectors + prev.length_sectors !=
        cur.destinations.front().offset_sectors) {
      gap_found = true;
    }
  }
  CHECK_MSG(gap_found, "fixture no longer exercises discontiguous physical extents");
}

TEST(corpus_striped_lv_reports_stripes_and_chunk) {
  const auto vg = load_vg_from_metadata_text("striped-2pv");
  const LogicalVolume* lv = vg.find_lv("lvstr");
  CHECK(lv != nullptr);

  std::string error;
  auto table = build_dm_table(vg, *lv, error);
  CHECK_MSG(table.has_value(), error);
  CHECK_EQ(table->size(), std::size_t{1});
  CHECK_EQ(table->front().type, std::string("striped"));
  CHECK_EQ(table->front().stripe_count, std::uint64_t{2});
  CHECK_EQ(table->front().chunk_sectors, std::uint64_t{128});  // 64 KiB
  CHECK_EQ(table->front().destinations.size(), std::size_t{2});
}

TEST(corpus_spanning_lv_crosses_two_physical_volumes) {
  const auto vg = load_vg_from_metadata_text("linear-2pv");
  CHECK_EQ(vg.pvs.size(), std::size_t{2});
  const LogicalVolume* lv = vg.find_lv("lvspan");
  CHECK(lv != nullptr);

  std::string error;
  auto table = build_dm_table(vg, *lv, error);
  CHECK_MSG(table.has_value(), error);
  CHECK_MSG(table->size() >= 2, "expected the LV to span two devices");
  CHECK_MSG(table->front().destinations.front().pv_key !=
                table->back().destinations.front().pv_key,
            "both targets landed on the same physical volume");
}

TEST(corpus_odd_geometry_uses_non_default_extent_size) {
  const auto vg = load_vg_from_metadata_text("odd-geometry");
  CHECK_EQ(vg.extent_size_sectors, std::uint64_t{4096});  // 2 MiB, not the 4 MiB default
  CHECK_MSG(vg.seqno > 20, "expected a churned seqno, got " + std::to_string(vg.seqno));
  testing::note("odd-geometry seqno = " + std::to_string(vg.seqno));
}

TEST(corpus_mixed_vg_refuses_unsupported_lvs_individually) {
  const auto vg = load_vg_from_metadata_text("mixed-vg");
  CHECK_MSG(vg.lvs.size() >= 10, "mixed-vg should contain supported and unsupported LVs");

  // Supported LVs in mixed-vg must build valid DM targets.
  for (const char* ok_name : {"lvplain", "lvsnap", "lvthin", "lvraid"}) {
    const LogicalVolume* lv = vg.find_lv(ok_name);
    CHECK(lv != nullptr);
    CHECK_MSG(lv->supported && !lv->internal, std::string(ok_name) + " must be supported");
    std::string error;
    CHECK_MSG(build_dm_table(vg, *lv, error).has_value(), error);
  }

  // Thin-pool container must not be directly mountable.
  const LogicalVolume* pool = vg.find_lv("tpool");
  CHECK(pool != nullptr);
  CHECK(pool->is_thin_pool);
  {
    std::string error;
    CHECK(!build_dm_table(vg, *pool, error).has_value());
  }

  // Out-of-scope cache LV must fail closed individually.
  const LogicalVolume* cache_lv = vg.find_lv("lvcache");
  CHECK(cache_lv != nullptr);
  CHECK(!cache_lv->supported);
  {
    std::string error;
    CHECK(!build_dm_table(vg, *cache_lv, error).has_value());
    CHECK_MSG(!cache_lv->unsupported_reason.empty(), "lvcache refused without a reason");
  }

  // Crucially the VG itself still parsed, with both PVs intact.
  CHECK_EQ(vg.pvs.size(), std::size_t{2});
}

// A snapshot origin must be exposed as a `snapshot-origin` target (never raw
// `linear`) so writes flow through copy-on-write and never corrupt snapshots.
TEST(corpus_snapshot_origin_maps_through_cow_target) {
  const auto vg = load_vg_from_metadata_text("mixed-vg");
  const LogicalVolume* origin = vg.find_lv("lvplain");
  CHECK(origin != nullptr);
  CHECK(origin->supported);
  CHECK(origin->is_snapshot_origin);

  std::string error;
  auto table = build_dm_table(vg, *origin, error);
  CHECK_MSG(table.has_value(), error);
  CHECK_EQ(table->size(), std::size_t{1});
  CHECK_EQ(table->front().type, std::string("snapshot-origin"));

  const LogicalVolume* snap = vg.find_lv("lvsnap");
  CHECK(snap != nullptr);
  CHECK(snap->supported);
  CHECK(snap->is_snapshot_cow);
  auto snap_table = build_dm_table(vg, *snap, error);
  CHECK_MSG(snap_table.has_value(), error);
  CHECK_EQ(snap_table->front().type, std::string("snapshot"));
  CHECK_EQ(snap_table->front().chunk_sectors, std::uint64_t{8});
}

TEST(corpus_tags_and_complex_fixtures_parse_cleanly) {
  const auto vg_lin = load_vg_from_metadata_text("linear-1pv");
  CHECK_EQ(vg_lin.tags.size(), std::size_t{1});
  CHECK_EQ(vg_lin.tags[0], std::string("base_vg"));
  CHECK_EQ(vg_lin.pvs[0].tags.size(), std::size_t{1});
  CHECK_EQ(vg_lin.pvs[0].tags[0], std::string("pv_tag0"));
  CHECK_EQ(vg_lin.find_lv("lvlin")->tags.size(), std::size_t{2});

  const auto vg_thin = load_vg_from_metadata_text("thin-pool");
  CHECK(vg_thin.find_lv("tpool")->is_thin_pool);
  CHECK(vg_thin.find_lv("lvthin")->supported);
  CHECK(vg_thin.find_lv("lvthinsnap")->supported);
  CHECK_EQ(vg_thin.find_lv("lvthinsnap")->segments[0].origin, std::string("lvthin"));

  const auto vg_raid = load_vg_from_metadata_text("raid1-mirror");
  CHECK(vg_raid.find_lv("lvraid1")->supported);
  CHECK(vg_raid.find_lv("lvmirror")->supported);
  CHECK(vg_raid.find_lv("lvmirlog")->supported);
  CHECK_EQ(vg_raid.find_lv("lvmirlog")->segments[0].mirror_log, std::string("lvmirlog_mlog"));
}

// ===========================================================================
// Tier 2 — real images. Adds the binary layer: label, PV header, mda_header,
// and the metadata ring buffer.
// ===========================================================================

namespace {

// Reads the VG through the binary layer. Fails with an actionable message when
// the images are present but carry no metadata, which is what a corpus
// generated by a buggy teardown looks like.
VolumeGroup load_vg_from_image(const std::string& case_name,
                               std::vector<std::string>* label_uuids = nullptr) {
  const auto images = images_for(case_name);
  CHECK_MSG(!images.empty(),
            "no decompressed image for " + case_name + " (is the `corpus` target built?)");

  std::string metadata_text;
  std::string last_error;

  for (const auto& image : images) {
    std::string error;
    auto source = open_file_source(image.string(), error);
    CHECK_MSG(source != nullptr, error);

    auto label = read_pv_label(*source, error);
    CHECK_MSG(label.has_value(), image.filename().string() + ": " + error);
    if (label_uuids) label_uuids->push_back(format_uuid(label->pv_uuid));

    if (metadata_text.empty()) {
      auto text = read_vg_metadata(*source, *label, error);
      if (text) {
        metadata_text = *text;
      } else {
        last_error = error;
      }
    }
  }

  CHECK_MSG(!metadata_text.empty(),
            case_name + ": images carry a valid PV label but no VG metadata (" + last_error +
                ").\n      The images were generated by a build of corpus/generate.sh that "
                "removed\n      the volume groups before compressing. Re-run "
                "`sudo ./generate.sh` to regenerate.");

  std::string error;
  auto vg = parse_vg_metadata(metadata_text, error);
  CHECK_MSG(vg.has_value(), error);
  return std::move(*vg);
}

}  // namespace

TEST(corpus_images_are_readable_end_to_end) {
  int compared = 0;
  for (const auto& name : corpus_cases()) {
    const auto vg = load_vg_from_image(name);
    const auto [c, r] = compare_against_oracle(name, vg);
    (void)r;
    compared += c;
  }
  CHECK_MSG(compared >= 8, "expected to verify at least 8 logical volumes from images, got " +
                               std::to_string(compared));
}

// Every PV the metadata names must correspond to a real image we can read a
// label from. Catches label parsing that only works on the first PV.
TEST(corpus_every_metadata_pv_has_a_matching_image_label) {
  for (const auto& name : corpus_cases()) {
    std::vector<std::string> label_uuids;
    const auto vg = load_vg_from_image(name, &label_uuids);
    for (const auto& pv : vg.pvs) {
      // Metadata stores UUIDs dashed; the on-disk label stores them raw.
      const bool matched =
          std::find(label_uuids.begin(), label_uuids.end(), pv.id) != label_uuids.end();
      CHECK_MSG(matched, name + ": metadata PV " + pv.key + " (" + pv.id +
                             ") has no matching image label");
    }
    testing::note(name + ": " + std::to_string(vg.pvs.size()) + " PV(s) matched their labels");
  }
}

// odd-geometry is built with two metadata areas; both must independently yield
// identical, CRC-valid metadata.
TEST(corpus_odd_geometry_has_two_usable_metadata_areas) {
  const auto images = images_for("odd-geometry");
  CHECK(!images.empty());

  std::string error;
  auto source = open_file_source(images.front().string(), error);
  CHECK_MSG(source != nullptr, error);
  auto label = read_pv_label(*source, error);
  CHECK_MSG(label.has_value(), error);

  CHECK_MSG(label->metadata_areas.size() == 2,
            "expected 2 metadata areas, found " + std::to_string(label->metadata_areas.size()));

  std::vector<std::string> texts;
  for (const auto& area : label->metadata_areas) {
    auto mda = read_mda_header(*source, area, error);
    CHECK_MSG(mda.has_value(), "mda at " + std::to_string(area.offset) + ": " + error);
    auto text = read_metadata_text(*source, *mda, error);
    CHECK_MSG(text.has_value(), "mda at " + std::to_string(area.offset) + ": " + error);
    texts.push_back(*text);
  }
  CHECK_MSG(texts[0] == texts[1], "the two metadata areas disagree");
}

// The 4Kn image must parse through exactly the same path as the 512-byte ones.
TEST(corpus_4kn_image_parses) {
  const auto vg = load_vg_from_image("sector4k");
  CHECK_EQ(vg.name, std::string("vg4k"));
  const LogicalVolume* lv = vg.find_lv("lv4k");
  CHECK(lv != nullptr);
  std::string error;
  CHECK_MSG(build_dm_table(vg, *lv, error).has_value(), error);
}
