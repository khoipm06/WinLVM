// Safe primitives for reading untrusted on-disk structures.
//
// Every integer that comes off a disk is attacker-controlled as far as this
// code is concerned: a corrupt or hostile PV must never be able to make us
// compute an offset that wraps. All arithmetic on disk-derived values goes
// through the checked helpers here, and they return nullopt rather than a
// wrapped result.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace lvmfmt {

inline std::uint16_t le16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8);
}

inline std::uint32_t le32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

inline std::uint64_t le64(const std::uint8_t* p) {
  return static_cast<std::uint64_t>(le32(p)) |
         (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

inline std::optional<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) {
  if (b > UINT64_MAX - a) return std::nullopt;
  return a + b;
}

inline std::optional<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b) {
  if (a != 0 && b > UINT64_MAX / a) return std::nullopt;
  return a * b;
}

// True when [offset, offset+length) lies entirely within [0, limit).
inline bool range_fits(std::uint64_t offset, std::uint64_t length, std::uint64_t limit) {
  auto end = checked_add(offset, length);
  return end.has_value() && *end <= limit;
}

}  // namespace lvmfmt
