#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lvmcore/arena.h"
#include "lvmcore/parser.h"
#include "lvmfmt/allocator.h"
#include "lvmfmt/bytes.h"
#include "lvmfmt/commit.h"
#include "lvmfmt/config.h"
#include "lvmfmt/crc32.h"
#include "lvmfmt/label.h"
#include "lvmfmt/metadata.h"
#include "lvmfmt/source.h"
#include "testing.h"

using namespace lvmfmt;

TEST(checked_arithmetic_refuses_to_wrap) {
  CHECK(checked_add(1, 2).value() == 3);
  CHECK(!checked_add(UINT64_MAX, 1).has_value());
  CHECK(checked_mul(1ull << 40, 1ull << 10).value() == (1ull << 50));
  CHECK(!checked_mul(UINT64_MAX, 2).has_value());
  CHECK(checked_mul(0, UINT64_MAX).value() == 0);

  CHECK(range_fits(0, 10, 10));
  CHECK(!range_fits(1, 10, 10));
  CHECK(!range_fits(UINT64_MAX, 1, UINT64_MAX));
}

// The exact arithmetic the extent math does on untrusted disk numbers:
// pe_start + stripe_offset*extent_size must refuse to wrap rather than
// map a read at a rotated address. Mirrors src/driver/lvm_read.c's C++
// counterpart in mapping.cpp (build_dm_table's checked_add/checked_mul).
TEST(overflow_paths_hit_by_extent_math_shapes) {
  // (2^32 extents) * (2^32 sectors) wraps uint64 — must be nullopt.
  CHECK(!checked_mul(1ull << 32, 1ull << 32).has_value());
  // pe_start near UINT64_MAX + 1 sector must refuse.
  CHECK(!checked_add(UINT64_MAX - 511, 512).has_value());
  // A range starting near the top must not fit even a 1-byte length.
  CHECK(!range_fits(UINT64_MAX, 1, UINT64_MAX));
  // Exactly-at-the-end fits; one past does not.
  CHECK(range_fits(UINT64_MAX - 1, 1, UINT64_MAX));
  CHECK(!range_fits(UINT64_MAX, 1, UINT64_MAX));
  // The two largest representable products that must still succeed.
  CHECK(checked_mul(UINT64_MAX, 1).value() == UINT64_MAX);
  CHECK(checked_add(UINT64_MAX, 0).value() == UINT64_MAX);
  // Zero-length range at any offset fits (read_metadata_text relies on
  // this class of boundary for its `first`/`wrapped` split).
  CHECK(range_fits(UINT64_MAX, 0, UINT64_MAX));
  CHECK(range_fits(0, 0, 0));
}

// LVM CRC-32 against the one vector we have from a real corpus label:
// the constants must never silently change to the zlib variant.
TEST(lvm_crc32_edge_vectors) {
  // Initial value is LVM2's INITIAL_CRC (third_party/lvm2 lib/misc/crc.h).
  CHECK_EQ(lvm_crc32("", 0), 0xf597a6cfu);
  // Incremental application must compose exactly like a one-shot call:
  // calc_crc(calc_crc(INIT, a), b) == calc_crc(INIT, a||b). This is how
  // LVM applies it to the label (fixed span) and text (fixed span).
  const char two[] = "xy";
  const uint32_t chained = lvm_crc32("y", 1, lvm_crc32("x", 1, lvm_crc32("", 0)));
  CHECK_EQ(lvm_crc32(two, 2), chained);
  // Reflected poly 0xedb88320: crc of a single 0x01 differs from crc of a
  // single 0x80 — guards against table-build regressions (the kernel-side
  // build_crc_table had exactly this shape; see src/driver/lvm_read.c).
  CHECK(lvm_crc32("\x01", 1) != lvm_crc32("\x80", 1));
}

