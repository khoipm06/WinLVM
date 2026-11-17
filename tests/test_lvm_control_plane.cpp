#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "lvmfmt/allocator.h"
#include "lvmfmt/label.h"
#include "lvmfmt/mapping.h"
#include "lvmfmt/metadata.h"
#include "lvmfmt/source.h"
#include "test_sha256.h"
#include "testing.h"
#include "tools/lvm/cmd_table.h"

namespace fs = std::filesystem;

namespace {

std::string run_cmd(const fs::path& exe, const std::string& args, int& status) {
#ifdef _WIN32
  const std::string command = "\"\"" + exe.string() + "\" " + args + " 2>&1\"";
  FILE* pipe = _popen(command.c_str(), "r");
#else
  const std::string command = "\"" + exe.string() + "\" " + args + " 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
#endif
  CHECK(pipe != nullptr);
  std::ostringstream output;
  char buffer[512];
  while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) output << buffer;
#ifdef _WIN32
  status = _pclose(pipe);
#else
  status = pclose(pipe);
#endif
  return output.str();
}

void create_blank_file(const fs::path& path, std::uint64_t size_bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  CHECK(f.is_open());
  std::vector<char> zeros(65536, 0);
  std::uint64_t written = 0;
  while (written < size_bytes) {
    std::uint64_t to_write = std::min<std::uint64_t>(size_bytes - written, zeros.size());
    f.write(zeros.data(), static_cast<std::streamsize>(to_write));
    written += to_write;
  }
  f.close();
}

struct TempDir {
  fs::path path;
  explicit TempDir(const std::string& tag) {
    path = fs::temp_directory_path() / ("winlvm_ctrl_" + tag);
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::string hash_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  CHECK(in.is_open());
  test_sha256_ctx ctx;
  test_sha256_init(&ctx);
  std::vector<char> buf(65536);
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto n = static_cast<std::size_t>(in.gcount());
    if (n > 0) test_sha256_update(&ctx, buf.data(), n);
  }
  char hex[65] = {};
  test_sha256_final_hex(&ctx, hex);
  return std::string(hex);
}

std::string normalize_raw_uuid(const std::string& id) {
  std::string out;
  out.reserve(32);
  for (char c : id) {
    if (c != '-') out.push_back(c);
  }
  return out;
}

// Fill a 512-byte sector with a deterministic unique pattern keyed by
// (tag, logical_sector_index) so any 1-sector shift or stripe/PV swap fails SHA-256.
void make_sector_pattern(std::uint64_t tag, std::uint64_t logical_sector, std::uint8_t out[512]) {
  std::uint64_t x = (tag * 0x9e3779b97f4a7c15ULL) ^ (logical_sector * 0xbf58476d1ce4e5b9ULL) ^ 0x94d049bb133111ebULL;
  for (std::size_t i = 0; i < 64; ++i) {
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    const std::uint64_t word = x * 0x2545f4914f6cdd1dULL;
    std::memcpy(out + i * 8, &word, 8);
  }
}

struct MappedSectorLoc {
  std::string pv_uuid;
  std::uint64_t pv_byte_offset;
};

MappedSectorLoc map_lv_sector(const std::vector<lvmfmt::DmTarget>& table, std::uint64_t logical_sector) {
  for (const auto& t : table) {
    if (logical_sector >= t.start_sector && logical_sector < t.start_sector + t.length_sectors) {
      const std::uint64_t rel = logical_sector - t.start_sector;
      if (t.type == "linear") {
        const auto& dst = t.destinations[0];
        return {normalize_raw_uuid(dst.pv_uuid), (dst.offset_sectors + rel) * 512ULL};
      }
      const std::uint64_t chunk_idx = rel / t.chunk_sectors;
      const std::uint64_t within = rel % t.chunk_sectors;
      const std::uint64_t dev = chunk_idx % t.stripe_count;
      const std::uint64_t stripe_row = chunk_idx / t.stripe_count;
      const auto& dst = t.destinations[static_cast<std::size_t>(dev)];
      const std::uint64_t pv_sec = dst.offset_sectors + stripe_row * t.chunk_sectors + within;
      return {normalize_raw_uuid(dst.pv_uuid), pv_sec * 512ULL};
    }
  }
  CHECK_MSG(false, "logical sector out of table bounds: " + std::to_string(logical_sector));
  return {};
}

