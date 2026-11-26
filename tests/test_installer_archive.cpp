#include "archive_format.h"
#include "env_path.h"
#include "testing.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace winlvm::installer;
namespace fs = std::filesystem;

struct TempDirGuard {
  fs::path path;
  explicit TempDirGuard(const std::string& prefix) {
    std::random_device rd;
    const uint64_t suffix =
        (static_cast<uint64_t>(rd()) << 32u) ^ static_cast<uint64_t>(rd());
    path = fs::temp_directory_path() / (prefix + "_" + std::to_string(suffix));
    fs::create_directories(path);
  }
  ~TempDirGuard() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void write_bytes(const fs::path& p, const std::vector<uint8_t>& data) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  CHECK(out.is_open());
  if (!data.empty()) {
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
  }
  CHECK(out.good());
}

std::vector<uint8_t> read_bytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary | std::ios::ate);
  CHECK(in.is_open());
  const auto sz = static_cast<size_t>(in.tellg());
  std::vector<uint8_t> buf(sz);
  in.seekg(0, std::ios::beg);
  if (sz > 0u) {
    in.read(reinterpret_cast<char*>(buf.data()),
            static_cast<std::streamsize>(sz));
    CHECK(in.good());
  }
  return buf;
}

void recompute_whole_archive_sha256(std::vector<uint8_t>& archive) {
  CHECK(archive.size() >= kFooterSize);
  const size_t footer_off = archive.size() - kFooterSize;
  uint64_t payload_offset = 0u;
  uint64_t payload_size = 0u;
  for (unsigned int i = 0u; i < 8u; ++i) {
    payload_offset |= static_cast<uint64_t>(archive[footer_off + 16u + i])
                      << (i * 8u);
    payload_size |= static_cast<uint64_t>(archive[footer_off + 24u + i])
                    << (i * 8u);
  }
  Sha256State st;
  st.update(archive.data() + static_cast<size_t>(payload_offset),
            static_cast<size_t>(payload_size));
  st.update(archive.data() + footer_off, 32u);
  const auto digest = st.finish();
  std::memcpy(archive.data() + footer_off + 32u, digest.data(), 32u);
}

}  // namespace