TEST(little_endian_readers) {
  const std::uint8_t buf[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  CHECK_EQ(le16(buf), std::uint16_t{0x0201});
  CHECK_EQ(le32(buf), std::uint32_t{0x04030201});
  CHECK_EQ(le64(buf), std::uint64_t{0x0807060504030201ull});
}

// The defining property of LVM's CRC: it is NOT the standard one. If someone
// later "fixes" crc32.cpp to match zlib, this test is what stops it.
TEST(lvm_crc32_is_not_standard_crc32) {
  const char data[] = "123456789";
  const std::uint32_t lvm = lvm_crc32(data, 9);
  const std::uint32_t standard_with_final_xor = lvm_crc32(data, 9, 0xffffffffu) ^ 0xffffffffu;
  CHECK(lvm != standard_with_final_xor);
  // Zero-length input returns the seed untouched.
  CHECK_EQ(lvm_crc32("", 0), kInitialCrc);
}

TEST(uuid_formatting_matches_lvm_grouping) {
  CHECK_EQ(format_uuid("MMmapVdo0dx2Tu6PrQPRL3hgD6BJR3Od"),
           std::string("MMmapV-do0d-x2Tu-6PrQ-PRL3-hgD6-BJR3Od"));
  CHECK_EQ(format_uuid("short"), std::string("short"));
}

TEST(config_parser_handles_the_metadata_grammar) {
  const char* text = R"(
# a comment
vg0 {
  id = "abc-def"
  seqno = 7
  status = ["RESIZEABLE", "READ", "WRITE"]
  extent_size = 8192
  physical_volumes {
    pv0 {
      id = "pvuuid"
      device = "/dev/loop2"
      pe_start = 2048
    }
  }
}
contents = "Text Format Volume Group"
)";
  std::string error;
  auto root = parse_config(text, error);
  CHECK_MSG(root.has_value(), error);

  const Section* vg = root->find_section("vg0");
  CHECK(vg != nullptr);
  CHECK_EQ(vg->string("id").value(), std::string("abc-def"));
  CHECK_EQ(vg->integer("seqno").value(), std::int64_t{7});
  CHECK_EQ(vg->integer("extent_size").value(), std::int64_t{8192});
  CHECK_EQ(vg->string_list("status").size(), std::size_t{3});
  CHECK_EQ(vg->string_list("status")[0], std::string("RESIZEABLE"));

  const Section* pvs = vg->find_section("physical_volumes");
  CHECK(pvs != nullptr);
  const Section* pv0 = pvs->find_section("pv0");
  CHECK(pv0 != nullptr);
  CHECK_EQ(pv0->integer("pe_start").value(), std::int64_t{2048});
  CHECK_EQ(root->string("contents").value(), std::string("Text Format Volume Group"));
}

TEST(config_parser_reports_errors_with_line_numbers) {
  std::string error;
  CHECK(!parse_config("vg0 {\n  id = \n", error).has_value());
  CHECK_MSG(error.find("line") != std::string::npos, error);
}

TEST(config_parser_handles_escaped_strings) {
  std::string error;
  auto root = parse_config(R"(k = "a\"b\\c")", error);
  CHECK_MSG(root.has_value(), error);
  CHECK_EQ(root->string("k").value(), std::string("a\"b\\c"));
}

namespace {

// A minimal but realistic VG: one PV, one LV split into two discontiguous
// segments — the shape that a naive start+length mapper gets wrong.
const char* kTwoSegmentVg = R"(
vgtest {
  id = "vgid"
  seqno = 4
  status = ["RESIZEABLE", "READ", "WRITE"]
  extent_size = 8192
  physical_volumes {
    pv0 {
      id = "pvid"
      device = "/dev/loop9"
      dev_size = 253952
      pe_start = 2048
      pe_count = 30
    }
  }
  logical_volumes {
    lvA {
      id = "lvid"
      status = ["READ", "WRITE", "VISIBLE"]
      segment_count = 2
      segment1 {
        start_extent = 0
        extent_count = 2
        type = "striped"
        stripe_count = 1
        stripes = ["pv0", 0]
      }
      segment2 {
        start_extent = 2
        extent_count = 4
        type = "striped"
        stripe_count = 1
        stripes = ["pv0", 4]
      }
    }
  }
}
)";

}  // namespace

TEST(metadata_model_reconstructs_segments) {
  std::string error;
  auto vg = parse_vg_metadata(kTwoSegmentVg, error);
  CHECK_MSG(vg.has_value(), error);
  CHECK_EQ(vg->name, std::string("vgtest"));
  CHECK_EQ(vg->seqno, std::uint64_t{4});
  CHECK_EQ(vg->extent_size_sectors, std::uint64_t{8192});
  CHECK_EQ(vg->pvs.size(), std::size_t{1});
  CHECK_EQ(vg->pvs[0].pe_start_sectors, std::uint64_t{2048});
  CHECK_EQ(vg->lvs.size(), std::size_t{1});

  const auto& lv = vg->lvs[0];
  CHECK(lv.supported);
  CHECK(!lv.internal);
  CHECK_EQ(lv.segments.size(), std::size_t{2});
  CHECK_EQ(lv.extent_count(), std::uint64_t{6});
}