lvmfmt::VolumeGroup load_vg_and_map_pvs(const std::vector<fs::path>& pv_paths,
                                        std::map<std::string, fs::path>& uuid_to_path) {
  uuid_to_path.clear();
  std::optional<lvmfmt::VolumeGroup> best;
  for (const auto& p : pv_paths) {
    std::string err;
    auto src = lvmfmt::open_file_source(p.string(), err);
    CHECK_MSG(src != nullptr, err);
    auto lbl = lvmfmt::read_pv_label(*src, err);
    CHECK_MSG(lbl.has_value(), err);
    uuid_to_path[normalize_raw_uuid(lbl->pv_uuid)] = p;
    auto meta = lvmfmt::read_vg_metadata(*src, *lbl, err);
    if (meta) {
      auto vg = lvmfmt::parse_vg_metadata(*meta, err);
      CHECK_MSG(vg.has_value(), err);
      if (!best || vg->seqno > best->seqno) best = std::move(*vg);
    }
  }
  CHECK(best.has_value());
  return std::move(*best);
}

void write_lv_pattern_range(const std::vector<fs::path>& pv_paths,
                            const std::string& lv_name,
                            std::uint64_t start_sector,
                            std::uint64_t sector_count,
                            std::uint64_t tag) {
  std::map<std::string, fs::path> uuid_to_path;
  auto vg = load_vg_and_map_pvs(pv_paths, uuid_to_path);
  const auto* lv = vg.find_lv(lv_name);
  CHECK(lv != nullptr);
  std::string err;
  auto table = lvmfmt::build_dm_table(vg, *lv, err);
  CHECK_MSG(table.has_value(), err);

  std::map<std::string, std::fstream> streams;
  for (const auto& kv : uuid_to_path) {
    streams[kv.first].open(kv.second, std::ios::in | std::ios::out | std::ios::binary);
    CHECK(streams[kv.first].is_open());
  }

  std::uint8_t sec[512];
  for (std::uint64_t s = start_sector; s < start_sector + sector_count; ++s) {
    const auto loc = map_lv_sector(*table, s);
    auto it = streams.find(loc.pv_uuid);
    CHECK(it != streams.end());
    make_sector_pattern(tag, s, sec);
    it->second.seekp(static_cast<std::streamoff>(loc.pv_byte_offset), std::ios::beg);
    it->second.write(reinterpret_cast<const char*>(sec), 512);
    CHECK(it->second.good());
  }
  for (auto& kv : streams) kv.second.flush();
}

std::string hash_lv_mapped_range(const std::vector<fs::path>& pv_paths,
                                 const std::string& lv_name,
                                 std::uint64_t byte_offset,
                                 std::uint64_t byte_length) {
  std::map<std::string, fs::path> uuid_to_path;
  auto vg = load_vg_and_map_pvs(pv_paths, uuid_to_path);
  const auto* lv = vg.find_lv(lv_name);
  CHECK(lv != nullptr);
  std::string err;
  auto table = lvmfmt::build_dm_table(vg, *lv, err);
  CHECK_MSG(table.has_value(), err);

  std::map<std::string, std::ifstream> streams;
  for (const auto& kv : uuid_to_path) {
    streams[kv.first].open(kv.second, std::ios::binary);
    CHECK(streams[kv.first].is_open());
  }

  test_sha256_ctx ctx;
  test_sha256_init(&ctx);
  std::uint8_t sec[512];
  std::uint64_t cur = byte_offset;
  std::uint64_t rem = byte_length;
  while (rem > 0) {
    const std::uint64_t logical_sec = cur / 512ULL;
    const std::uint64_t within = cur % 512ULL;
    const std::uint64_t take = std::min<std::uint64_t>(rem, 512ULL - within);
    const auto loc = map_lv_sector(*table, logical_sec);
    auto it = streams.find(loc.pv_uuid);
    CHECK(it != streams.end());
    it->second.seekg(static_cast<std::streamoff>(loc.pv_byte_offset + within), std::ios::beg);
    it->second.read(reinterpret_cast<char*>(sec), static_cast<std::streamsize>(take));
    CHECK(static_cast<std::uint64_t>(it->second.gcount()) == take);
    test_sha256_update(&ctx, sec, static_cast<std::size_t>(take));
    cur += take;
    rem -= take;
  }

  char hex[65] = {};
  test_sha256_final_hex(&ctx, hex);
  return std::string(hex);
}

std::string expected_pattern_sha256(std::uint64_t start_sector,
                                    std::uint64_t sector_count,
                                    std::uint64_t tag) {
  test_sha256_ctx ctx;
  test_sha256_init(&ctx);
  std::uint8_t sec[512];
  for (std::uint64_t s = start_sector; s < start_sector + sector_count; ++s) {
    make_sector_pattern(tag, s, sec);
    test_sha256_update(&ctx, sec, 512);
  }
  char hex[65] = {};
  test_sha256_final_hex(&ctx, hex);
  return std::string(hex);
}

} // namespace

