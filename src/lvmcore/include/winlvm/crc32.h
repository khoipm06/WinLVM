#pragma once

#include <cstddef>
#include <cstdint>

namespace winlvm {

inline constexpr std::uint32_t kLvmInitialCrc = 0xf597a6cfU;

std::uint32_t lvm_crc32(std::uint32_t initial, const std::uint8_t* data, std::size_t size);

}  // namespace winlvm
