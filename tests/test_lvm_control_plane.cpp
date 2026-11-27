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

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "lvmfmt/allocator.h"
#include "lvmfmt/bytes.h"
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
#ifdef _WIN32
    const auto pid = static_cast<unsigned long>(_getpid());
#else
    const auto pid = static_cast<unsigned long>(getpid());
#endif
    path = fs::temp_directory_path() / ("winlvm_ctrl_" + tag + "_" + std::to_string(pid));
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
  TempDir tmp("pv_life");
  const fs::path exe = fs::path(WINLVM_LVM_EXE);
  const fs::path pv_file = tmp.path / "test_ctrl_pv1.img";
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
  TempDir tmp("vg_lv_life");
  const fs::path exe = fs::path(WINLVM_LVM_EXE);
  const fs::path pv1 = tmp.path / "test_ctrl_vg_pv1.img";
  const fs::path pv2 = tmp.path / "test_ctrl_vg_pv2.img";
  const fs::path backup_file = tmp.path / "test_ctrl_backup.vg";

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
  TempDir tmp("pvmove_life");
  const fs::path exe = fs::path(WINLVM_LVM_EXE);
  const fs::path pv_a = tmp.path / "test_ctrl_move_a.img";
  const fs::path pv_b = tmp.path / "test_ctrl_move_b.img";

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

  // Confirm original lvodd stream and all oracle windows still match generated odd-geometry/lv-hashes.txt.
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

namespace {

std::uint32_t test_dm_bm_checksum(const std::uint8_t* block, std::uint32_t xor_salt) {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 4; i < 4096u; ++i) {
    crc ^= block[i];
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 1u) ? ((crc >> 1) ^ 0x82f63b78u) : (crc >> 1);
    }
  }
  return crc ^ xor_salt;
}

std::uint64_t linear_lv_first_pv_offset(const lvmfmt::VolumeGroup& vg,
                                        const std::string& lv_name,
                                        const std::map<std::string, fs::path>& uuid_to_path,
                                        fs::path& out_pv_path) {
  const auto* lv = vg.find_lv(lv_name);
  CHECK(lv != nullptr);
  CHECK(!lv->segments.empty());
  CHECK(!lv->segments[0].stripes.empty());
  const auto& st = lv->segments[0].stripes[0];
  const auto* pv = vg.find_pv(st.target);
  CHECK(pv != nullptr);
  auto it = uuid_to_path.find(normalize_raw_uuid(pv->id));
  CHECK(it != uuid_to_path.end());
  out_pv_path = it->second;
  return (pv->pe_start_sectors * 512ULL) +
         (st.start_extent * vg.extent_size_sectors * 512ULL);
}

std::string hash_file_slice(const fs::path& path, std::uint64_t offset, std::uint64_t length) {
  std::ifstream in(path, std::ios::binary);
  CHECK(in.is_open());
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  test_sha256_ctx ctx;
  test_sha256_init(&ctx);
  std::vector<char> buf(65536);
  std::uint64_t rem = length;
  while (rem > 0) {
    const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(rem, buf.size()));
    in.read(buf.data(), static_cast<std::streamsize>(take));
    CHECK_EQ(static_cast<std::size_t>(in.gcount()), take);
    test_sha256_update(&ctx, buf.data(), take);
    rem -= take;
  }
  char hex[65] = {};
  test_sha256_final_hex(&ctx, hex);
  return std::string(hex);
}

}  // namespace