TEST(installer_crc32_and_sha256_known_vectors) {
  const std::string abc = "abc";
  const auto sha_abc = sha256_bytes(
      reinterpret_cast<const uint8_t*>(abc.data()), abc.size());
  CHECK_EQ(
      sha256_hex(sha_abc),
      std::string(
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

  const auto sha_empty = sha256_bytes(nullptr, 0u);
  CHECK_EQ(
      sha256_hex(sha_empty),
      std::string(
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

  const std::string check_str = "123456789";
  CHECK_EQ(crc32_ieee(reinterpret_cast<const uint8_t*>(check_str.data()),
                      check_str.size()),
           0xcbf43926u);
  CHECK_EQ(crc32_ieee(nullptr, 0u), 0x00000000u);
}

TEST(installer_archive_roundtrip_store) {
  const std::vector<uint8_t> stub = {'M', 'Z', 0x90u, 0x00u, 0x03u, 0x00u};
  const std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {
      {"driver/winlvm.inf", {'[', 'V', 'e', 'r', 's', 'i', 'o', 'n', ']'}},
      {"bin/lvm.exe", std::vector<uint8_t>(4096u, 0x5au)},
      {"LICENSE", {}},
      {"driver/vol/winlvm-vol.inf", {'V', 'O', 'L'}},
  };

  std::vector<uint8_t> packed;
  std::string err;
  CHECK_MSG(pack_archive_to_buffer(stub, files, packed, err), err);

  // Stub bytes remain intact at offset 0.
  CHECK(packed.size() > stub.size() + kFooterSize);
  CHECK(std::memcmp(packed.data(), stub.data(), stub.size()) == 0);

  std::vector<std::pair<std::string, std::vector<uint8_t>>> unpacked;
  CHECK_MSG(unpack_all_from_buffer(packed.data(), packed.size(), unpacked, err),
            err);
  CHECK_EQ(unpacked.size(), files.size());

  // Pack output is deterministically sorted by path.
  CHECK_EQ(unpacked[0].first, std::string("LICENSE"));
  CHECK_EQ(unpacked[0].second.size(), 0u);
  CHECK_EQ(unpacked[1].first, std::string("bin/lvm.exe"));
  CHECK_EQ(unpacked[1].second.size(), 4096u);
  CHECK_EQ(unpacked[2].first, std::string("driver/vol/winlvm-vol.inf"));
  CHECK_EQ(unpacked[3].first, std::string("driver/winlvm.inf"));
}

TEST(installer_archive_detects_corruption) {
  const std::vector<uint8_t> stub = {'M', 'Z', 0x01u, 0x02u};
  const std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {
      {"bin/lvm.exe", {'E', 'L', 'F', 'P', 'E', '0', '1', '2'}},
  };

  std::vector<uint8_t> packed;
  std::string err;
  CHECK(pack_archive_to_buffer(stub, files, packed, err));

  // 1. Flip a byte in payload: whole-archive SHA-256 fails.
  {
    auto bad = packed;
    bad[stub.size() + kEntryHeaderSize + 1u] ^= 0x01u;
    ParsedArchive parsed;
    CHECK(!parse_and_verify_archive_buffer(bad.data(), bad.size(), parsed, err));
  }

  // 2. Flip footer magic: magic check fails.
  {
    auto bad = packed;
    bad[bad.size() - kFooterSize] = 'X';
    ParsedArchive parsed;
    CHECK(!parse_and_verify_archive_buffer(bad.data(), bad.size(), parsed, err));
  }

  // 3. Flip payload byte AND recompute whole-archive SHA-256: per-entry CRC/SHA
  // still catches tampering during unpack.
  {
    auto bad = packed;
    ParsedArchive parsed;
    CHECK(parse_and_verify_archive_buffer(bad.data(), bad.size(), parsed, err));
    const size_t data_off = static_cast<size_t>(parsed.entries[0].data_offset);
    bad[data_off] ^= 0xffu;
    recompute_whole_archive_sha256(bad);
    std::vector<std::pair<std::string, std::vector<uint8_t>>> unpacked;
    CHECK(!unpack_all_from_buffer(bad.data(), bad.size(), unpacked, err));
  }

  // 4. Truncated archive fails cleanly.
  {
    auto trunc = packed;
    trunc.resize(packed.size() - 5u);
    ParsedArchive parsed;
    CHECK(!parse_and_verify_archive_buffer(trunc.data(), trunc.size(), parsed,
                                           err));
  }
}

TEST(installer_archive_rejects_path_traversal_and_unsafe_paths) {
  std::string err;
  const std::vector<std::string> bad_paths = {
      "",
      "/etc/passwd",
      "bin/",
      "../escape.exe",
      "bin/../../Windows/System32/evil.dll",
      "bin/./lvm.exe",
      "bin//lvm.exe",
      "bin\\lvm.exe",
      "C:/Windows/win.ini",
      "driver/CON",
      "driver/nul.txt",
      "driver/COM1.inf",
      "driver/trailing.",
      "driver/trailing ",
      std::string("driver/bad\0null", 15u),
      "driver/bad\x01ctrl",
  };
  for (const auto& p : bad_paths) {
    CHECK_MSG(!validate_relative_path(p, err), "should reject path: " + p);
  }

  // Injecting a traversal path directly into a forged archive with a valid
  // whole-archive SHA-256 must still be rejected during parsing.
  const std::vector<uint8_t> stub = {'M', 'Z'};
  const std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {
      {"aa/bb.txt", {'o', 'k'}},
  };
  std::vector<uint8_t> forged;
  CHECK(pack_archive_to_buffer(stub, files, forged, err));
  const size_t path_off = stub.size() + kEntryHeaderSize;
  // Overwrite "aa/bb.txt" (9 bytes) with "../bb.txt" (9 bytes) and re-hash.
  std::memcpy(forged.data() + path_off, "../bb.txt", 9u);
  recompute_whole_archive_sha256(forged);

  ParsedArchive parsed;
  CHECK(!parse_and_verify_archive_buffer(forged.data(), forged.size(), parsed,
                                         err));
}

TEST(installer_archive_python_packager_deflate_interop) {
#ifdef WINLVM_PACK_SCRIPT
  TempDirGuard tmp("winlvm_pack_test");
  const fs::path stub_path = tmp.path / "stub.exe";
  const fs::path stage_dir = tmp.path / "stage";
  const fs::path out_exe = tmp.path / "winlvm-setup-x64.exe";

  // Synthetic stub + varied payload data exercising stored, fixed-Huffman, and
  // dynamic-Huffman DEFLATE blocks plus incompressible random bytes.
  std::vector<uint8_t> stub(128u, 0x4du);
  write_bytes(stub_path, stub);

  std::vector<uint8_t> repetitive(65536u);
  for (size_t i = 0u; i < repetitive.size(); ++i) {
    repetitive[i] = static_cast<uint8_t>("WinLVM-installer-pattern-"[i % 25u]);
  }
  std::vector<uint8_t> mixed(40000u);
  uint32_t rng = 0x12345678u;
  for (size_t i = 0u; i < mixed.size(); ++i) {
    if ((i % 64u) < 40u) {
      mixed[i] = static_cast<uint8_t>(i & 0x1fu);
    } else {
      rng = rng * 1664525u + 1013904223u;
      mixed[i] = static_cast<uint8_t>(rng >> 24u);
    }
  }
  std::vector<uint8_t> random_bytes(2048u);
  for (size_t i = 0u; i < random_bytes.size(); ++i) {
    rng = rng * 1664525u + 1013904223u;
    random_bytes[i] = static_cast<uint8_t>(rng >> 24u);
  }
  const std::vector<uint8_t> empty_bytes;
  const std::vector<uint8_t> tiny_bytes = {'O', 'K', '\n'};

  write_bytes(stage_dir / "driver" / "winlvm.sys", mixed);
  write_bytes(stage_dir / "driver" / "winlvm.inf", repetitive);
  write_bytes(stage_dir / "driver" / "vol" / "winlvm-vol.inf", tiny_bytes);
  write_bytes(stage_dir / "bin" / "lvm.exe", random_bytes);
  write_bytes(stage_dir / "NOTICE", empty_bytes);

#ifdef _WIN32
  const std::string py = "python";
#else
  const std::string py = "python3";
#endif
  const std::string cmd = py + " \"" + std::string(WINLVM_PACK_SCRIPT) +
                          "\" --stub \"" + stub_path.string() +
                          "\" --stage-dir \"" + stage_dir.string() +
                          "\" --out \"" + out_exe.string() + "\"";
  const int rc = std::system(cmd.c_str());
  CHECK_EQ(rc, 0);
  CHECK(fs::exists(out_exe));

  ParsedArchive parsed;
  std::string err;
  CHECK_MSG(verify_archive_file_full(out_exe, parsed, err), err);
  CHECK_EQ(parsed.entries.size(), 5u);

  // Repetitive and mixed payloads must have used DEFLATE compression.
  const auto* inf_entry = parsed.find_entry("driver/winlvm.inf");
  CHECK(inf_entry != nullptr);
  CHECK(inf_entry->compression == CompressionMethod::Deflate);
  CHECK(inf_entry->stored_size < inf_entry->raw_size);

  std::vector<uint8_t> extracted;
  CHECK_MSG(read_entry_from_file(out_exe, *inf_entry, extracted, err), err);
  CHECK(extracted == repetitive);

  const auto* sys_entry = parsed.find_entry("driver/winlvm.sys");
  CHECK(sys_entry != nullptr);
  CHECK_MSG(read_entry_from_file(out_exe, *sys_entry, extracted, err), err);
  CHECK(extracted == mixed);

  const auto* bin_entry = parsed.find_entry("bin/lvm.exe");
  CHECK(bin_entry != nullptr);
  CHECK_MSG(read_entry_from_file(out_exe, *bin_entry, extracted, err), err);
  CHECK(extracted == random_bytes);

  const auto* notice_entry = parsed.find_entry("NOTICE");
  CHECK(notice_entry != nullptr);
  CHECK_MSG(read_entry_from_file(out_exe, *notice_entry, extracted, err), err);
  CHECK(extracted.empty());

  // Also test buffer unpack directly on the packed executable.
  const auto raw_exe = read_bytes(out_exe);
  std::vector<std::pair<std::string, std::vector<uint8_t>>> all;
  CHECK_MSG(unpack_all_from_buffer(raw_exe.data(), raw_exe.size(), all, err),
            err);
  CHECK_EQ(all.size(), 5u);

  // Corrupting a deflated payload byte (even with recomputed whole-archive
  // SHA-256) must fail decompression or per-entry checksum verification.
  {
    auto tampered = raw_exe;
    const size_t data_off = static_cast<size_t>(inf_entry->data_offset);
    tampered[data_off + (inf_entry->stored_size / 2u)] ^= 0x55u;
    recompute_whole_archive_sha256(tampered);
    std::vector<std::pair<std::string, std::vector<uint8_t>>> bad_out;
    CHECK(!unpack_all_from_buffer(tampered.data(), tampered.size(), bad_out,
                                  err));
  }

  // Case-insensitive duplicate paths must be rejected at pack time.
  {
    const std::vector<std::pair<std::string, std::vector<uint8_t>>> dup_files = {
        {"bin/LVM.exe", {1u}},
        {"bin/lvm.exe", {2u}},
    };
    std::vector<uint8_t> dup_out;
    CHECK(!pack_archive_to_buffer(stub, dup_files, dup_out, err));
  }
#endif
}

TEST(installer_env_path_add_remove_dedup) {
  std::wstring out;
  CHECK(path_list_add(L"C:\\Windows\\System32;C:\\Windows",
                      L"C:\\Program Files\\WinLVM\\bin\\", out));
  CHECK(out == L"C:\\Windows\\System32;C:\\Windows;C:\\Program Files\\WinLVM\\bin");

  // Case-insensitive and trailing-backslash deduplication.
  std::wstring out2;
  CHECK(!path_list_add(out, L"c:\\program files\\winlvm\\bin\\", out2));
  CHECK(out2 == out);

  // Removal strips matching entry cleanly without leaving double semicolons.
  std::wstring removed;
  CHECK(path_list_remove(
      L"C:\\Windows;\"c:\\program files\\winlvm\\bin\\\";C:\\Tools",
      L"C:\\Program Files\\WinLVM\\bin", removed));
  CHECK(removed == L"C:\\Windows;C:\\Tools");

  // Removing absent entry is a no-op.
  std::wstring unchanged;
  CHECK(!path_list_remove(removed, L"C:\\Program Files\\WinLVM\\bin", unchanged));
  CHECK(unchanged == removed);
}