TEST(unsupported_segment_types_are_recorded_not_thrown_away) {
  std::string text = kTwoSegmentVg;
  const auto pos = text.find("\"striped\"");
  text.replace(pos, 9, "\"raid5_ls\"");

  std::string error;
  auto vg = parse_vg_metadata(text, error);
  CHECK_MSG(vg.has_value(), error);
  // The VG still parses — only the unsupported LV is refused.
  CHECK_EQ(vg->lvs.size(), std::size_t{1});
  CHECK(!vg->lvs[0].supported);
  CHECK_MSG(vg->lvs[0].unsupported_reason.find("raid5_ls") != std::string::npos,
            vg->lvs[0].unsupported_reason);
}

TEST(vg_with_zero_extent_size_is_refused) {
  std::string text = kTwoSegmentVg;
  const auto pos = text.find("extent_size = 8192");
  text.replace(pos, 18, "extent_size = 0   ");
  std::string error;
  CHECK(!parse_vg_metadata(text, error).has_value());
}

TEST(reduce_lv_enforces_striped_divisibility) {
  VolumeGroup vg;
  vg.name = "testvg";
  vg.id = "vg-test-id";
  vg.extent_size_sectors = 8192;
  vg.seqno = 1;

  PhysicalVolume pv0;
  pv0.key = "pv0";
  pv0.id = "pv0-uuid";
  pv0.pe_count = 10;
  vg.pvs.push_back(pv0);

  PhysicalVolume pv1;
  pv1.key = "pv1";
  pv1.id = "pv1-uuid";
  pv1.pe_count = 10;
  vg.pvs.push_back(pv1);

  LogicalVolume lv;
  lv.name = "stripe_lv";
  lv.id = "lv-stripe-id";

  Segment seg;
  seg.start_extent = 0;
  seg.extent_count = 4;
  seg.type = "striped";
  seg.stripe_count = 2;
  seg.stripe_size_sectors = 128;
  seg.stripes.push_back({"pv0", 0});
  seg.stripes.push_back({"pv1", 0});
  lv.segments.push_back(seg);
  vg.lvs.push_back(lv);

  std::string err;
  // Reducing by 1 leaves 3 extents, which is not divisible by stripe_count (2)
  CHECK(!reduce_lv(vg, "stripe_lv", 1, err));
  CHECK(err.find("cannot reduce striped segment to non-multiple of stripe count") != std::string::npos);

  // Reducing by 2 leaves 2 extents (divisible by 2) -> must succeed
  CHECK(reduce_lv(vg, "stripe_lv", 2, err));
  CHECK_EQ(vg.lvs[0].extent_count(), std::uint64_t{2});
  CHECK_EQ(vg.lvs[0].segments[0].extent_count, std::uint64_t{2});
}