TEST(lvm_control_plane_pvchange_and_tags_lifecycle) {
  TempDir tmp("pvchange_tags");
  const fs::path exe = fs::path(WINLVM_LVM_EXE);
  const fs::path pv0 = tmp.path / "pv0.img";
  const fs::path pv1 = tmp.path / "pv1.img";
  const fs::path orphan = tmp.path / "orphan.img";
  create_blank_file(pv0, 32 * 1024 * 1024);
  create_blank_file(pv1, 32 * 1024 * 1024);
  create_blank_file(orphan, 16 * 1024 * 1024);

  const std::string pvs = "\"" + pv0.string() + "\" \"" + pv1.string() + "\"";
  int st = -1;

  // pvcreate rejects both invalid tags and orphan PV tags (tags require VG membership).
  run_cmd(exe, "pvcreate --addtag \"bad tag!\" \"" + orphan.string() + "\"", st);
  CHECK(st != 0);
  run_cmd(exe, "pvcreate --addtag valid_tag \"" + orphan.string() + "\"", st);
  CHECK(st != 0);
  std::string out = run_cmd(exe, "pvcreate \"" + orphan.string() + "\"", st);
  CHECK_MSG(st == 0, out);

  // Orphan pvchange -u regenerates LABELONE UUID cleanly.
  std::string old_orphan_uuid;
  {
    std::string err;
    auto src = lvmfmt::open_file_source(orphan.string(), err);
    CHECK(src != nullptr);
    auto lbl = lvmfmt::read_pv_label(*src, err);
    CHECK(lbl.has_value());
    old_orphan_uuid = lbl->pv_uuid;
  }
  out = run_cmd(exe, "pvchange -u \"" + orphan.string() + "\"", st);
  CHECK_MSG(st == 0, out);
  {
    std::string err;
    auto src = lvmfmt::open_file_source(orphan.string(), err);
    CHECK(src != nullptr);
    auto lbl = lvmfmt::read_pv_label(*src, err);
    CHECK(lbl.has_value());
    CHECK(lbl->pv_uuid != old_orphan_uuid);
  }
  out = run_cmd(exe, "pvck \"" + orphan.string() + "\"", st);
  CHECK_MSG(st == 0, out);

  // Create 2-PV VG with tags, test pvchange -x, -u, --addtag/--deltag, -a.
  out = run_cmd(exe, "pvcreate " + pvs, st);
  CHECK_MSG(st == 0, out);
  out = run_cmd(exe, "vgcreate --addtag vgtag1 --addtag vgtag2 -s 4M tagvg " + pvs, st);
  CHECK_MSG(st == 0, out);

  // vgchange tag mutations
  out = run_cmd(exe, "vgchange --addtag vgtag3 --deltag vgtag1 tagvg " + pvs, st);
  CHECK_MSG(st == 0, out);
  {
    std::map<std::string, fs::path> dummy;
    auto vg = load_vg_and_map_pvs({pv0, pv1}, dummy);
    CHECK_EQ(vg.tags.size(), std::size_t{2});
    CHECK(std::find(vg.tags.begin(), vg.tags.end(), "vgtag2") != vg.tags.end());
    CHECK(std::find(vg.tags.begin(), vg.tags.end(), "vgtag3") != vg.tags.end());
  }

  // pvchange -x n on pv0 disables allocation on pv0 (persists status = []).
  out = run_cmd(exe, "pvchange -x n \"" + pv0.string() + "\" \"" + pv1.string() + "\"", st);
  CHECK_MSG(st == 0, out);
  {
    std::map<std::string, fs::path> dummy;
    auto vg = load_vg_and_map_pvs({pv0, pv1}, dummy);
    CHECK(!vg.pvs[0].is_allocatable());
    CHECK(vg.pvs[1].is_allocatable());
  }

  // Creating a 2-stripe LV must fail while pv0 is non-allocatable.
  run_cmd(exe, "lvcreate -L 8M -i 2 -n blocked_str tagvg " + pvs, st);
  CHECK(st != 0);

  // Re-enable allocation on all PVs via -a -x y, add PV tags to pv0, rotate pv1 UUID.
  out = run_cmd(exe, "pvchange -a -x y " + pvs, st);
  CHECK_MSG(st == 0, out);
  out = run_cmd(exe, "pvchange --addtag pvtag1 --addtag pvtag2 \"" + pv0.string() + "\" \"" +
                     pv1.string() + "\"", st);
  CHECK_MSG(st == 0, out);
  out = run_cmd(exe, "pvchange --deltag pvtag1 \"" + pv0.string() + "\" \"" + pv1.string() + "\"", st);
  CHECK_MSG(st == 0, out);

  std::string old_pv1_uuid;
  {
    std::string err;
    auto src = lvmfmt::open_file_source(pv1.string(), err);
    auto lbl = lvmfmt::read_pv_label(*src, err);
    CHECK(lbl.has_value());
    old_pv1_uuid = lbl->pv_uuid;
  }
  out = run_cmd(exe, "pvchange -u \"" + pv1.string() + "\" \"" + pv0.string() + "\"", st);
  CHECK_MSG(st == 0, out);
  {
    std::string err;
    auto src = lvmfmt::open_file_source(pv1.string(), err);
    auto lbl = lvmfmt::read_pv_label(*src, err);
    CHECK(lbl.has_value());
    CHECK(lbl->pv_uuid != old_pv1_uuid);
    std::map<std::string, fs::path> dummy;
    auto vg = load_vg_and_map_pvs({pv0, pv1}, dummy);
    CHECK(vg.find_pv_by_uuid(lbl->pv_uuid) != nullptr);
    CHECK(vg.find_pv_by_uuid(old_pv1_uuid) == nullptr);
  }

  // Create tagged LVs and mutate LV tags.
  out = run_cmd(exe, "lvcreate --addtag lvtag1 -L 4M -n lv1 tagvg " + pvs, st);
  CHECK_MSG(st == 0, out);
  out = run_cmd(exe, "lvcreate --addtag other_lv -L 4M -n lv2 tagvg " + pvs, st);
  CHECK_MSG(st == 0, out);
  out = run_cmd(exe, "lvchange --addtag lvtag2 --deltag lvtag1 tagvg/lv1 " + pvs, st);
  CHECK_MSG(st == 0, out);

  // Verify `lvm tags` lists every unique tag across PV/VG/LV.
  out = run_cmd(exe, "tags " + pvs, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("pvtag2") != std::string::npos);
  CHECK(out.find("vgtag2") != std::string::npos);
  CHECK(out.find("vgtag3") != std::string::npos);
  CHECK(out.find("lvtag2") != std::string::npos);
  CHECK(out.find("other_lv") != std::string::npos);
  CHECK(out.find("pvtag1") == std::string::npos);
  CHECK(out.find("vgtag1") == std::string::npos);
  CHECK(out.find("lvtag1") == std::string::npos);

  // Verify @tag filtering and -o +tags across pvs / vgs / lvs.
  out = run_cmd(exe, "pvs -o +tags @pvtag2 " + pvs, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find(pv0.string()) != std::string::npos);
  CHECK(out.find(pv1.string()) == std::string::npos);
  CHECK(out.find("pvtag2") != std::string::npos);

  out = run_cmd(exe, "vgs -o +tags @vgtag3 " + pvs, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("tagvg") != std::string::npos);
  CHECK(out.find("vgtag3") != std::string::npos);
  out = run_cmd(exe, "vgs --noheadings @nonexistent_tag " + pvs, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("tagvg") == std::string::npos);

  out = run_cmd(exe, "lvs -o +tags @lvtag2 " + pvs, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("lv1") != std::string::npos);
  CHECK(out.find("lv2") == std::string::npos);

  // Verify JSON report includes tags cleanly.
  out = run_cmd(exe, "lvs --reportformat json_std @lvtag2 " + pvs, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("\"lv_tags\": [\"lvtag2\"]") != std::string::npos);

  out = run_cmd(exe, "pvck " + pvs, st);
  CHECK_MSG(st == 0, out);
  out = run_cmd(exe, "vgck tagvg " + pvs, st);
  CHECK_MSG(st == 0, out);
}

