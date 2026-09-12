#include <string>

#include "lvmfmt/bytes.h"
#include "lvmfmt/config.h"
#include "lvmfmt/crc32.h"
#include "lvmfmt/label.h"
#include "lvmfmt/metadata.h"
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
  text.replace(pos, 9, "\"thin\"");

  std::string error;
  auto vg = parse_vg_metadata(text, error);
  CHECK_MSG(vg.has_value(), error);
  // The VG still parses — only the LV is refused.
  CHECK_EQ(vg->lvs.size(), std::size_t{1});
  CHECK(!vg->lvs[0].supported);
  CHECK_MSG(vg->lvs[0].unsupported_reason.find("thin") != std::string::npos,
            vg->lvs[0].unsupported_reason);
}

TEST(vg_with_zero_extent_size_is_refused) {
  std::string text = kTwoSegmentVg;
  const auto pos = text.find("extent_size = 8192");
  text.replace(pos, 18, "extent_size = 0   ");
  std::string error;
  CHECK(!parse_vg_metadata(text, error).has_value());
}
