#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "testing.h"
#include "lvmfmt/source.h"
#include "lvmfmt/label.h"
#include "lvmfmt/metadata.h"
#include "tools/lvm/cmd_table.h"

namespace fs = std::filesystem;

namespace {

std::string run_cmd(const fs::path& exe, const std::string& args, int& status) {
  const std::string command = "\"" + exe.string() + "\" " + args + " 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  CHECK(pipe != nullptr);
  std::ostringstream output;
  char buffer[512];
  while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) output << buffer;
  status = pclose(pipe);
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