TEST(lvm_control_plane_thin_pool_and_thin_lv_lifecycle) {
  TempDir tmp("thin_life");
  const fs::path exe = fs::path(WINLVM_LVM_EXE);
  const fs::path pv0 = tmp.path / "pv0.img";
  create_blank_file(pv0, 48 * 1024 * 1024);
  const std::string q = "\"" + pv0.string() + "\"";

  int st = -1;
  std::string out = run_cmd(exe, "pvcreate " + q, st);
  CHECK_MSG(st == 0, out);
  out = run_cmd(exe, "vgcreate -s 4M thinvg " + q, st);
  CHECK_MSG(st == 0, out);

  // Create thin-pool via --type thin-pool AND another via -T -L <size> <vg>/<pool>.
  out = run_cmd(exe, "lvcreate --type thin-pool -L 16M --poolmetadatasize 4M -n tpool thinvg " + q, st);
  CHECK_MSG(st == 0, out);
  out = run_cmd(exe, "lvcreate -L 8M -T thinvg/tpool2 " + q, st);
  CHECK_MSG(st == 0, out);

  // Create two thin volumes via both -V -T and --type thin --virtualsize --thinpool syntaxes.
  out = run_cmd(exe, "lvcreate -V 8M -T thinvg/tpool -n lvthin1 " + q, st);
  CHECK_MSG(st == 0, out);
  out = run_cmd(exe, "lvcreate --type thin --virtualsize 12M --thinpool tpool -n lvthin2 thinvg " + q, st);
  CHECK_MSG(st == 0, out);

  // Verify raw dm-thin v2 superblock, B-tree roots, and space maps on tpool_tmeta.
  std::map<std::string, fs::path> u2p;
  auto vg = load_vg_and_map_pvs({pv0}, u2p);
  const auto* pool = vg.find_lv("tpool");
  const auto* t1 = vg.find_lv("lvthin1");
  const auto* t2 = vg.find_lv("lvthin2");
  CHECK(pool != nullptr && t1 != nullptr && t2 != nullptr);
  CHECK_EQ(pool->segments[0].type, std::string("thin-pool"));
  CHECK_EQ(pool->segments[0].transaction_id, std::uint64_t{2});
  CHECK_EQ(t1->segments[0].device_id, std::uint64_t{1});
  CHECK_EQ(t2->segments[0].device_id, std::uint64_t{2});

  fs::path tmeta_pv_path;
  const std::uint64_t tmeta_off = linear_lv_first_pv_offset(vg, "tpool_tmeta", u2p, tmeta_pv_path);
  std::ifstream mf(tmeta_pv_path, std::ios::binary);
  CHECK(mf.is_open());

  std::vector<std::uint8_t> blk0(4096);
  mf.seekg(static_cast<std::streamoff>(tmeta_off), std::ios::beg);
  mf.read(reinterpret_cast<char*>(blk0.data()), 4096);
  CHECK_EQ(lvmfmt::le32(blk0.data()), test_dm_bm_checksum(blk0.data(), 160774u));
  CHECK_EQ(lvmfmt::le64(blk0.data() + 32), std::uint64_t{27022010ULL});
  CHECK_EQ(lvmfmt::le32(blk0.data() + 40), std::uint32_t{2u});
  CHECK_EQ(lvmfmt::le64(blk0.data() + 48), std::uint64_t{2ULL});  // trans_id
  CHECK_EQ(lvmfmt::le32(blk0.data() + 336), std::uint32_t{128u});  // 64 KiB chunk

  const std::uint64_t map_root = lvmfmt::le64(blk0.data() + 320);
  const std::uint64_t det_root = lvmfmt::le64(blk0.data() + 328);

  std::vector<std::uint8_t> map_blk(4096), det_blk(4096);
  mf.seekg(static_cast<std::streamoff>(tmeta_off + map_root * 4096ULL), std::ios::beg);
  mf.read(reinterpret_cast<char*>(map_blk.data()), 4096);
  CHECK_EQ(lvmfmt::le32(map_blk.data()), test_dm_bm_checksum(map_blk.data(), 121107u));
  CHECK_EQ(lvmfmt::le32(map_blk.data() + 16), std::uint32_t{2u});  // 2 thin devices

  mf.seekg(static_cast<std::streamoff>(tmeta_off + det_root * 4096ULL), std::ios::beg);
  mf.read(reinterpret_cast<char*>(det_blk.data()), 4096);
  CHECK_EQ(lvmfmt::le32(det_blk.data()), test_dm_bm_checksum(det_blk.data(), 121107u));
  CHECK_EQ(lvmfmt::le32(det_blk.data() + 16), std::uint32_t{2u});

  out = run_cmd(exe, "lvs --reportformat json " + q, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("\"segtype\": \"thin-pool\"") != std::string::npos);
  CHECK(out.find("\"segtype\": \"thin\"") != std::string::npos);
  CHECK(out.find("\"pool_lv\": \"tpool\"") != std::string::npos);

  out = run_cmd(exe, "vgck thinvg " + q, st);
  CHECK_MSG(st == 0, out);
}

TEST(lvm_control_plane_snapshot_create_and_offline_merge) {
  TempDir tmp("snap_merge");
  const fs::path exe = fs::path(WINLVM_LVM_EXE);
  const fs::path pv0 = tmp.path / "pv0.img";
  create_blank_file(pv0, 32 * 1024 * 1024);
  const std::string q = "\"" + pv0.string() + "\"";

  int st = -1;
  CHECK_MSG((run_cmd(exe, "pvcreate " + q, st), st == 0), "pvcreate");
  CHECK_MSG((run_cmd(exe, "vgcreate -s 4M snapvg " + q, st), st == 0), "vgcreate");
  CHECK_MSG((run_cmd(exe, "lvcreate -L 8M -n lvorig snapvg " + q, st), st == 0), "lvcreate");

  // Populate 8 chunks (32 KiB) of lvorig with deterministic pattern tag 0x1111.
  write_lv_pattern_range({pv0}, "lvorig", 0, 64, 0x1111ULL);
  const std::string orig_c1_sha = expected_pattern_sha256(8, 8, 0x1111ULL);
  const std::string orig_c2_sha = expected_pattern_sha256(16, 8, 0x1111ULL);

  // Create snapshot lvsnap (4M COW, default 4 KiB chunks).
  std::string out = run_cmd(exe, "lvcreate -s -L 4M -n lvsnap snapvg/lvorig " + q, st);
  CHECK_MSG(st == 0, out);

  // Verify COW header on disk and JSON lvs representation.
  std::map<std::string, fs::path> u2p;
  auto vg = load_vg_and_map_pvs({pv0}, u2p);
  fs::path cow_pv_path;
  const std::uint64_t cow_off = linear_lv_first_pv_offset(vg, "lvsnap", u2p, cow_pv_path);
  {
    std::ifstream cf(cow_pv_path, std::ios::binary);
    std::uint8_t hdr[16]{};
    cf.seekg(static_cast<std::streamoff>(cow_off), std::ios::beg);
    cf.read(reinterpret_cast<char*>(hdr), 16);
    CHECK_EQ(lvmfmt::le32(hdr + 0), std::uint32_t{0x70416e53u});
    CHECK_EQ(lvmfmt::le32(hdr + 4), std::uint32_t{1u});
    CHECK_EQ(lvmfmt::le32(hdr + 8), std::uint32_t{1u});
    CHECK_EQ(lvmfmt::le32(hdr + 12), std::uint32_t{8u});
  }

  out = run_cmd(exe, "lvs --reportformat json " + q, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("\"lv_name\": \"lvsnap\"") != std::string::npos);
  CHECK(out.find("\"origin\": \"lvorig\"") != std::string::npos);

  // Populate two COW exceptions in area 0: old_chunk 0 -> new_chunk 2, old_chunk 3 -> new_chunk 3.
  const std::string snap_c0_sha = expected_pattern_sha256(0, 8, 0x9999ULL);
  const std::string snap_c3_sha = expected_pattern_sha256(24, 8, 0x8888ULL);
  {
    std::fstream cf(cow_pv_path, std::ios::in | std::ios::out | std::ios::binary);
    CHECK(cf.is_open());
    std::uint8_t area0[32]{};
    const std::uint64_t pairs[4] = {0ULL, 2ULL, 3ULL, 3ULL};
    for (int i = 0; i < 4; ++i) {
      for (int b = 0; b < 8; ++b) {
        area0[i * 8 + b] = static_cast<std::uint8_t>((pairs[i] >> (b * 8)) & 0xffu);
      }
    }
    cf.seekp(static_cast<std::streamoff>(cow_off + 4096ULL), std::ios::beg);
    cf.write(reinterpret_cast<const char*>(area0), sizeof(area0));

    std::uint8_t sec[512];
    cf.seekp(static_cast<std::streamoff>(cow_off + 2ULL * 4096ULL), std::ios::beg);
    for (std::uint64_t s = 0; s < 8; ++s) {
      make_sector_pattern(0x9999ULL, s, sec);
      cf.write(reinterpret_cast<const char*>(sec), 512);
    }
    cf.seekp(static_cast<std::streamoff>(cow_off + 3ULL * 4096ULL), std::ios::beg);
    for (std::uint64_t s = 24; s < 32; ++s) {
      make_sector_pattern(0x8888ULL, s, sec);
      cf.write(reinterpret_cast<const char*>(sec), 512);
    }
    cf.flush();
  }

  // Merge snapshot back into origin.
  out = run_cmd(exe, "lvconvert --merge snapvg/lvsnap " + q, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("2 exception chunk(s) applied") != std::string::npos);

  // Verify merged chunks 0 and 3 match snapshot payloads, chunks 1 and 2 unchanged, snapshot removed.
  CHECK_EQ(hash_lv_mapped_range({pv0}, "lvorig", 0, 4096), snap_c0_sha);
  CHECK_EQ(hash_lv_mapped_range({pv0}, "lvorig", 4096, 4096), orig_c1_sha);
  CHECK_EQ(hash_lv_mapped_range({pv0}, "lvorig", 8192, 4096), orig_c2_sha);
  CHECK_EQ(hash_lv_mapped_range({pv0}, "lvorig", 12288, 4096), snap_c3_sha);

  auto vg_after = load_vg_and_map_pvs({pv0}, u2p);
  CHECK(vg_after.find_lv("lvsnap") == nullptr);
  CHECK(vg_after.find_lv("snapshot0") == nullptr);
  CHECK(vg_after.find_lv("lvorig") != nullptr);
  out = run_cmd(exe, "vgck snapvg " + q, st);
  CHECK_MSG(st == 0, out);
}