TEST(serialize_vg_escapes_device_and_preserves_status_and_system_id) {
  VolumeGroup vg;
  vg.name = "vg_escape";
  vg.id = "11111122223333444455556666777788";
  vg.seqno = 9;
  vg.extent_size_sectors = 8192;
  vg.system_id = "win-host-01";
  vg.status = {"RESIZEABLE", "READ", "WRITE", "EXPORTED"};

  PhysicalVolume pv;
  pv.key = "pv0";
  pv.id = "aaaaaa11112222333344445555666677";
  pv.device = "\\\\.\\PhysicalDrive11";
  pv.status = {"ALLOCATABLE"};
  pv.dev_size_sectors = 65536;
  pv.pe_start_sectors = 2048;
  pv.pe_count = 7;
  vg.pvs.push_back(pv);

  LogicalVolume lv;
  lv.name = "lv_ro";
  lv.id = "bbbbbb11112222333344445555666677";
  lv.status = {"READ", "VISIBLE"};
  Segment seg;
  seg.start_extent = 0;
  seg.extent_count = 2;
  seg.type = "striped";
  seg.stripe_count = 1;
  seg.stripes.push_back({"pv0", 0});
  lv.segments.push_back(seg);
  vg.lvs.push_back(lv);

  std::string err;
  auto text = serialize_vg_to_text(vg, err);
  CHECK_MSG(text.has_value(), err);
  CHECK(text->find("device = \"\\\\\\\\.\\\\PhysicalDrive11\"") != std::string::npos);
  CHECK(text->find("system_id = \"win-host-01\"") != std::string::npos);
  CHECK(text->find("\"EXPORTED\"") != std::string::npos);

  auto parsed = parse_vg_metadata(*text, err);
  CHECK_MSG(parsed.has_value(), err);
  CHECK_EQ(parsed->system_id, std::string("win-host-01"));
  CHECK(parsed->is_exported());
  CHECK_EQ(parsed->pvs.size(), std::size_t{1});
  CHECK_EQ(parsed->pvs[0].device, std::string("\\\\.\\PhysicalDrive11"));
  CHECK_EQ(parsed->lvs.size(), std::size_t{1});
  CHECK(parsed->lvs[0].is_read_only());

  // Freestanding kernel parser must agree on system_id and LV read-only state.
  std::vector<std::uint8_t> arena_buf(256u * 1024u);
  lvmcore_arena arena;
  lvmcore_arena_init(&arena, arena_buf.data(), arena_buf.size());
  lvmcore_volume_group core_vg{};
  lvmcore_err core_err{};
  CHECK_EQ(static_cast<int>(lvmcore_parse_vg_text(text->c_str(), text->size(), &arena, &core_vg, &core_err)),
           static_cast<int>(LVMCORE_OK));
  CHECK_EQ(std::string(core_vg.system_id), std::string("win-host-01"));
  CHECK_EQ(core_vg.read_only, 0);
  CHECK_EQ(core_vg.lv_count, std::size_t{1});
  CHECK_EQ(core_vg.lvs[0].read_only, 1);
}

TEST(serialize_vg_refuses_unsupported_segments_closed) {
  VolumeGroup vg;
  vg.name = "vg_cache";
  vg.id = "11111122223333444455556666777799";
  vg.seqno = 1;
  vg.extent_size_sectors = 8192;
  PhysicalVolume pv;
  pv.key = "pv0";
  pv.id = "aaaaaa11112222333344445555666688";
  pv.pe_start_sectors = 2048;
  pv.pe_count = 7;
  vg.pvs.push_back(pv);

  LogicalVolume lv;
  lv.name = "lv_cache";
  lv.id = "bbbbbb11112222333344445555666688";
  lv.supported = false;
  lv.unsupported_reason = "unsupported segment type 'cache'";
  Segment seg;
  seg.start_extent = 0;
  seg.extent_count = 2;
  seg.type = "cache";
  lv.segments.push_back(seg);
  vg.lvs.push_back(lv);

  std::string err;
  CHECK(!serialize_vg_to_text(vg, err).has_value());
  CHECK(!err.empty());
}

