#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "testing.h"

namespace fs = std::filesystem;

namespace {
std::string run(const fs::path& exe, const std::string& args, int& status) {
  const std::string command = "\"" + exe.string() + "\" " + args + " 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  CHECK(pipe != nullptr);
  std::ostringstream output;
  char buffer[256];
  while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) output << buffer;
  status = pclose(pipe);
  return output.str();
}
}

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
  CHECK(status != 0);
  CHECK(output.find("--image is required") != std::string::npos);
}