TEST(lvm_control_plane_pv_lifecycle) {
  const fs::path exe = fs::path(WINLVM_LVM_EXE);
  const fs::path pv_file = fs::temp_directory_path() / "test_ctrl_pv1.img";
  create_blank_file(pv_file, 32 * 1024 * 1024);

  int status = 0;
  // 1. pvcreate
  std::string out = run_cmd(exe, "pvcreate \"" + pv_file.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("successfully created") != std::string::npos);

  // 2. verify PV label
  std::string err;
  auto src = lvmfmt::open_file_source(pv_file.string(), err);
  CHECK(src != nullptr);
  auto label = lvmfmt::read_pv_label(*src, err);
  CHECK(label.has_value());
  CHECK_EQ(label->pv_uuid.size(), 32);
  src.reset();

  // 3. pvck
  out = run_cmd(exe, "pvck \"" + pv_file.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("verified OK") != std::string::npos);

  // 4. pvdisplay
  out = run_cmd(exe, "pvdisplay \"" + pv_file.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("Physical volume") != std::string::npos);

  // 5. pvremove
  out = run_cmd(exe, "pvremove \"" + pv_file.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("successfully wiped") != std::string::npos);

  // 6. verify label is wiped
  src = lvmfmt::open_file_source(pv_file.string(), err);
  CHECK(src != nullptr);
  label = lvmfmt::read_pv_label(*src, err);
  CHECK(!label.has_value());
  src.reset();

  fs::remove(pv_file);
}