TEST(serialize_vg_roundtrips_tags_and_complex_segments) {
  const char* meta = R"(
vgfull {
  id = "111111-2222-3333-4444-5555-6666-777788"
  seqno = 5
  status = ["RESIZEABLE", "READ", "WRITE"]
  flags = []
  tags = ["vg_prod", "tier1"]
  extent_size = 8192
  physical_volumes {
    pv0 {
      id = "aaaaaa-1111-2222-3333-4444-5555-666677"
      device = "/dev/loop0"
      status = ["ALLOCATABLE"]
      flags = []
      tags = ["ssd"]
      dev_size = 262144
      pe_start = 2048
      pe_count = 31
    }
    pv1 {
      id = "bbbbbb-1111-2222-3333-4444-5555-666677"
      device = "/dev/loop1"
      status = ["ALLOCATABLE"]
      flags = []
      tags = ["nvme"]
      dev_size = 262144
      pe_start = 2048
      pe_count = 31
    }
  }
  logical_volumes {
    tpool {
      id = "cccccc-1111-2222-3333-4444-5555-000001"
      status = ["READ", "WRITE", "VISIBLE"]
      segment_count = 1
      segment1 {
        start_extent = 0
        extent_count = 4
        type = "thin-pool"
        metadata = "tpool_tmeta"
        pool = "tpool_tdata"
        transaction_id = 3
        chunk_size = 128
        discards = "passdown"
        zero_new_blocks = 1
      }
    }
    tpool_tmeta {
      id = "cccccc-1111-2222-3333-4444-5555-000002"
      status = ["READ", "WRITE"]
      segment_count = 1
      segment1 { start_extent = 0 extent_count = 1 type = "striped" stripe_count = 1 stripes = ["pv0", 0] }
    }
    tpool_tdata {
      id = "cccccc-1111-2222-3333-4444-5555-000003"
      status = ["READ", "WRITE"]
      segment_count = 1
      segment1 { start_extent = 0 extent_count = 4 type = "striped" stripe_count = 1 stripes = ["pv0", 1] }
    }
    lvthin {
      id = "cccccc-1111-2222-3333-4444-5555-000004"
      status = ["READ", "WRITE", "VISIBLE"]
      tags = ["thin_data"]
      segment_count = 1
      segment1 {
        start_extent = 0
        extent_count = 2
        type = "thin"
        thin_pool = "tpool"
        transaction_id = 1
        device_id = 7
      }
    }
    lvraid {
      id = "cccccc-1111-2222-3333-4444-5555-000005"
      status = ["READ", "WRITE", "VISIBLE"]
      tags = ["redundant"]
      segment_count = 1
      segment1 {
        start_extent = 0
        extent_count = 2
        type = "raid1"
        device_count = 2
        region_size = 4096
        raids = ["lvraid_rmeta_0", "lvraid_rimage_0", "lvraid_rmeta_1", "lvraid_rimage_1"]
      }
    }
    lvraid_rmeta_0 {
      id = "cccccc-1111-2222-3333-4444-5555-000006"
      status = ["READ", "WRITE"]
      segment_count = 1
      segment1 { start_extent = 0 extent_count = 1 type = "striped" stripe_count = 1 stripes = ["pv0", 5] }
    }
    lvraid_rimage_0 {
      id = "cccccc-1111-2222-3333-4444-5555-000007"
      status = ["READ", "WRITE"]
      segment_count = 1
      segment1 { start_extent = 0 extent_count = 2 type = "striped" stripe_count = 1 stripes = ["pv0", 6] }
    }
    lvraid_rmeta_1 {
      id = "cccccc-1111-2222-3333-4444-5555-000008"
      status = ["READ", "WRITE"]
      segment_count = 1
      segment1 { start_extent = 0 extent_count = 1 type = "striped" stripe_count = 1 stripes = ["pv1", 0] }
    }
    lvraid_rimage_1 {
      id = "cccccc-1111-2222-3333-4444-5555-000009"
      status = ["READ", "WRITE"]
      segment_count = 1
      segment1 { start_extent = 0 extent_count = 2 type = "striped" stripe_count = 1 stripes = ["pv1", 1] }
    }
  }
}
)";
  std::string err;
  auto vg1 = parse_vg_metadata(meta, err);
  CHECK_MSG(vg1.has_value(), err);
  CHECK_EQ(vg1->tags.size(), std::size_t{2});
  CHECK_EQ(vg1->pvs[0].tags.size(), std::size_t{1});
  CHECK_EQ(vg1->find_lv("lvthin")->tags[0], std::string("thin_data"));

  auto text = serialize_vg_to_text(*vg1, err);
  CHECK_MSG(text.has_value(), err);
  auto vg2 = parse_vg_metadata(*text, err);
  CHECK_MSG(vg2.has_value(), err);
  CHECK(vg2->tags == vg1->tags);
  CHECK(vg2->pvs[0].tags == vg1->pvs[0].tags);
  CHECK(vg2->pvs[1].tags == vg1->pvs[1].tags);
  CHECK(vg2->find_lv("lvthin")->tags == vg1->find_lv("lvthin")->tags);
  CHECK_EQ(vg2->find_lv("lvthin")->segments[0].device_id, std::uint64_t{7});
  CHECK_EQ(vg2->find_lv("tpool")->segments[0].chunk_size_sectors, std::uint64_t{128});
  CHECK_EQ(vg2->find_lv("lvraid")->segments[0].raid_legs.size(), std::size_t{2});
}