TEST(lvm_control_plane_raid1_and_mirror_convert_roundtrip) {
  TempDir tmp("raid_mirror");
  const fs::path exe = fs::path(WINLVM_LVM_EXE);
  const fs::path pv0 = tmp.path / "pv0.img";
  const fs::path pv1 = tmp.path / "pv1.img";
  create_blank_file(pv0, 32 * 1024 * 1024);
  create_blank_file(pv1, 32 * 1024 * 1024);
  const std::string pvs = "\"" + pv0.string() + "\" \"" + pv1.string() + "\"";

  int st = -1;
  CHECK_MSG((run_cmd(exe, "pvcreate " + pvs, st), st == 0), "pvcreate");
  CHECK_MSG((run_cmd(exe, "vgcreate -s 4M rmvg " + pvs, st), st == 0), "vgcreate");
  CHECK_MSG((run_cmd(exe, "lvcreate -L 4M -n lvraid rmvg " + pvs, st), st == 0), "lvcreate raid");
  CHECK_MSG((run_cmd(exe, "lvcreate -L 4M -n lvmir rmvg " + pvs, st), st == 0), "lvcreate mir");

  constexpr std::uint64_t kSectors4M = (4ULL * 1024ULL * 1024ULL) / 512ULL;
  write_lv_pattern_range({pv0, pv1}, "lvraid", 0, kSectors4M, 0x52414944ULL);
  write_lv_pattern_range({pv0, pv1}, "lvmir", 0, kSectors4M, 0x4d495252ULL);
  const std::string exp_raid_sha = expected_pattern_sha256(0, kSectors4M, 0x52414944ULL);
  const std::string exp_mir_sha = expected_pattern_sha256(0, kSectors4M, 0x4d495252ULL);

  // Convert lvraid -> 2-leg raid1 and verify both legs + DmRd superblocks + JSON.
  std::string out = run_cmd(exe, "lvconvert --type raid1 -m 1 rmvg/lvraid " + pvs, st);
  CHECK_MSG(st == 0, out);

  std::map<std::string, fs::path> u2p;
  auto vg = load_vg_and_map_pvs({pv0, pv1}, u2p);
  const auto* rlv = vg.find_lv("lvraid");
  CHECK(rlv != nullptr);
  CHECK_EQ(rlv->segments[0].type, std::string("raid1"));
  CHECK_EQ(rlv->segments[0].device_count, std::uint64_t{2});
  CHECK(vg.find_lv("lvraid_rmeta_0") != nullptr && vg.find_lv("lvraid_rmeta_1") != nullptr);
  CHECK(vg.find_lv("lvraid_rimage_0") != nullptr && vg.find_lv("lvraid_rimage_1") != nullptr);

  fs::path leg0_pv, leg1_pv, rm0_pv, rm1_pv;
  const std::uint64_t leg0_off = linear_lv_first_pv_offset(vg, "lvraid_rimage_0", u2p, leg0_pv);
  const std::uint64_t leg1_off = linear_lv_first_pv_offset(vg, "lvraid_rimage_1", u2p, leg1_pv);
  CHECK(leg0_pv != leg1_pv);
  CHECK_EQ(hash_file_slice(leg0_pv, leg0_off, 4ULL * 1024ULL * 1024ULL), exp_raid_sha);
  CHECK_EQ(hash_file_slice(leg1_pv, leg1_off, 4ULL * 1024ULL * 1024ULL), exp_raid_sha);

  const std::uint64_t rm0_off = linear_lv_first_pv_offset(vg, "lvraid_rmeta_0", u2p, rm0_pv);
  const std::uint64_t rm1_off = linear_lv_first_pv_offset(vg, "lvraid_rmeta_1", u2p, rm1_pv);
  auto check_rmeta = [](const fs::path& p, std::uint64_t off, std::uint32_t expected_pos) {
    std::ifstream f(p, std::ios::binary);
    std::vector<std::uint8_t> buf(8192);
    f.seekg(static_cast<std::streamoff>(off), std::ios::beg);
    f.read(reinterpret_cast<char*>(buf.data()), 8192);
    CHECK_EQ(lvmfmt::le32(buf.data() + 0x00), std::uint32_t{0x64526d44u});
    CHECK_EQ(lvmfmt::le32(buf.data() + 0x08), std::uint32_t{2u});
    CHECK_EQ(lvmfmt::le32(buf.data() + 0x0c), expected_pos);
    CHECK_EQ(lvmfmt::le64(buf.data() + 0x20), UINT64_MAX);
    CHECK_EQ(lvmfmt::le64(buf.data() + 0x28), UINT64_MAX);
    CHECK_EQ(lvmfmt::le32(buf.data() + 0x30), std::uint32_t{1u});
    CHECK_EQ(lvmfmt::le32(buf.data() + 4096 + 0x00), std::uint32_t{0x6d746962u});
  };
  check_rmeta(rm0_pv, rm0_off, 0u);
  check_rmeta(rm1_pv, rm1_off, 1u);

  // Convert lvmir -> 2-leg mirror (--mirrorlog core) and verify both legs match SHA-256.
  out = run_cmd(exe, "lvconvert --type mirror -m 1 --mirrorlog core rmvg/lvmir " + pvs, st);
  CHECK_MSG(st == 0, out);

  vg = load_vg_and_map_pvs({pv0, pv1}, u2p);
  fs::path m0_pv, m1_pv;
  const std::uint64_t m0_off = linear_lv_first_pv_offset(vg, "lvmir_mimage_0", u2p, m0_pv);
  const std::uint64_t m1_off = linear_lv_first_pv_offset(vg, "lvmir_mimage_1", u2p, m1_pv);
  CHECK(m0_pv != m1_pv);
  CHECK_EQ(hash_file_slice(m0_pv, m0_off, 4ULL * 1024ULL * 1024ULL), exp_mir_sha);
  CHECK_EQ(hash_file_slice(m1_pv, m1_off, 4ULL * 1024ULL * 1024ULL), exp_mir_sha);

  out = run_cmd(exe, "lvs --reportformat json " + pvs, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("\"segtype\": \"raid1\"") != std::string::npos);
  CHECK(out.find("\"segtype\": \"mirror\"") != std::string::npos);

  // Collapse both back to linear (-m 0 and --type linear) and verify data + freed extents.
  out = run_cmd(exe, "lvconvert -m 0 rmvg/lvraid " + pvs, st);
  CHECK_MSG(st == 0, out);
  out = run_cmd(exe, "lvconvert --type linear rmvg/lvmir " + pvs, st);
  CHECK_MSG(st == 0, out);

  vg = load_vg_and_map_pvs({pv0, pv1}, u2p);
  CHECK(vg.find_lv("lvraid_rmeta_0") == nullptr);
  CHECK(vg.find_lv("lvraid_rimage_1") == nullptr);
  CHECK(vg.find_lv("lvmir_mimage_1") == nullptr);
  CHECK_EQ(hash_lv_mapped_range({pv0, pv1}, "lvraid", 0, 4ULL * 1024ULL * 1024ULL), exp_raid_sha);
  CHECK_EQ(hash_lv_mapped_range({pv0, pv1}, "lvmir", 0, 4ULL * 1024ULL * 1024ULL), exp_mir_sha);

  out = run_cmd(exe, "vgck rmvg " + pvs, st);
  CHECK_MSG(st == 0, out);
}

