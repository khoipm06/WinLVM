#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
    dir = fs::temp_directory_path() / ("winlvm_cli_" + suffix);
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
      "pvdisplay " + q,
      "vgdisplay " + q,
      "lvdisplay " + q,
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

  // 5. mixed-vg unsupported segment mutation attempt
  {
    const fs::path mix_a = tmp.dir / "mixed-vg-a.img";
    const fs::path mix_b = tmp.dir / "mixed-vg-b.img";
    fs::copy_file(fs::path(WINLVM_CORPUS_IMAGES) / "mixed-vg-a.img", mix_a,
                  fs::copy_options::overwrite_existing);
    fs::copy_file(fs::path(WINLVM_CORPUS_IMAGES) / "mixed-vg-b.img", mix_b,
                  fs::copy_options::overwrite_existing);
    const std::string sha_a = file_sha256(mix_a);
    const std::string sha_b = file_sha256(mix_b);
    const std::string qa = "\"" + mix_a.string() + "\"";
    const std::string qb = "\"" + mix_b.string() + "\"";
    int st = 0;
    run(lvm, "lvcreate -L 4M -n new_lv vgmix " + qa + " " + qb, st);
    CHECK(st != 0);
    run(lvm, "lvrename vgmix/lvplain lvplain2 " + qa + " " + qb, st);
    CHECK(st != 0);
    run(lvm, "lvremove vgmix/lvthin " + qa + " " + qb, st);
    CHECK(st != 0);
    CHECK_EQ(file_sha256(mix_a), sha_a);
    CHECK_EQ(file_sha256(mix_b), sha_b);
  }

  // 6. Unsupported subcommands (pvchange, lvconvert, lvm lvmdump)
  {
    const fs::path img = tmp.dir / "unsupported_subcmd.img";
    fs::copy_file(fs::path(WINLVM_CORPUS_IMAGES) / "linear-1pv.img", img,
                  fs::copy_options::overwrite_existing);
    const std::string sha = file_sha256(img);
    const std::string q = "\"" + img.string() + "\"";
    int st = 0;
    run(lvm, "pvchange -x n " + q, st);
    CHECK(st != 0);
    run(lvm, "lvconvert vgbase/lvlin " + q, st);
    CHECK(st != 0);
    run(lvm, "lvmdump " + q, st);
    CHECK(st != 0);
    CHECK_EQ(file_sha256(img), sha);
  }
}