TEST(circular_mda_ring_wraps_across_boundary_and_stays_valid) {
  namespace fs = std::filesystem;
  const fs::path pv_path = fs::temp_directory_path() / "winlvm_unit_mda_wrap.img";
  struct Guard {
    fs::path p;
    ~Guard() { std::error_code ec; fs::remove(p, ec); }
  } guard{pv_path};

  {
    std::ofstream f(pv_path, std::ios::binary | std::ios::trunc);
    CHECK(f.is_open());
    std::vector<char> zeros(65536, 0);
    for (int i = 0; i < 128; ++i) f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
  }

  std::string err;
  auto src = open_storage_source(pv_path.string(), true, err);
  CHECK_MSG(src != nullptr, err);

  PvCreateOptions opts;
  opts.pe_start_sectors = 2048;
  opts.mda_size_sectors = 32;  // 16 KiB circular ring buffer
  std::string pv_uuid;
  CHECK_MSG(pvcreate_initialize(*src, opts, pv_uuid, err), err);

  VolumeGroup vg;
  vg.name = "vg_ring";
  vg.id = "99999988887777666655554444333322";
  vg.seqno = 0;
  vg.extent_size_sectors = 8192;
  vg.status = {"RESIZEABLE", "READ", "WRITE"};

  PhysicalVolume pv;
  pv.key = "pv0";
  pv.id = pv_uuid;
  pv.device = "/dev/loop0";
  pv.status = {"ALLOCATABLE"};
  pv.dev_size_sectors = (8ULL * 1024ULL * 1024ULL) / 512ULL;
  pv.pe_start_sectors = 2048;
  pv.pe_count = 1;
  vg.pvs.push_back(pv);

  LogicalVolume lv;
  lv.name = "lv_ring";
  lv.id = "1234567890abcdef1234567890abcdef";
  lv.status = {"READ", "WRITE", "VISIBLE"};
  Segment seg;
  seg.start_extent = 0;
  seg.extent_count = 1;
  seg.type = "striped";
  seg.stripe_count = 1;
  seg.stripes.push_back({"pv0", 0});
  lv.segments.push_back(seg);
  vg.lvs.push_back(lv);

  int wrap_count = 0;
  int ring_revolutions = 0;
  std::uint64_t prev_offset = 0;
  std::vector<std::uint8_t> core_arena_buf(512u * 1024u);

  for (int i = 1; i <= 60; ++i) {
    vg.seqno = static_cast<std::uint64_t>(i);
    vg.lvs[0].name = (i % 2 == 0) ? "lv_ring_even" : "lv_ring_odd";

    std::vector<ByteSource*> targets = {src.get()};
    CHECK_MSG(commit_vg_metadata(vg, targets, err), err);

    auto lbl = read_pv_label(*src, err);
    CHECK_MSG(lbl.has_value(), err);
    CHECK_EQ(lbl->metadata_areas.size(), std::size_t{1});
    CHECK_EQ(lbl->metadata_areas[0].size, std::uint64_t{16384});

    auto mda = read_mda_header(*src, lbl->metadata_areas[0], err);
    CHECK_MSG(mda.has_value(), err);
    if (prev_offset > 0 && mda->commit_offset < prev_offset) {
      ring_revolutions++;
    }
    prev_offset = mda->commit_offset;
    if (mda->commit_offset + mda->commit_size > mda->size) {
      wrap_count++;
    }

    auto text = read_vg_metadata(*src, *lbl, err);
    CHECK_MSG(text.has_value(), err);
    auto parsed = parse_vg_metadata(*text, err);
    CHECK_MSG(parsed.has_value(), err);
    CHECK_EQ(parsed->seqno, vg.seqno);
    CHECK_EQ(parsed->lvs[0].name, vg.lvs[0].name);

    // Confirm freestanding lvmcore also decodes the wrapped ring buffer cleanly.
    auto read_cb = [](void* impl, std::uint64_t off, void* buf, std::size_t len) -> std::size_t {
      auto* s = static_cast<ByteSource*>(impl);
      return s->read(off, buf, len) ? len : 0;
    };
    lvmcore_byte_source csrc{src.get(), src->size(), read_cb};
    lvmcore_arena arena;
    lvmcore_arena_init(&arena, core_arena_buf.data(), core_arena_buf.size());
    lvmcore_volume_group cvg{};
    lvmcore_err cerr{};
    CHECK_EQ(static_cast<int>(lvmcore_read_vg(&csrc, LVMCORE_READ_FLAG_DEFAULT, &arena, &cvg, &cerr)),
             static_cast<int>(LVMCORE_OK));
    CHECK_EQ(cvg.seqno, vg.seqno);
  }

  CHECK_MSG(wrap_count >= 2,
            "expected >= 2 active ring wraps across 60 commits, observed " + std::to_string(wrap_count));
  CHECK_MSG(ring_revolutions >= 3,
            "expected >= 3 full ring revolutions across 60 commits, observed " + std::to_string(ring_revolutions));
  testing::note("observed " + std::to_string(wrap_count) + " active wraps and " +
                std::to_string(ring_revolutions) + " ring revolutions across 60 commits");
}
