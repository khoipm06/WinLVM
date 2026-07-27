#pragma once

#include <cstddef>
#include <cstdint>

namespace lvmfmt {

// LVM2's CRC-32 is NOT the standard one. It uses the reflected polynomial
// 0xedb88320 with an initial value of 0xf597a6cf and performs **no final XOR**.
// Feeding LVM data through a stock zlib-style crc32() produces wrong answers
// that look plausible, so this must stay a distinct function.
inline constexpr std::uint32_t kInitialCrc = 0xf597a6cfu;

std::uint32_t lvm_crc32(const void* data, std::size_t length,
                        std::uint32_t initial = kInitialCrc);

}  // namespace lvmfmt