TEST(lvm_control_plane_composite_safety_and_corruption_guards) {
  TempDir tmp("composite_guards");
  const fs::path exe = fs::path(WINLVM_LVM_EXE);

  auto patch_u32 = [](std::vector<std::uint8_t>& buf, std::size_t off, std::uint32_t v) {
    buf[off + 0] = static_cast<std::uint8_t>(v & 0xffu);
    buf[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xffu);
    buf[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xffu);
    buf[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xffu);
  };
  auto patch_u64 = [](std::vector<std::uint8_t>& buf, std::size_t off, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) buf[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (i * 8)) & 0xffu);
  };

  // 1. Thin-pool safety guards: resize refusal, composite rename, thin LV removal
  //    (including last thin LV), superblock state checks, and B-tree bounds validation.
  {
    const fs::path pv = tmp.path / "thin_adv.img";
    create_blank_file(pv, 48 * 1024 * 1024);
    const std::string q = "\"" + pv.string() + "\"";
    int st = -1;
    CHECK_MSG((run_cmd(exe, "pvcreate " + q, st), st == 0), "pvcreate");
    CHECK_MSG((run_cmd(exe, "vgcreate -s 4M tv " + q, st), st == 0), "vgcreate");
    CHECK_MSG((run_cmd(exe, "lvcreate --type thin-pool -L 16M --poolmetadatasize 4M -n tp tv " + q, st), st == 0), "tp");
    CHECK_MSG((run_cmd(exe, "lvcreate -V 8M -T tv/tp -n th1 " + q, st), st == 0), "th1");
    CHECK_MSG((run_cmd(exe, "lvcreate -V 8M -T tv/tp -n th2 " + q, st), st == 0), "th2");

    // lvreduce/lvresize on thin-pool and thin LV must fail closed without touching disk.
    const std::string pre_reduce_sha = hash_file(pv);
    run_cmd(exe, "lvreduce -L -4M tv/tp " + q, st);
    CHECK(st != 0);
    run_cmd(exe, "lvreduce -L -4M tv/th1 " + q, st);
    CHECK(st != 0);
    run_cmd(exe, "lvresize -L 4M tv/th1 " + q, st);
    CHECK(st != 0);
    CHECK_EQ(hash_file(pv), pre_reduce_sha);

    // lvrename on thin-pool atomically updates _tmeta/_tdata and thin LV pool references.
    std::string out = run_cmd(exe, "lvrename tv/tp tp_ren " + q, st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "vgck tv " + q, st);
    CHECK_MSG(st == 0, out);
    {
      std::map<std::string, fs::path> u2p;
      auto vg = load_vg_and_map_pvs({pv}, u2p);
      CHECK(vg.find_lv("tp_ren") != nullptr);
      CHECK(vg.find_lv("tp_ren_tmeta") != nullptr);
      CHECK(vg.find_lv("tp_ren_tdata") != nullptr);
      CHECK_EQ(vg.find_lv("th1")->segments[0].pool_lv, std::string("tp_ren"));
    }

    // lvremove on non-last and last thin LVs updates tmeta, bumps transaction_id, and leaves pool valid.
    out = run_cmd(exe, "lvremove tv/th1 " + q, st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "lvremove tv/th2 " + q, st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "vgck tv " + q, st);
    CHECK_MSG(st == 0, out);
    {
      std::map<std::string, fs::path> u2p;
      auto vg = load_vg_and_map_pvs({pv}, u2p);
      CHECK(vg.find_lv("th1") == nullptr && vg.find_lv("th2") == nullptr);
      const auto* pool = vg.find_lv("tp_ren");
      CHECK(pool != nullptr);
      CHECK(!pool->internal);
      CHECK_EQ(pool->segments[0].transaction_id, std::uint64_t{4});
    }

    // Locate tp_ren_tmeta for superblock and B-tree corruption tests.
    std::map<std::string, fs::path> u2p;
    auto vg = load_vg_and_map_pvs({pv}, u2p);
    fs::path tmeta_pv;
    const std::uint64_t tmeta_off = linear_lv_first_pv_offset(vg, "tp_ren_tmeta", u2p, tmeta_pv);

    auto read_blk = [&](std::uint64_t bnr) {
      std::vector<std::uint8_t> b(4096);
      std::ifstream f(tmeta_pv, std::ios::binary);
      f.seekg(static_cast<std::streamoff>(tmeta_off + bnr * 4096ULL), std::ios::beg);
      f.read(reinterpret_cast<char*>(b.data()), 4096);
      return b;
    };
    auto write_blk = [&](std::uint64_t bnr, const std::vector<std::uint8_t>& b) {
      std::fstream f(tmeta_pv, std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(static_cast<std::streamoff>(tmeta_off + bnr * 4096ULL), std::ios::beg);
      f.write(reinterpret_cast<const char*>(b.data()), 4096);
      f.flush();
    };

    const auto orig_sb = read_blk(0);
    const std::uint64_t det_root = lvmfmt::le64(orig_sb.data() + 328);
    const auto orig_det = read_blk(det_root);

    // NEEDS_CHECK flag set (with valid CRC) must fail closed with zero disk changes.
    {
      auto bad_sb = orig_sb;
      patch_u32(bad_sb, 4, 1u);
      patch_u32(bad_sb, 0, test_dm_bm_checksum(bad_sb.data(), 160774u));
      write_blk(0, bad_sb);
      const std::string sha = hash_file(pv);
      run_cmd(exe, "lvcreate -V 8M -T tv/tp_ren -n blocked1 " + q, st);
      CHECK(st != 0);
      CHECK_EQ(hash_file(pv), sha);
    }

    // Non-zero held_root (with valid CRC) must fail closed with zero disk changes.
    {
      auto bad_sb = orig_sb;
      patch_u64(bad_sb, 56, 99ULL);
      patch_u32(bad_sb, 0, test_dm_bm_checksum(bad_sb.data(), 160774u));
      write_blk(0, bad_sb);
      const std::string sha = hash_file(pv);
      run_cmd(exe, "lvcreate -V 8M -T tv/tp_ren -n blocked2 " + q, st);
      CHECK(st != 0);
      CHECK_EQ(hash_file(pv), sha);
    }

    // transaction_id mismatch (tmeta=42 vs VG=4) must fail closed with zero disk changes.
    {
      auto bad_sb = orig_sb;
      patch_u64(bad_sb, 48, 42ULL);
      patch_u32(bad_sb, 0, test_dm_bm_checksum(bad_sb.data(), 160774u));
      write_blk(0, bad_sb);
      const std::string sha = hash_file(pv);
      run_cmd(exe, "lvcreate -V 8M -T tv/tp_ren -n blocked3 " + q, st);
      CHECK(st != 0);
      CHECK_EQ(hash_file(pv), sha);
    }

    // Restore clean superblock, then corrupt device_details_root B-tree node bounds.
    write_blk(0, orig_sb);
    {
      auto bad_det = orig_det;
      patch_u32(bad_det, 16, 500u);     // nr_entries = 500
      patch_u32(bad_det, 20, 100000u);  // max_entries = 100000 (exceeds 4 KiB block)
      patch_u32(bad_det, 0, test_dm_bm_checksum(bad_det.data(), 121107u));
      write_blk(det_root, bad_det);
      const std::string sha = hash_file(pv);
      out = run_cmd(exe, "lvcreate -V 8M -T tv/tp_ren -n blocked_oob " + q, st);
      CHECK_MSG(st != 0, "corrupt B-tree node must fail closed");
      CHECK_EQ(hash_file(pv), sha);
    }

    // Restore clean device_details_root, then verify dm-thin lazy refcounting when
    // deleting a thin LV whose mapping leaf is shared (msm rc == 2) vs overflow (rc == 3).
    write_blk(det_root, orig_det);
    CHECK_MSG((run_cmd(exe, "lvcreate -V 8M -T tv/tp_ren -n th_a " + q, st), st == 0), "th_a");
    CHECK_MSG((run_cmd(exe, "lvcreate -V 8M -T tv/tp_ren -n th_b " + q, st), st == 0), "th_b");
    {
      auto set_bm2 = [](std::vector<std::uint8_t>& bm, std::uint32_t slot, std::uint8_t rc) {
        const std::uint8_t enc = (rc == 0u) ? 0u : (rc == 1u) ? 2u : (rc == 2u) ? 1u : 3u;
        const std::size_t off = 16u + (slot / 4u);
        const std::uint32_t shift = (slot % 4u) * 2u;
        bm[off] = static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(bm[off]) & ~(0x3u << shift)) |
            (static_cast<std::uint32_t>(enc) << shift));
      };
      auto get_bm2 = [](const std::vector<std::uint8_t>& bm, std::uint32_t slot) -> std::uint8_t {
        const std::uint8_t enc = static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(bm[16u + (slot / 4u)]) >> ((slot % 4u) * 2u)) & 0x3u);
        return (enc == 0u) ? 0u : (enc == 2u) ? 1u : (enc == 1u) ? 2u : 3u;
      };

      auto sb = read_blk(0);
      const std::uint64_t map_root_blk = lvmfmt::le64(sb.data() + 320);
      const std::uint64_t msm_idx_blk = lvmfmt::le64(sb.data() + 208);
      const std::uint64_t dsm_root_blk = lvmfmt::le64(sb.data() + 80);

      auto map_root = read_blk(map_root_blk);
      const std::uint32_t mr_max = lvmfmt::le32(map_root.data() + 20);
      const std::size_t mr_vbase = 32u + static_cast<std::size_t>(mr_max) * 8u;
      const std::uint64_t leaf_a = lvmfmt::le64(map_root.data() + mr_vbase + 0u);
      const std::uint64_t leaf_b = lvmfmt::le64(map_root.data() + mr_vbase + 8u);

      // Populate leaf_a with 1 mapped data block (block 5, dsm rc = 1), point dev_b to
      // leaf_a as a lazy-shared snapshot leaf (msm rc[leaf_a] = 2, free leaf_b).
      auto la = read_blk(leaf_a);
      const std::uint32_t la_max = lvmfmt::le32(la.data() + 20);
      const std::size_t la_vbase = 32u + static_cast<std::size_t>(la_max) * 8u;
      patch_u32(la, 16, 1u);
      patch_u64(la, 32, 0ULL);               // virtual block 0
      patch_u64(la, la_vbase, 5ULL << 24);   // data block 5
      patch_u32(la, 0, test_dm_bm_checksum(la.data(), 121107u));
      write_blk(leaf_a, la);

      patch_u64(map_root, mr_vbase + 8u, leaf_a);
      patch_u32(map_root, 0, test_dm_bm_checksum(map_root.data(), 121107u));
      write_blk(map_root_blk, map_root);

      // Mark data block 5 allocated (dsm rc = 1) and update dsm_nr_allocated = 1.
      auto dsm_leaf = read_blk(dsm_root_blk);
      const std::uint32_t dl_max = lvmfmt::le32(dsm_leaf.data() + 20);
      const std::size_t dl_vbase = 32u + static_cast<std::size_t>(dl_max) * 8u;
      const std::uint64_t dsm_bm_blk = lvmfmt::le64(dsm_leaf.data() + dl_vbase);
      const std::uint32_t dsm_free = lvmfmt::le32(dsm_leaf.data() + dl_vbase + 8);
      auto dsm_bm = read_blk(dsm_bm_blk);
      set_bm2(dsm_bm, 5u, 1u);
      patch_u32(dsm_bm, 0, test_dm_bm_checksum(dsm_bm.data(), 240779u));
      write_blk(dsm_bm_blk, dsm_bm);
      patch_u32(dsm_leaf, dl_vbase + 8, dsm_free - 1u);
      patch_u32(dsm_leaf, 0, test_dm_bm_checksum(dsm_leaf.data(), 121107u));
      write_blk(dsm_root_blk, dsm_leaf);
      patch_u64(sb, 72, 1ULL);

      // Set msm rc[leaf_a] = 3 (overflow) first -> lvremove th_a must fail closed.
      auto msm_idx = read_blk(msm_idx_blk);
      const std::uint64_t msm_bm_blk = lvmfmt::le64(msm_idx.data() + 16);
      auto msm_bm = read_blk(msm_bm_blk);
      set_bm2(msm_bm, static_cast<std::uint32_t>(leaf_a), 3u);
      set_bm2(msm_bm, static_cast<std::uint32_t>(leaf_b), 0u);
      patch_u32(msm_bm, 0, test_dm_bm_checksum(msm_bm.data(), 240779u));
      write_blk(msm_bm_blk, msm_bm);
      patch_u32(sb, 0, test_dm_bm_checksum(sb.data(), 160774u));
      write_blk(0, sb);
      {
        const std::string sha = hash_file(pv);
        run_cmd(exe, "lvremove tv/th_a " + q, st);
        CHECK_MSG(st != 0, "overflow msm refcount (>=3) must fail closed");
        CHECK_EQ(hash_file(pv), sha);
      }

      // Now set msm rc[leaf_a] = 2 (lazy shared snapshot) -> lvremove th_a must drop
      // msm rc[leaf_a] 2 -> 1 and leave data block 5 dsm rc == 1 untouched!
      set_bm2(msm_bm, static_cast<std::uint32_t>(leaf_a), 2u);
      patch_u32(msm_bm, 0, test_dm_bm_checksum(msm_bm.data(), 240779u));
      write_blk(msm_bm_blk, msm_bm);
      out = run_cmd(exe, "lvremove tv/th_a " + q, st);
      CHECK_MSG(st == 0, out);
      CHECK_EQ(get_bm2(read_blk(msm_bm_blk), static_cast<std::uint32_t>(leaf_a)), std::uint8_t{1});
      CHECK_EQ(get_bm2(read_blk(dsm_bm_blk), 5u), std::uint8_t{1});
      CHECK_EQ(lvmfmt::le64(read_blk(0).data() + 72), std::uint64_t{1});

      // Deleting the surviving th_b now drops msm rc[leaf_a] 1 -> 0 and frees data block 5!
      out = run_cmd(exe, "lvremove tv/th_b " + q, st);
      CHECK_MSG(st == 0, out);
      CHECK_EQ(get_bm2(read_blk(msm_bm_blk), static_cast<std::uint32_t>(leaf_a)), std::uint8_t{0});
      CHECK_EQ(get_bm2(read_blk(dsm_bm_blk), 5u), std::uint8_t{0});
      CHECK_EQ(lvmfmt::le64(read_blk(0).data() + 72), std::uint64_t{0});
    }
  }

  // 2. Snapshot guards: origin reduce refusal, exception chunk overflow & metadata collision,
  //    and multi-snapshot merge refusal.
  {
    const fs::path pv = tmp.path / "snap_adv.img";
    create_blank_file(pv, 32 * 1024 * 1024);
    const std::string q = "\"" + pv.string() + "\"";
    int st = -1;
    CHECK_MSG((run_cmd(exe, "pvcreate " + q, st), st == 0), "pvcreate");
    CHECK_MSG((run_cmd(exe, "vgcreate -s 4M sv " + q, st), st == 0), "vgcreate");
    CHECK_MSG((run_cmd(exe, "lvcreate -L 8M -n orig sv " + q, st), st == 0), "orig");
    write_lv_pattern_range({pv}, "orig", 0, 16, 0xCAFEBABEULL);
    const std::string orig_c0_sha = expected_pattern_sha256(0, 8, 0xCAFEBABEULL);

    CHECK_MSG((run_cmd(exe, "lvcreate -s -L 4M -n sn1 sv/orig " + q, st), st == 0), "sn1");

    // lvreduce on a snapshot origin must fail closed.
    run_cmd(exe, "lvreduce -L -4M sv/orig " + q, st);
    CHECK(st != 0);

    // Exception (old_chunk = 1ULL << 52, new_chunk = 2) wraps old_chunk * 4096 to 0;
    // merge must fail closed and leave origin chunk 0 untouched.
    std::map<std::string, fs::path> u2p;
    auto vg = load_vg_and_map_pvs({pv}, u2p);
    fs::path cow_pv;
    const std::uint64_t cow_off = linear_lv_first_pv_offset(vg, "sn1", u2p, cow_pv);
    {
      std::vector<std::uint8_t> area0(32, 0);
      patch_u64(area0, 0, (1ULL << 52));  // wraps to 0 when multiplied by 4096
      patch_u64(area0, 8, 2ULL);
      std::fstream f(cow_pv, std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(static_cast<std::streamoff>(cow_off + 4096ULL), std::ios::beg);
      f.write(reinterpret_cast<const char*>(area0.data()), static_cast<std::streamsize>(area0.size()));
      std::vector<std::uint8_t> pwn(4096, 0xEE);
      f.seekp(static_cast<std::streamoff>(cow_off + 2ULL * 4096ULL), std::ios::beg);
      f.write(reinterpret_cast<const char*>(pwn.data()), 4096);
      f.flush();
    }
    run_cmd(exe, "lvconvert --merge sv/sn1 " + q, st);
    CHECK(st != 0);

    // new_chunk colliding with an exception metadata chunk (chunk 258) must fail closed.
    {
      std::vector<std::uint8_t> area0(32, 0);
      patch_u64(area0, 0, 0ULL);
      patch_u64(area0, 8, 258ULL);  // area 1 metadata chunk: 1 + 1*(256+1) == 258
      std::fstream f(cow_pv, std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(static_cast<std::streamoff>(cow_off + 4096ULL), std::ios::beg);
      f.write(reinterpret_cast<const char*>(area0.data()), static_cast<std::streamsize>(area0.size()));
      f.flush();
    }
    run_cmd(exe, "lvconvert --merge sv/sn1 " + q, st);
    CHECK(st != 0);

    // Clear sn1 area 0 and add sibling snapshot sn2: merge with sibling snapshots must fail closed.
    {
      std::vector<std::uint8_t> zeros(32, 0);
      std::fstream f(cow_pv, std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(static_cast<std::streamoff>(cow_off + 4096ULL), std::ios::beg);
      f.write(reinterpret_cast<const char*>(zeros.data()), static_cast<std::streamsize>(zeros.size()));
      f.flush();
    }
    CHECK_MSG((run_cmd(exe, "lvcreate -s -L 4M -n sn2 sv/orig " + q, st), st == 0), "sn2");
    run_cmd(exe, "lvconvert --merge sv/sn1 " + q, st);
    CHECK(st != 0);

    // Verify origin chunk 0 was never corrupted by any failed merge.
    CHECK_EQ(hash_file_slice(cow_pv, linear_lv_first_pv_offset(vg, "orig", u2p, cow_pv), 4096), orig_c0_sha);
  }

  // 3. RAID1 guards: lvreduce refusal, pvmove colocation refusal, atomic composite rename,
  //    and DmRd superblock consistency checks before collapsing to linear.
  {
    const fs::path pv0 = tmp.path / "raid_adv0.img";
    const fs::path pv1 = tmp.path / "raid_adv1.img";
    create_blank_file(pv0, 32 * 1024 * 1024);
    create_blank_file(pv1, 32 * 1024 * 1024);
    const std::string pvs = "\"" + pv0.string() + "\" \"" + pv1.string() + "\"";
    int st = -1;
    CHECK_MSG((run_cmd(exe, "pvcreate " + pvs, st), st == 0), "pvcreate");
    CHECK_MSG((run_cmd(exe, "vgcreate -s 4M rv " + pvs, st), st == 0), "vgcreate");
    CHECK_MSG((run_cmd(exe, "lvcreate -L 8M -n lvr rv " + pvs, st), st == 0), "lvr");
    CHECK_MSG((run_cmd(exe, "lvconvert --type raid1 -m 1 rv/lvr " + pvs, st), st == 0), "raid1");

    // lvreduce on raid1 fails closed.
    run_cmd(exe, "lvreduce -L -4M rv/lvr " + pvs, st);
    CHECK(st != 0);

    // pvmove targeting a raid1 LV or whole-PV pvmove colocating legs must fail closed.
    run_cmd(exe, "pvmove -n lvr " + pvs, st);
    CHECK(st != 0);
    run_cmd(exe, "pvmove " + pvs, st);
    CHECK(st != 0);

    // lvrename on raid1 atomically renames all 4 sub-LVs and passes vgck.
    std::string out = run_cmd(exe, "lvrename rv/lvr lvr_ren " + pvs, st);
    CHECK_MSG(st == 0, out);
    out = run_cmd(exe, "vgck rv " + pvs, st);
    CHECK_MSG(st == 0, out);

    // Degraded or stale rmeta_1 (events=5, failed_devices=1) must block lvconvert -m 0.
    std::map<std::string, fs::path> u2p;
    auto vg = load_vg_and_map_pvs({pv0, pv1}, u2p);
    fs::path rm1_pv;
    const std::uint64_t rm1_off = linear_lv_first_pv_offset(vg, "lvr_ren_rmeta_1", u2p, rm1_pv);
    std::vector<std::uint8_t> orig_rm1(4096);
    {
      std::ifstream f(rm1_pv, std::ios::binary);
      f.seekg(static_cast<std::streamoff>(rm1_off), std::ios::beg);
      f.read(reinterpret_cast<char*>(orig_rm1.data()), 4096);
    }
    {
      auto bad = orig_rm1;
      patch_u64(bad, 0x10, 5ULL);  // events = 5 (diverges from leg 0 events = 1)
      patch_u64(bad, 0x18, 1ULL);  // failed_devices = 0x1 (leg 0 marked failed)
      std::fstream f(rm1_pv, std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(static_cast<std::streamoff>(rm1_off), std::ios::beg);
      f.write(reinterpret_cast<const char*>(bad.data()), 4096);
      f.flush();
    }
    run_cmd(exe, "lvconvert -m 0 rv/lvr_ren " + pvs, st);
    CHECK(st != 0);

    // Incomplete recovery (disk_recovery_offset = 0) must also block lvconvert -m 0.
    {
      auto bad = orig_rm1;
      patch_u64(bad, 0x20, 0ULL);  // disk_recovery_offset = 0 (< array_sectors)
      std::fstream f(rm1_pv, std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(static_cast<std::streamoff>(rm1_off), std::ios::beg);
      f.write(reinterpret_cast<const char*>(bad.data()), 4096);
      f.flush();
    }
    run_cmd(exe, "lvconvert -m 0 rv/lvr_ren " + pvs, st);
    CHECK(st != 0);
  }
}