TEST(lvm_control_plane_vg_and_lv_lifecycle) {
  const fs::path exe = fs::path(WINLVM_LVM_EXE);
  const fs::path pv1 = fs::temp_directory_path() / "test_ctrl_vg_pv1.img";
  const fs::path pv2 = fs::temp_directory_path() / "test_ctrl_vg_pv2.img";
  const fs::path backup_file = fs::temp_directory_path() / "test_ctrl_backup.vg";

  create_blank_file(pv1, 32 * 1024 * 1024);
  create_blank_file(pv2, 32 * 1024 * 1024);

  int status = 0;
  // Initialize PVs
  run_cmd(exe, "pvcreate \"" + pv1.string() + "\" \"" + pv2.string() + "\"", status);
  CHECK_EQ(status, 0);

  // Create Volume Group
  std::string out = run_cmd(exe, "vgcreate test_vg \"" + pv1.string() + "\" \"" + pv2.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("successfully created") != std::string::npos);

  // Query vgs
  out = run_cmd(exe, "vgs \"" + pv1.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("test_vg") != std::string::npos);

  // Query vgdisplay
  out = run_cmd(exe, "vgdisplay \"" + pv1.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("Volume group") != std::string::npos);

  // Create Linear Logical Volume
  out = run_cmd(exe, "lvcreate -L 8M -n lv_linear test_vg \"" + pv1.string() + "\" \"" + pv2.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("created") != std::string::npos);

  // Query lvs
  out = run_cmd(exe, "lvs \"" + pv1.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("lv_linear") != std::string::npos);

  // Extend LV
  out = run_cmd(exe, "lvextend -L +4M lv_linear \"" + pv1.string() + "\" \"" + pv2.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("successfully resized") != std::string::npos);

  // Rename LV
  out = run_cmd(exe, "lvrename lv_linear lv_renamed \"" + pv1.string() + "\" \"" + pv2.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("Renamed") != std::string::npos);

  // Reduce LV
  out = run_cmd(exe, "lvreduce -L -4M lv_renamed \"" + pv1.string() + "\" \"" + pv2.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("successfully resized") != std::string::npos);

  // Backup VG configuration
  out = run_cmd(exe, "vgcfgbackup -f \"" + backup_file.string() + "\" test_vg \"" + pv1.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(fs::exists(backup_file));

  // Remove LV
  out = run_cmd(exe, "lvremove lv_renamed \"" + pv1.string() + "\" \"" + pv2.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("successfully removed") != std::string::npos);

  // Restore VG configuration
  out = run_cmd(exe, "vgcfgrestore -f \"" + backup_file.string() + "\" test_vg \"" + pv1.string() + "\" \"" + pv2.string() + "\"", status);
  CHECK_EQ(status, 0);

  // Verify LV is restored
  out = run_cmd(exe, "lvs \"" + pv1.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("lv_renamed") != std::string::npos);

  // Remove VG
  out = run_cmd(exe, "vgremove test_vg \"" + pv1.string() + "\" \"" + pv2.string() + "\"", status);
  CHECK_EQ(status, 0);

  // Cleanup
  fs::remove(pv1);
  fs::remove(pv2);
  fs::remove(backup_file);
}

TEST(lvm_control_plane_pvmove_and_data_copy) {
  const fs::path exe = fs::path(WINLVM_LVM_EXE);
  const fs::path pv_a = fs::temp_directory_path() / "test_ctrl_move_a.img";
  const fs::path pv_b = fs::temp_directory_path() / "test_ctrl_move_b.img";

  create_blank_file(pv_a, 32 * 1024 * 1024);
  create_blank_file(pv_b, 32 * 1024 * 1024);

  int status = 0;
  run_cmd(exe, "pvcreate \"" + pv_a.string() + "\" \"" + pv_b.string() + "\"", status);
  CHECK_EQ(status, 0);

  run_cmd(exe, "vgcreate move_vg \"" + pv_a.string() + "\" \"" + pv_b.string() + "\"", status);
  CHECK_EQ(status, 0);

  run_cmd(exe, "lvcreate -L 4M -n move_lv move_vg \"" + pv_a.string() + "\" \"" + pv_b.string() + "\"", status);
  CHECK_EQ(status, 0);

  // Write known magic payload into data area of pv_a
  // pe_start is 1 MiB (offset 1048576)
  const std::string magic = "WINLVM_PVMOVE_DATA_VERIFICATION_TOKEN_12345";
  {
    std::fstream f(pv_a, std::ios::in | std::ios::out | std::ios::binary);
    CHECK(f.is_open());
    f.seekp(1048576, std::ios::beg);
    f.write(magic.data(), static_cast<std::streamsize>(magic.size()));
    f.flush();
  }

  // Execute pvmove
  std::string out = run_cmd(exe, "pvmove \"" + pv_a.string() + "\" \"" + pv_b.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("successfully moved") != std::string::npos);

  // Verify that the payload is now copied to pv_b data area
  {
    std::ifstream f(pv_b, std::ios::binary);
    CHECK(f.is_open());
    f.seekg(1048576, std::ios::beg);
    std::vector<char> buf(magic.size(), 0);
    f.read(buf.data(), static_cast<std::streamsize>(magic.size()));
    std::string read_magic(buf.data(), buf.size());
    CHECK_EQ(read_magic, magic);
  }

  // Cleanup
  fs::remove(pv_a);
  fs::remove(pv_b);
}

TEST(lvm_control_plane_all_36_commands_help) {
  const fs::path exe = fs::path(WINLVM_LVM_EXE);
  const auto& cmds = lvmfmt::get_all_commands();
  CHECK_MSG(cmds.size() >= 36, "Command table should have at least 36 upstream LVM2 commands");

  for (const auto& cmd : cmds) {
    int status = 0;
    std::string out = run_cmd(exe, cmd.name + " --help", status);
    CHECK_EQ(status, 0);
    CHECK_MSG(out.find("Usage:") != std::string::npos, "Help missing for " + cmd.name);
    CHECK_MSG(out.find(cmd.name) != std::string::npos, "Command name missing in help for " + cmd.name);
  }
}

TEST(lvm_control_plane_dual_mda_odd_geometry_commit) {
  TempDir tmp("dual_mda");
  const fs::path exe = fs::path(WINLVM_LVM_EXE);
  const fs::path img = tmp.path / "odd-geometry.img";
  fs::copy_file(fs::path(WINLVM_CORPUS_IMAGES) / "odd-geometry.img", img,
                fs::copy_options::overwrite_existing);

  // Inspect initial dual-MDA layout and seqno.
  std::string err;
  std::uint64_t orig_seqno = 0;
  {
    auto src = lvmfmt::open_file_source(img.string(), err);
    CHECK_MSG(src != nullptr, err);
    auto lbl = lvmfmt::read_pv_label(*src, err);
    CHECK_MSG(lbl.has_value(), err);
    CHECK_EQ(lbl->metadata_areas.size(), std::size_t{2});
    CHECK_EQ(lbl->metadata_areas[0].offset, std::uint64_t{4096});
    CHECK_EQ(lbl->metadata_areas[1].offset, std::uint64_t{133169152});
    auto meta = lvmfmt::read_vg_metadata(*src, *lbl, err);
    CHECK_MSG(meta.has_value(), err);
    auto vg = lvmfmt::parse_vg_metadata(*meta, err);
    CHECK_MSG(vg.has_value(), err);
    orig_seqno = vg->seqno;
  }

  // Create a new LV on odd-geometry via lvm CLI.
  int status = -1;
  std::string out = run_cmd(exe, "lvcreate -L 4M -n lv_dual_mda vgodd \"" + img.string() + "\"", status);
  CHECK_MSG(status == 0, out);

  // Verify BOTH MDAs updated identically with incremented seqno.
  {
    auto src = lvmfmt::open_file_source(img.string(), err);
    CHECK_MSG(src != nullptr, err);
    auto lbl = lvmfmt::read_pv_label(*src, err);
    CHECK_MSG(lbl.has_value(), err);
    CHECK_EQ(lbl->metadata_areas.size(), std::size_t{2});
    CHECK_EQ(lbl->metadata_areas[0].offset, std::uint64_t{4096});
    CHECK_EQ(lbl->metadata_areas[1].offset, std::uint64_t{133169152});

    std::vector<std::string> mda_texts;
    for (const auto& area : lbl->metadata_areas) {
      auto mda = lvmfmt::read_mda_header(*src, area, err);
      CHECK_MSG(mda.has_value(), err);
      auto text = lvmfmt::read_metadata_text(*src, *mda, err);
      CHECK_MSG(text.has_value(), err);
      auto vg = lvmfmt::parse_vg_metadata(*text, err);
      CHECK_MSG(vg.has_value(), err);
      CHECK_EQ(vg->seqno, orig_seqno + 1);
      CHECK(vg->find_lv("lvodd") != nullptr);
      CHECK(vg->find_lv("lv_dual_mda") != nullptr);
      mda_texts.push_back(*text);
    }
    CHECK_EQ(mda_texts[0], mda_texts[1]);
  }

  // Confirm pvck and vgck pass on the mutated dual-MDA image.
  out = run_cmd(exe, "pvck \"" + img.string() + "\"", status);
  CHECK_MSG(status == 0, out);
  out = run_cmd(exe, "vgck vgodd \"" + img.string() + "\"", status);
  CHECK_MSG(status == 0, out);

  // Confirm original lvodd stream and all oracle windows still match corpus/expected/odd-geometry/lv-hashes.txt.
  std::ifstream hf(fs::path(WINLVM_CORPUS_EXPECTED) / "odd-geometry" / "lv-hashes.txt");
  CHECK(hf.is_open());
  std::string line;
  int checked = 0;
  while (std::getline(hf, line)) {
    if (line.empty() || line[0] == '#') continue;
    char lv_name[64] = {};
    unsigned long long off = 0, len = 0;
    char hex[80] = {};
    if (std::sscanf(line.c_str(), "%63s offset=%llu length=%llu sha256=%64s",
                    lv_name, &off, &len, hex) == 4) {
      const std::string got = hash_lv_mapped_range({img}, lv_name, off, len);
      CHECK_EQ(got, std::string(hex));
      checked++;
    } else if (std::sscanf(line.c_str(), "%63s whole sha256=%64s", lv_name, hex) == 2) {
      const std::string got = hash_lv_mapped_range({img}, lv_name, 0, 16ULL * 1024ULL * 1024ULL);
      CHECK_EQ(got, std::string(hex));
      checked++;
    }
  }
  CHECK_EQ(checked, 5);
}

TEST(lvm_control_plane_cli_mda_ring_wraparound) {
  TempDir tmp("mda_wrap_cli");
  const fs::path exe = fs::path(WINLVM_LVM_EXE);
  const fs::path pv = tmp.path / "ring_pv.img";
  create_blank_file(pv, 16 * 1024 * 1024);

  int status = -1;
  std::string out = run_cmd(exe, "pvcreate --metadatasize 16k \"" + pv.string() + "\"", status);
  CHECK_MSG(status == 0, out);
  out = run_cmd(exe, "vgcreate -s 4M ring_vg \"" + pv.string() + "\"", status);
  CHECK_MSG(status == 0, out);
  out = run_cmd(exe, "lvcreate -L 4M -n lv_a ring_vg \"" + pv.string() + "\"", status);
  CHECK_MSG(status == 0, out);

  int wrap_count = 0;
  int ring_revolutions = 0;
  std::uint64_t prev_offset = 0;
  std::string cur_name = "lv_a";
  for (int iter = 0; iter < 55; ++iter) {
    const std::string next_name = (cur_name == "lv_a") ? "lv_b" : "lv_a";
    out = run_cmd(exe, "lvrename ring_vg/" + cur_name + " " + next_name + " \"" + pv.string() + "\"", status);
    CHECK_MSG(status == 0, out);
    cur_name = next_name;

    std::string err;
    auto src = lvmfmt::open_file_source(pv.string(), err);
    CHECK_MSG(src != nullptr, err);
    auto lbl = lvmfmt::read_pv_label(*src, err);
    CHECK_MSG(lbl.has_value(), err);
    auto mda = lvmfmt::read_mda_header(*src, lbl->metadata_areas[0], err);
    CHECK_MSG(mda.has_value(), err);
    if (prev_offset > 0 && mda->commit_offset < prev_offset) {
      ring_revolutions++;
    }
    prev_offset = mda->commit_offset;
    const bool wrapped = (mda->commit_offset + mda->commit_size > mda->size);
    if (wrapped) wrap_count++;
    auto text = lvmfmt::read_vg_metadata(*src, *lbl, err);
    CHECK_MSG(text.has_value(), err);
    auto vg = lvmfmt::parse_vg_metadata(*text, err);
    CHECK_MSG(vg.has_value(), err);
    CHECK(vg->find_lv(cur_name) != nullptr);
    src.reset();

    if (wrapped) {
      out = run_cmd(exe, "pvck \"" + pv.string() + "\"", status);
      CHECK_MSG(status == 0, out);
      out = run_cmd(exe, "vgck ring_vg \"" + pv.string() + "\"", status);
      CHECK_MSG(status == 0, out);
    }
  }

  CHECK_MSG(wrap_count >= 2, "expected >= 2 CLI ring wraps, saw " + std::to_string(wrap_count));
  CHECK_MSG(ring_revolutions >= 3,
            "expected >= 3 CLI ring revolutions, saw " + std::to_string(ring_revolutions));
  out = run_cmd(exe, "pvck \"" + pv.string() + "\"", status);
  CHECK_MSG(status == 0, out);
  out = run_cmd(exe, "vgck ring_vg \"" + pv.string() + "\"", status);
  CHECK_MSG(status == 0, out);
}

TEST(lvm_control_plane_readonly_lv_and_vgexport_import) {
  TempDir tmp("perm_export");
  const fs::path exe = fs::path(WINLVM_LVM_EXE);
  const fs::path pv = tmp.path / "perm_pv.img";
  create_blank_file(pv, 32 * 1024 * 1024);

  int status = -1;
  CHECK_EQ(run_cmd(exe, "pvcreate \"" + pv.string() + "\"", status).empty(), false);
  CHECK_EQ(status, 0);
  CHECK_EQ(run_cmd(exe, "vgcreate -s 4M perm_vg \"" + pv.string() + "\"", status).empty(), false);
  CHECK_EQ(status, 0);
  CHECK_EQ(run_cmd(exe, "lvcreate -L 4M -n perm_lv perm_vg \"" + pv.string() + "\"", status).empty(), false);
  CHECK_EQ(status, 0);

  // Set LV read-only via lvchange -p r and verify persistence.
  std::string out = run_cmd(exe, "lvchange -p r perm_vg/perm_lv \"" + pv.string() + "\"", status);
  CHECK_MSG(status == 0, out);
  out = run_cmd(exe, "lvdisplay \"" + pv.string() + "\"", status);
  CHECK_EQ(status, 0);
  CHECK(out.find("read only") != std::string::npos);

  {
    std::map<std::string, fs::path> dummy;
    auto vg = load_vg_and_map_pvs({pv}, dummy);
    const auto* lv = vg.find_lv("perm_lv");
    CHECK(lv != nullptr);
    CHECK(lv->is_read_only());
  }

  // Restore read-write permission.
  out = run_cmd(exe, "lvchange -p rw perm_vg/perm_lv \"" + pv.string() + "\"", status);
  CHECK_MSG(status == 0, out);
  {
    std::map<std::string, fs::path> dummy;
    auto vg = load_vg_and_map_pvs({pv}, dummy);
    CHECK(!vg.find_lv("perm_lv")->is_read_only());
  }

  // vgexport -> mutating commands refused without altering disk -> vgimport restores mutability.
  out = run_cmd(exe, "vgexport perm_vg \"" + pv.string() + "\"", status);
  CHECK_MSG(status == 0, out);
  {
    std::map<std::string, fs::path> dummy;
    auto vg = load_vg_and_map_pvs({pv}, dummy);
    CHECK(vg.is_exported());
  }
  const std::string exported_sha = hash_file(pv);
  run_cmd(exe, "lvcreate -L 4M -n blocked_lv perm_vg \"" + pv.string() + "\"", status);
  CHECK(status != 0);
  CHECK_EQ(hash_file(pv), exported_sha);

  out = run_cmd(exe, "vgimport perm_vg \"" + pv.string() + "\"", status);
  CHECK_MSG(status == 0, out);
  {
    std::map<std::string, fs::path> dummy;
    auto vg = load_vg_and_map_pvs({pv}, dummy);
    CHECK(!vg.is_exported());
  }
  out = run_cmd(exe, "lvcreate -L 4M -n after_import perm_vg \"" + pv.string() + "\"", status);
  CHECK_MSG(status == 0, out);
  out = run_cmd(exe, "vgck perm_vg \"" + pv.string() + "\"", status);
  CHECK_MSG(status == 0, out);
}

TEST(lvm_control_plane_cross_boundary_payload_integrity) {
  TempDir tmp("xbound");
  const fs::path exe = fs::path(WINLVM_LVM_EXE);

  // Part 1: 2-PV striped LV (-i 2 -I 64k) across lvextend, lvresize, lvreduce.
  {
    const fs::path s0 = tmp.path / "str_pv0.img";
    const fs::path s1 = tmp.path / "str_pv1.img";
    create_blank_file(s0, 32 * 1024 * 1024);
    create_blank_file(s1, 32 * 1024 * 1024);
    const std::string pvs = "\"" + s0.string() + "\" \"" + s1.string() + "\"";

    int st = -1;
    std::string out = run_cmd(exe, "pvcreate " + pvs, st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "vgcreate -s 4M str_vg " + pvs, st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "lvcreate -L 8M -i 2 -I 64k -n lv_s str_vg " + pvs, st);
    CHECK_MSG(st == 0, out);

    constexpr std::uint64_t kSectors8M = (8ULL * 1024ULL * 1024ULL) / 512ULL;
    constexpr std::uint64_t kTagS = 0x51525354ULL;
    write_lv_pattern_range({s0, s1}, "lv_s", 0, kSectors8M, kTagS);
    const std::string exp_8m = expected_pattern_sha256(0, kSectors8M, kTagS);
    CHECK_EQ(hash_lv_mapped_range({s0, s1}, "lv_s", 0, kSectors8M * 512ULL), exp_8m);

    // Striped lvextend +8M (total 16M)
    out = run_cmd(exe, "lvextend -L +8M str_vg/lv_s " + pvs, st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "vgck str_vg " + pvs, st);
    CHECK_MSG(st == 0, out);
    CHECK_EQ(hash_lv_mapped_range({s0, s1}, "lv_s", 0, kSectors8M * 512ULL), exp_8m);

    // Populate newly extended [8M, 16M) across 64 KiB stripe boundaries.
    write_lv_pattern_range({s0, s1}, "lv_s", kSectors8M, kSectors8M, kTagS);
    const std::string exp_16m = expected_pattern_sha256(0, kSectors8M * 2ULL, kTagS);
    CHECK_EQ(hash_lv_mapped_range({s0, s1}, "lv_s", 0, kSectors8M * 2ULL * 512ULL), exp_16m);

    // Striped lvresize +8M (total 24M), lvreduce -8M (16M), lvresize to 8M.
    out = run_cmd(exe, "lvresize -L +8M str_vg/lv_s " + pvs, st);
    CHECK_MSG(st == 0, out);
    CHECK_EQ(hash_lv_mapped_range({s0, s1}, "lv_s", 0, kSectors8M * 2ULL * 512ULL), exp_16m);

    out = run_cmd(exe, "lvreduce -L -8M str_vg/lv_s " + pvs, st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "vgck str_vg " + pvs, st);
    CHECK_MSG(st == 0, out);
    CHECK_EQ(hash_lv_mapped_range({s0, s1}, "lv_s", 0, kSectors8M * 2ULL * 512ULL), exp_16m);

    out = run_cmd(exe, "lvresize -L 8M str_vg/lv_s " + pvs, st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "vgck str_vg " + pvs, st);
    CHECK_MSG(st == 0, out);
    CHECK_EQ(hash_lv_mapped_range({s0, s1}, "lv_s", 0, kSectors8M * 512ULL), exp_8m);
  }

  // Part 2: 2-PV linear LV crossing PV0->PV1 boundary + lvextend, lvreduce, lvresize, pvmove, vgreduce, pvresize.
  {
    const fs::path l0 = tmp.path / "lin_pv0.img";
    const fs::path l1 = tmp.path / "lin_pv1.img";
    create_blank_file(l0, 16 * 1024 * 1024);  // 3 extents (12 MiB) usable
    create_blank_file(l1, 32 * 1024 * 1024);  // 7 extents (28 MiB) usable
    const std::string pvs = "\"" + l0.string() + "\" \"" + l1.string() + "\"";

    int st = -1;
    std::string out = run_cmd(exe, "pvcreate " + pvs, st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "vgcreate -s 4M lin_vg " + pvs, st);
    CHECK_MSG(st == 0, out);
    // 20 MiB = 5 extents: 3 extents on l0 (12 MiB) + 2 extents on l1 (8 MiB), spanning PV boundary!
    out = run_cmd(exe, "lvcreate -L 20M -n lv_l lin_vg " + pvs, st);
    CHECK_MSG(st == 0, out);

    constexpr std::uint64_t kSectors16M = (16ULL * 1024ULL * 1024ULL) / 512ULL;
    constexpr std::uint64_t kSectors20M = (20ULL * 1024ULL * 1024ULL) / 512ULL;
    constexpr std::uint64_t kTagL = 0xA1B2C3D4ULL;
    write_lv_pattern_range({l0, l1}, "lv_l", 0, kSectors20M, kTagL);
    const std::string exp_16m = expected_pattern_sha256(0, kSectors16M, kTagL);
    const std::string exp_20m = expected_pattern_sha256(0, kSectors20M, kTagL);
    CHECK_EQ(hash_lv_mapped_range({l0, l1}, "lv_l", 0, kSectors20M * 512ULL), exp_20m);

    // lvextend +4M -> lvreduce -4M -> lvresize to 16M (still spans l0 + l1).
    out = run_cmd(exe, "lvextend -L +4M lin_vg/lv_l " + pvs, st);
    CHECK_MSG(st == 0, out);
    CHECK_EQ(hash_lv_mapped_range({l0, l1}, "lv_l", 0, kSectors20M * 512ULL), exp_20m);

    out = run_cmd(exe, "lvreduce -L -4M lin_vg/lv_l " + pvs, st);
    CHECK_MSG(st == 0, out);
    CHECK_EQ(hash_lv_mapped_range({l0, l1}, "lv_l", 0, kSectors20M * 512ULL), exp_20m);

    out = run_cmd(exe, "lvresize -L 16M lin_vg/lv_l " + pvs, st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "vgck lin_vg " + pvs, st);
    CHECK_MSG(st == 0, out);
    CHECK_EQ(hash_lv_mapped_range({l0, l1}, "lv_l", 0, kSectors16M * 512ULL), exp_16m);

    // pvmove all extents from l0 to l1 and confirm every surviving byte matches SHA-256.
    out = run_cmd(exe, "pvmove " + pvs, st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "vgck lin_vg " + pvs, st);
    CHECK_MSG(st == 0, out);
    CHECK_EQ(hash_lv_mapped_range({l0, l1}, "lv_l", 0, kSectors16M * 512ULL), exp_16m);

    // vgreduce empty l0 from lin_vg -> l0 becomes orphan, lv_l on l1 stays bit-identical.
    out = run_cmd(exe, "vgreduce lin_vg " + pvs, st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "vgck lin_vg \"" + l1.string() + "\"", st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "pvck \"" + l0.string() + "\"", st);
    CHECK_MSG(st == 0, out);
    {
      std::string err;
      auto s0 = lvmfmt::open_file_source(l0.string(), err);
      CHECK_MSG(s0 != nullptr, err);
      auto lbl0 = lvmfmt::read_pv_label(*s0, err);
      CHECK_MSG(lbl0.has_value(), err);
      CHECK(!lvmfmt::read_vg_metadata(*s0, *lbl0, err).has_value());
    }
    CHECK_EQ(hash_lv_mapped_range({l1}, "lv_l", 0, kSectors16M * 512ULL), exp_16m);

    // Grow backing file of l1 to 48 MiB -> pvresize -> shrink to 40 MiB -> lvextend +4M.
    fs::resize_file(l1, 48ULL * 1024ULL * 1024ULL);
    out = run_cmd(exe, "pvresize \"" + l1.string() + "\"", st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "pvck \"" + l1.string() + "\"", st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "vgck lin_vg \"" + l1.string() + "\"", st);
    CHECK_MSG(st == 0, out);
    {
      std::map<std::string, fs::path> dummy;
      auto vg = load_vg_and_map_pvs({l1}, dummy);
      CHECK_EQ(vg.pvs.size(), std::size_t{1});
      CHECK_EQ(vg.pvs[0].pe_count, std::uint64_t{11});  // (48 - 1) / 4 == 11
    }
    CHECK_EQ(hash_lv_mapped_range({l1}, "lv_l", 0, kSectors16M * 512ULL), exp_16m);

    out = run_cmd(exe, "pvresize --setphysicalvolumesize 40M \"" + l1.string() + "\"", st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "vgck lin_vg \"" + l1.string() + "\"", st);
    CHECK_MSG(st == 0, out);
    {
      std::map<std::string, fs::path> dummy;
      auto vg = load_vg_and_map_pvs({l1}, dummy);
      CHECK_EQ(vg.pvs[0].pe_count, std::uint64_t{9});  // (40 - 1) / 4 == 9
    }
    out = run_cmd(exe, "lvextend -L +4M lin_vg/lv_l \"" + l1.string() + "\"", st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "vgck lin_vg \"" + l1.string() + "\"", st);
    CHECK_MSG(st == 0, out);
    CHECK_EQ(hash_lv_mapped_range({l1}, "lv_l", 0, kSectors16M * 512ULL), exp_16m);
  }
}
