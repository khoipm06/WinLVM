#include "lvmfmt/crc32.h"

#include <array>

namespace lvmfmt {
namespace {

// Reflected CRC-32 table, polynomial 0xedb88320. Built at compile time so
// there is no initialisation order or thread-safety question when this code is
// later pulled into a driver.
constexpr std::array<std::uint32_t, 256> make_table() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
    }
    t[i] = c;
  }
  return t;
}

constexpr auto kTable = make_table();

}  // namespace

std::uint32_t lvm_crc32(const void* data, std::size_t length, std::uint32_t initial) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = initial;
  for (std::size_t i = 0; i < length; ++i) {
    crc = kTable[(crc ^ p[i]) & 0xffu] ^ (crc >> 8);
  }
  return crc;  // deliberately no final XOR — see header
}

}  // namespace lvmfmt
