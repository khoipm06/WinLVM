#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "lvmfmt/commit.h"
#include "lvmfmt/crc32.h"
#include "lvmfmt/label.h"
#include "lvmfmt/metadata.h"
#include "lvmfmt/source.h"
#include "test_sha256.h"
#include "testing.h"

namespace fs = std::filesystem;

namespace {

std::string run(const fs::path& exe, const std::string& args, int& status) {
#ifdef _WIN32
  const std::string command = "\"\"" + exe.string() + "\" " + args + " 2>&1\"";
  FILE* pipe = _popen(command.c_str(), "r");
#else
  const std::string command = "\"" + exe.string() + "\" " + args + " 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
#endif
  CHECK(pipe != nullptr);
  std::ostringstream output;
  char buffer[256];
  while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) output << buffer;
#ifdef _WIN32
  status = _pclose(pipe);
#else
  status = pclose(pipe);
#endif
  return output.str();
}

std::string file_sha256(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  CHECK_MSG(in.is_open(), "failed to open for SHA-256: " + path.string());
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

struct TempDirGuard {
  fs::path dir;
  explicit TempDirGuard(const std::string& suffix) {
#ifdef _WIN32
    const auto pid = static_cast<unsigned long>(_getpid());
#else
    const auto pid = static_cast<unsigned long>(getpid());
#endif
    dir = fs::temp_directory_path() / ("winlvm_cli_" + suffix + "_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
  }
  ~TempDirGuard() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

}  // namespace

TEST(lvm_cli_valid_image_has_stable_metadata) {
  const fs::path exe = fs::path(WINLVM_LVM_EXE);
  const fs::path image = fs::path(WINLVM_CORPUS_IMAGES) / "linear-1pv.img";
  int status = 0;
  const std::string output = run(exe, "--image \"" + image.string() + "\" pvs", status);
  CHECK_EQ(status, 0);
  CHECK(output.find("PV UUID:") != std::string::npos);
  CHECK(output.find("VG:") != std::string::npos);
  CHECK(output.find("PVs:") != std::string::npos);
}

TEST(lvm_cli_missing_image_fails) {
  int status = 0;
  const std::string output = run(fs::path(WINLVM_LVM_EXE), "--image missing.img pvs", status);
  CHECK(status != 0);
  CHECK(output.find("cannot open") != std::string::npos);
}

TEST(lvm_cli_invalid_args_fail) {
  int status = 0;
  const std::string output = run(fs::path(WINLVM_LVM_EXE), "--image foo.img unknown", status);
  CHECK(status != 0);
  CHECK(output.find("usage:") != std::string::npos);
}

TEST(lvm_cli_online_mode_fails_closed) {
  int status = 0;
  const std::string output = run(fs::path(WINLVM_LVM_EXE), "pvs", status);
#ifdef _WIN32
  CHECK_EQ(status, 0);
  CHECK(output.find("PV UUID") != std::string::npos);
#else
  CHECK(status != 0);
  CHECK(output.find("--image is required") != std::string::npos);
#endif
}

TEST(lvm_cli_version_outputs_banner) {
  int status = 0;
  const std::string output = run(fs::path(WINLVM_LVM_EXE), "version", status);
  CHECK_EQ(status, 0);
  CHECK(output.find("WinLVM CLI version") != std::string::npos);
}

TEST(lvm_cli_vgscan_without_win32_fails_closed) {
  int status = 0;
  const std::string output = run(fs::path(WINLVM_LVM_EXE), "vgscan", status);
#ifdef _WIN32
  (void)output;
  CHECK_EQ(status, 0);
#else
  CHECK(status != 0);
  CHECK(output.find("usage:") != std::string::npos);
#endif
}

TEST(lvm_cli_read_only_commands_preserve_exact_image_sha256) {
  TempDirGuard tmp("ro_inv");
  const fs::path img = tmp.dir / "linear-1pv.img";
  fs::copy_file(fs::path(WINLVM_CORPUS_IMAGES) / "linear-1pv.img", img,
                fs::copy_options::overwrite_existing);

  const std::string before_sha = file_sha256(img);

  // Direct read-only API source
  std::string err;
  auto src = lvmfmt::open_file_source(img.string(), err);
  CHECK_MSG(src != nullptr, err);
  auto lbl = lvmfmt::read_pv_label(*src, err);
  CHECK_MSG(lbl.has_value(), err);
  auto meta = lvmfmt::read_vg_metadata(*src, *lbl, err);
  CHECK_MSG(meta.has_value(), err);
  auto vg = lvmfmt::parse_vg_metadata(*meta, err);
  CHECK_MSG(vg.has_value(), err);
  src.reset();

  const fs::path lvm = fs::path(WINLVM_LVM_EXE);
  const fs::path lvmdump = fs::path(WINLVM_LVMDUMP_EXE);
  const std::string q = "\"" + img.string() + "\"";

  const std::vector<std::string> ro_cmds = {
      "--image " + q + " pvs",
      "--image " + q + " vgs",
      "--image " + q + " lvs",
      "--image " + q + " dump",
      "pvs " + q,
      "vgs " + q,
      "lvs " + q,
      "pvs --reportformat json " + q,
      "vgs --reportformat json " + q,
      "lvs --reportformat json " + q,
      "pvdisplay " + q,
      "pvdisplay --reportformat json " + q,
      "vgdisplay " + q,
      "vgdisplay --reportformat json " + q,
      "lvdisplay " + q,
      "lvdisplay --reportformat json " + q,
      "fullreport --reportformat json " + q,
      "fullreport --reportformat json_std " + q,
      "tags " + q,
      "pvck " + q,
      "vgck " + q,
      "pvscan " + q,
      "lvscan " + q,
      "lvmdiskscan " + q,
  };

  for (const auto& cmd : ro_cmds) {
    int status = -1;
    const std::string out = run(lvm, cmd, status);
    CHECK_MSG(status == 0, "read-only command failed: " + cmd + "\n" + out);
  }

  int dump_status = -1;
  CHECK_EQ(run(lvmdump, q, dump_status).empty(), false);
  CHECK_EQ(dump_status, 0);
  CHECK_EQ(run(lvmdump, q + " --metadata", dump_status).empty(), false);
  CHECK_EQ(dump_status, 0);

  CHECK_EQ(file_sha256(img), before_sha);
}

TEST(lvm_cli_failed_mutations_fail_closed_and_preserve_image_sha256) {
  TempDirGuard tmp("neg_inv");
  const fs::path lvm = fs::path(WINLVM_LVM_EXE);

  // 1. Wrong VG name
  {
    const fs::path img = tmp.dir / "wrong_vg.img";
    fs::copy_file(fs::path(WINLVM_CORPUS_IMAGES) / "linear-1pv.img", img,
                  fs::copy_options::overwrite_existing);
    const std::string sha = file_sha256(img);
    const std::string q = "\"" + img.string() + "\"";
    int st = 0;
    run(lvm, "lvcreate -L 4M -n bad_lv wrong_vg_name " + q, st);
    CHECK(st != 0);
    run(lvm, "vgrename wrong_vg_name other_vg " + q, st);
    CHECK(st != 0);
    run(lvm, "vgremove wrong_vg_name " + q, st);
    CHECK(st != 0);
    CHECK_EQ(file_sha256(img), sha);
  }

  // 2. Truncated image
  {
    const fs::path img = tmp.dir / "truncated.img";
    fs::copy_file(fs::path(WINLVM_CORPUS_IMAGES) / "linear-1pv.img", img,
                  fs::copy_options::overwrite_existing);
    fs::resize_file(img, 6000);
    const std::string sha = file_sha256(img);
    const std::string q = "\"" + img.string() + "\"";
    int st = 0;
    run(lvm, "lvcreate -L 4M -n bad_lv vgbase " + q, st);
    CHECK(st != 0);
    run(lvm, "pvck " + q, st);
    CHECK(st != 0);
    run(lvm, "vgck " + q, st);
    CHECK(st != 0);
    CHECK_EQ(file_sha256(img), sha);
  }

  // 3. Corrupted MDA CRC
  {
    const fs::path img = tmp.dir / "bad_crc.img";
    fs::copy_file(fs::path(WINLVM_CORPUS_IMAGES) / "linear-1pv.img", img,
                  fs::copy_options::overwrite_existing);
    {
      std::fstream f(img, std::ios::in | std::ios::out | std::ios::binary);
      CHECK(f.is_open());
      char byte = 0;
      f.seekg(4096 + 100, std::ios::beg);
      f.read(&byte, 1);
      byte = static_cast<char>(byte ^ 0x5A);
      f.seekp(4096 + 100, std::ios::beg);
      f.write(&byte, 1);
    }
    const std::string sha = file_sha256(img);
    const std::string q = "\"" + img.string() + "\"";
    int st = 0;
    run(lvm, "lvcreate -L 4M -n bad_lv vgbase " + q, st);
    CHECK(st != 0);
    run(lvm, "lvrename vgbase/lvlin lv2 " + q, st);
    CHECK(st != 0);
    run(lvm, "pvck " + q, st);
    CHECK(st != 0);
    run(lvm, "vgck " + q, st);
    CHECK(st != 0);
    CHECK_EQ(file_sha256(img), sha);
  }

  // 4. Missing 2nd PV of a 2-PV VG
  {
    const fs::path img_a = tmp.dir / "linear-2pv-a.img";
    fs::copy_file(fs::path(WINLVM_CORPUS_IMAGES) / "linear-2pv-a.img", img_a,
                  fs::copy_options::overwrite_existing);
    const std::string sha = file_sha256(img_a);
    const std::string q = "\"" + img_a.string() + "\"";
    int st = 0;
    run(lvm, "lvcreate -L 4M -n bad_lv vgspan " + q, st);
    CHECK(st != 0);
    run(lvm, "lvextend -L +4M vgspan/lvspan " + q, st);
    CHECK(st != 0);
    run(lvm, "vgrename vgspan vgspan2 " + q, st);
    CHECK(st != 0);
    run(lvm, "vgremove vgspan " + q, st);
    CHECK(st != 0);
    CHECK_EQ(file_sha256(img_a), sha);
  }

  // 5. Unsupported segment type (raid5) mutation attempt must fail closed
  {
    const fs::path img = tmp.dir / "unsupported_raid5.img";
    fs::copy_file(fs::path(WINLVM_CORPUS_IMAGES) / "linear-1pv.img", img,
                  fs::copy_options::overwrite_existing);
    // Patch the linear segment type in the committed VG metadata to "raid5" and recompute CRCs.
    std::string err;
    {
      auto src = lvmfmt::open_storage_source(img.string(), true, err);
      CHECK_MSG(src != nullptr, err);
      auto lbl = lvmfmt::read_pv_label(*src, err);
      CHECK_MSG(lbl.has_value(), err);
      auto mda = lvmfmt::read_mda_header(*src, lbl->metadata_areas[0], err);
      CHECK_MSG(mda.has_value(), err);
      auto text = lvmfmt::read_metadata_text(*src, *mda, err);
      CHECK_MSG(text.has_value(), err);
      auto pos = text->find("type = \"striped\"");
      CHECK(pos != std::string::npos);
      text->replace(pos, 16, "type = \"raid5\"  ");
      // Write back exact same length and update raw_locn CRC + mda header CRC.
      const std::uint32_t new_text_crc = lvmfmt::lvm_crc32(text->data(), text->size());
      CHECK(src->write(mda->start + mda->commit_offset, text->data(), text->size(), err));
      std::vector<std::uint8_t> hdr;
      CHECK(src->read_vec(mda->start, 512, hdr));
      hdr[56] = static_cast<std::uint8_t>(new_text_crc & 0xffu);
      hdr[57] = static_cast<std::uint8_t>((new_text_crc >> 8) & 0xffu);
      hdr[58] = static_cast<std::uint8_t>((new_text_crc >> 16) & 0xffu);
      hdr[59] = static_cast<std::uint8_t>((new_text_crc >> 24) & 0xffu);
      const std::uint32_t hdr_crc = lvmfmt::lvm_crc32(hdr.data() + 4, 512 - 4);
      hdr[0] = static_cast<std::uint8_t>(hdr_crc & 0xffu);
      hdr[1] = static_cast<std::uint8_t>((hdr_crc >> 8) & 0xffu);
      hdr[2] = static_cast<std::uint8_t>((hdr_crc >> 16) & 0xffu);
      hdr[3] = static_cast<std::uint8_t>((hdr_crc >> 24) & 0xffu);
      CHECK(src->write(mda->start, hdr.data(), hdr.size(), err));
    }
    const std::string sha = file_sha256(img);
    const std::string q = "\"" + img.string() + "\"";
    int st = 0;
    run(lvm, "lvcreate -L 4M -n new_lv vgbase " + q, st);
    CHECK(st != 0);
    run(lvm, "lvrename vgbase/lvlin lv2 " + q, st);
    CHECK(st != 0);
    CHECK_EQ(file_sha256(img), sha);
  }

  // 6. Unsupported subcommands/modes must fail closed without touching image
  {
    const fs::path img = tmp.dir / "unsupported_subcmd.img";
    fs::copy_file(fs::path(WINLVM_CORPUS_IMAGES) / "linear-1pv.img", img,
                  fs::copy_options::overwrite_existing);
    const std::string sha = file_sha256(img);
    const std::string q = "\"" + img.string() + "\"";
    int st = 0;
    run(lvm, "lvconvert vgbase/lvlin " + q, st);
    CHECK(st != 0);
    run(lvm, "lvconvert --type raid5 -m 1 vgbase/lvlin " + q, st);
    CHECK(st != 0);
    run(lvm, "lvconvert --type cache vgbase/lvlin " + q, st);
    CHECK(st != 0);
    run(lvm, "lvconvert --type mirror -m 1 --mirrorlog disk vgbase/lvlin " + q, st);
    CHECK(st != 0);
    run(lvm, "pvs --reportformat invalid_fmt " + q, st);
    CHECK(st != 0);
    run(lvm, "pvchange --addtag \"bad!tag\" " + q, st);
    CHECK(st != 0);
    run(lvm, "lvmdump " + q, st);
    CHECK(st != 0);
    CHECK_EQ(file_sha256(img), sha);
  }
}

TEST(lvm_cli_json_reporting_and_fullreport) {
  const fs::path lvm = fs::path(WINLVM_LVM_EXE);
  const fs::path mix_a = fs::path(WINLVM_CORPUS_IMAGES) / "mixed-vg-a.img";
  const fs::path mix_b = fs::path(WINLVM_CORPUS_IMAGES) / "mixed-vg-b.img";
  const std::string args = "\"" + mix_a.string() + "\" \"" + mix_b.string() + "\"";

  int st = -1;
  std::string out = run(lvm, "pvs --reportformat json " + args, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("\"report\"") != std::string::npos);
  CHECK(out.find("\"pv\"") != std::string::npos);
  CHECK(out.find("\"pv_fmt\": \"lvm2\"") != std::string::npos);
  CHECK(out.find("\"pv_tags\": \"\"") != std::string::npos);

  out = run(lvm, "pvs --reportformat json_std " + args, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("\"pv_tags\": []") != std::string::npos);

  out = run(lvm, "vgs --reportformat json " + args, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("\"vg\"") != std::string::npos);
  CHECK(out.find("\"vg_name\": \"vgmix\"") != std::string::npos);
  CHECK(out.find("\"snap_count\": \"1\"") != std::string::npos);

  out = run(lvm, "vgs --reportformat json_std " + args, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("\"pv_count\": 2") != std::string::npos);
  CHECK(out.find("\"snap_count\": 1") != std::string::npos);
  CHECK(out.find("\"vg_tags\": []") != std::string::npos);

  out = run(lvm, "lvs --reportformat json " + args, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("\"lv\"") != std::string::npos);
  CHECK(out.find("\"segtype\": \"thin-pool\"") != std::string::npos);
  CHECK(out.find("\"segtype\": \"thin\"") != std::string::npos);
  CHECK(out.find("\"pool_lv\": \"tpool\"") != std::string::npos);
  CHECK(out.find("\"origin\": \"lvplain\"") != std::string::npos);
  CHECK(out.find("\"segtype\": \"raid1\"") != std::string::npos);

  out = run(lvm, "pvdisplay --reportformat json " + args, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("\"pv\"") != std::string::npos);

  out = run(lvm, "vgdisplay --reportformat json " + args, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("\"vg\"") != std::string::npos);

  out = run(lvm, "lvdisplay --reportformat json " + args, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("\"lv\"") != std::string::npos);

  out = run(lvm, "fullreport --reportformat json " + args, st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("\"vg\"") != std::string::npos);
  CHECK(out.find("\"pv\"") != std::string::npos);
  CHECK(out.find("\"lv\"") != std::string::npos);
}

TEST(lvm_cli_lvmconfig_and_dumpconfig) {
  TempDirGuard tmp("cfg");
  const fs::path lvm = fs::path(WINLVM_LVM_EXE);
  const fs::path out_file = tmp.dir / "lvm.conf";

  int st = -1;
  std::string out = run(lvm, "lvmconfig --type default", st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("global {") != std::string::npos);
  CHECK(out.find("devices {") != std::string::npos);
  CHECK(out.find("allocation {") != std::string::npos);
  CHECK(out.find("activation {") != std::string::npos);
  CHECK(out.find("log {") != std::string::npos);

  out = run(lvm, "dumpconfig --type full --withcomments --withversions global", st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("# Configuration section global.") != std::string::npos);
  CHECK(out.find("2.03.22") != std::string::npos);
  CHECK(out.find("umask=077") != std::string::npos);

  out = run(lvm, "lvmconfig global/umask", st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("umask=077") != std::string::npos);

  out = run(lvm, "lvmconfig --reportformat json global/umask", st);
  CHECK_MSG(st == 0, out);
  CHECK(out.find("\"global\"") != std::string::npos);
  CHECK(out.find("\"umask\": \"077\"") != std::string::npos);

  out = run(lvm, "lvmconfig --type current --file \"" + out_file.string() + "\" activation", st);
  CHECK_MSG(st == 0, out);
  CHECK(fs::exists(out_file));
  std::ifstream in(out_file);
  std::ostringstream ss;
  ss << in.rdbuf();
  CHECK(ss.str().find("activation {") != std::string::npos);

  // Unknown section/key and invalid --type must fail closed.
  run(lvm, "lvmconfig nonexistent/key", st);
  CHECK(st != 0);
  run(lvm, "lvmconfig --type bogus_type", st);
  CHECK(st != 0);
}
