#include "archive_format.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_set>

namespace winlvm::installer {
namespace {

uint16_t read_le16(const uint8_t* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                               (static_cast<uint16_t>(p[1]) << 8u));
}

uint32_t read_le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u) |
         (static_cast<uint32_t>(p[2]) << 16u) |
         (static_cast<uint32_t>(p[3]) << 24u);
}

uint64_t read_le64(const uint8_t* p) {
  return static_cast<uint64_t>(read_le32(p)) |
         (static_cast<uint64_t>(read_le32(p + 4)) << 32u);
}

void write_le16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xffu);
  p[1] = static_cast<uint8_t>((v >> 8u) & 0xffu);
}

void write_le32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xffu);
  p[1] = static_cast<uint8_t>((v >> 8u) & 0xffu);
  p[2] = static_cast<uint8_t>((v >> 16u) & 0xffu);
  p[3] = static_cast<uint8_t>((v >> 24u) & 0xffu);
}

void write_le64(uint8_t* p, uint64_t v) {
  write_le32(p, static_cast<uint32_t>(v & 0xffffffffu));
  write_le32(p + 4, static_cast<uint32_t>((v >> 32u) & 0xffffffffu));
}

constexpr uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

uint32_t rotr32(uint32_t x, unsigned int n) {
  return (x >> n) | (x << (32u - n));
}

void sha256_compress(uint32_t h[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (unsigned int i = 0u; i < 16u; ++i) {
    const size_t j = static_cast<size_t>(i) * 4u;
    w[i] = (static_cast<uint32_t>(block[j]) << 24u) |
           (static_cast<uint32_t>(block[j + 1u]) << 16u) |
           (static_cast<uint32_t>(block[j + 2u]) << 8u) |
           static_cast<uint32_t>(block[j + 3u]);
  }
  for (unsigned int i = 16u; i < 64u; ++i) {
    const uint32_t s0 =
        rotr32(w[i - 15u], 7u) ^ rotr32(w[i - 15u], 18u) ^ (w[i - 15u] >> 3u);
    const uint32_t s1 =
        rotr32(w[i - 2u], 17u) ^ rotr32(w[i - 2u], 19u) ^ (w[i - 2u] >> 10u);
    w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
  }

  uint32_t a = h[0];
  uint32_t b = h[1];
  uint32_t c = h[2];
  uint32_t d = h[3];
  uint32_t e = h[4];
  uint32_t f = h[5];
  uint32_t g = h[6];
  uint32_t k = h[7];

  for (unsigned int i = 0u; i < 64u; ++i) {
    const uint32_t s1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t temp1 = k + s1 + ch + kSha256K[i] + w[i];
    const uint32_t s0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = s0 + maj;

    k = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += k;
}

std::string ascii_lower(const std::string& s) {
  std::string out = s;
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

bool is_valid_utf8(const std::string& s) {
  const auto* p = reinterpret_cast<const uint8_t*>(s.data());
  const size_t n = s.size();
  size_t i = 0u;
  while (i < n) {
    const uint8_t b0 = p[i];
    if (b0 < 0x80u) {
      ++i;
      continue;
    }
    if ((b0 & 0xe0u) == 0xc0u) {
      if (i + 1u >= n) return false;
      const uint8_t b1 = p[i + 1u];
      if ((b1 & 0xc0u) != 0x80u) return false;
      const uint32_t cp =
          (static_cast<uint32_t>(b0 & 0x1fu) << 6u) | (b1 & 0x3fu);
      if (cp < 0x80u) return false;
      i += 2u;
      continue;
    }
    if ((b0 & 0xf0u) == 0xe0u) {
      if (i + 2u >= n) return false;
      const uint8_t b1 = p[i + 1u];
      const uint8_t b2 = p[i + 2u];
      if ((b1 & 0xc0u) != 0x80u || (b2 & 0xc0u) != 0x80u) return false;
      const uint32_t cp = (static_cast<uint32_t>(b0 & 0x0fu) << 12u) |
                          (static_cast<uint32_t>(b1 & 0x3fu) << 6u) |
                          (b2 & 0x3fu);
      if (cp < 0x800u || (cp >= 0xd800u && cp <= 0xdfffu)) return false;
      i += 3u;
      continue;
    }
    if ((b0 & 0xf8u) == 0xf0u) {
      if (i + 3u >= n) return false;
      const uint8_t b1 = p[i + 1u];
      const uint8_t b2 = p[i + 2u];
      const uint8_t b3 = p[i + 3u];
      if ((b1 & 0xc0u) != 0x80u || (b2 & 0xc0u) != 0x80u ||
          (b3 & 0xc0u) != 0x80u) {
        return false;
      }
      const uint32_t cp = (static_cast<uint32_t>(b0 & 0x07u) << 18u) |
                          (static_cast<uint32_t>(b1 & 0x3fu) << 12u) |
                          (static_cast<uint32_t>(b2 & 0x3fu) << 6u) |
                          (b3 & 0x3fu);
      if (cp < 0x10000u || cp > 0x10ffffu) return false;
      i += 4u;
      continue;
    }
    return false;
  }
  return true;
}

bool is_windows_reserved_name(const std::string& segment) {
  const size_t dot = segment.find('.');
  const std::string stem =
      ascii_lower(dot == std::string::npos ? segment : segment.substr(0u, dot));
  if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") {
    return true;
  }
  if (stem.size() == 4u &&
      (stem.compare(0u, 3u, "com") == 0 || stem.compare(0u, 3u, "lpt") == 0) &&
      stem[3] >= '1' && stem[3] <= '9') {
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Minimal RFC 1951 (raw DEFLATE) Inflate decoder.
// ---------------------------------------------------------------------------
struct BitReader {
  const uint8_t* data = nullptr;
  size_t len = 0u;
  size_t pos = 0u;
  uint32_t bitbuf = 0u;
  unsigned int bitcnt = 0u;

  bool ensure(unsigned int n) {
    while (bitcnt < n) {
      if (pos >= len) return false;
      bitbuf |= static_cast<uint32_t>(data[pos++]) << bitcnt;
      bitcnt += 8u;
    }
    return true;
  }

  bool read_bits(unsigned int n, uint32_t& out) {
    if (n == 0u) {
      out = 0u;
      return true;
    }
    if (!ensure(n)) return false;
    out = bitbuf & ((1u << n) - 1u);
    bitbuf >>= n;
    bitcnt -= n;
    return true;
  }

  void align_byte() {
    const unsigned int drop = bitcnt & 7u;
    bitbuf >>= drop;
    bitcnt -= drop;
  }

  bool read_aligned_bytes(uint8_t* dst, size_t count) {
    align_byte();
    while (count > 0u && bitcnt >= 8u) {
      *dst++ = static_cast<uint8_t>(bitbuf & 0xffu);
      bitbuf >>= 8u;
      bitcnt -= 8u;
      --count;
    }
    if (count > len - pos) return false;
    if (count > 0u) {
      std::memcpy(dst, data + pos, count);
      pos += count;
    }
    return true;
  }
};

constexpr unsigned int kMaxHuffmanBits = 15u;

struct HuffmanTable {
  std::array<uint16_t, kMaxHuffmanBits + 1u> count{};
  std::array<uint16_t, 320u> symbols{};
  unsigned int max_len = 0u;

  bool build(const uint8_t* lengths, size_t num_symbols, bool allow_empty) {
    count.fill(0u);
    max_len = 0u;
    size_t non_zero = 0u;
    for (size_t i = 0u; i < num_symbols; ++i) {
      if (lengths[i] > kMaxHuffmanBits) return false;
      if (lengths[i] > 0u) {
        ++count[lengths[i]];
        if (lengths[i] > max_len) max_len = lengths[i];
        ++non_zero;
      }
    }
    if (non_zero == 0u) {
      return allow_empty;
    }

    int left = 1;
    for (unsigned int bits = 1u; bits <= kMaxHuffmanBits; ++bits) {
      left <<= 1;
      left -= static_cast<int>(count[bits]);
      if (left < 0) return false;  // Oversubscribed code.
    }
    // Single-symbol distance trees (RFC 1951) leave left == 1; all other trees
    // must be complete (left == 0).
    if (left > 0 && !(non_zero == 1u && count[1] == 1u)) {
      return false;
    }

    std::array<uint16_t, kMaxHuffmanBits + 1u> offs{};
    for (unsigned int bits = 1u; bits < kMaxHuffmanBits; ++bits) {
      offs[bits + 1u] = static_cast<uint16_t>(offs[bits] + count[bits]);
    }
    for (size_t sym = 0u; sym < num_symbols; ++sym) {
      if (lengths[sym] != 0u) {
        symbols[offs[lengths[sym]]++] = static_cast<uint16_t>(sym);
      }
    }
    return true;
  }

  bool decode(BitReader& br, uint16_t& sym_out) const {
    if (max_len == 0u) return false;
    int code = 0;
    int first = 0;
    int index = 0;
    for (unsigned int len = 1u; len <= max_len; ++len) {
      uint32_t bit = 0u;
      if (!br.read_bits(1u, bit)) return false;
      code |= static_cast<int>(bit);
      const int cnt = static_cast<int>(count[len]);
      if (code - cnt < first) {
        sym_out = symbols[static_cast<size_t>(index + (code - first))];
        return true;
      }
      index += cnt;
      first += cnt;
      first <<= 1;
      code <<= 1;
    }
    return false;
  }
};

bool decode_huffman_block(BitReader& br,
                          const HuffmanTable& litlen,
                          const HuffmanTable& dist,
                          size_t expected_out_len,
                          std::vector<uint8_t>& out,
                          std::string& error) {
  static constexpr uint16_t kLenBase[29] = {
      3u,  4u,  5u,  6u,  7u,  8u,  9u,  10u, 11u,  13u,
      15u, 17u, 19u, 23u, 27u, 31u, 35u, 43u, 51u,  59u,
      67u, 83u, 99u, 115u, 131u, 163u, 195u, 227u, 258u};
  static constexpr uint8_t kLenExtra[29] = {
      0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u, 1u, 1u, 1u, 2u, 2u, 2u,
      2u, 3u, 3u, 3u, 3u, 4u, 4u, 4u, 4u, 5u, 5u, 5u, 5u, 0u};
  static constexpr uint16_t kDistBase[30] = {
      1u,     2u,     3u,     4u,     5u,     7u,    9u,    13u,
      17u,    25u,    33u,    49u,    65u,    97u,   129u,  193u,
      257u,   385u,   513u,   769u,   1025u,  1537u, 2049u, 3073u,
      4097u,  6145u,  8193u,  12289u, 16385u, 24577u};
  static constexpr uint8_t kDistExtra[30] = {
      0u, 0u, 0u, 0u, 1u, 1u, 2u, 2u,  3u,  3u,  4u,  4u,  5u,  5u,  6u,
      6u, 7u, 7u, 8u, 8u, 9u, 9u, 10u, 10u, 11u, 11u, 12u, 12u, 13u, 13u};

  for (;;) {
    uint16_t sym = 0u;
    if (!litlen.decode(br, sym)) {
      error = "deflate: truncated or invalid literal/length Huffman symbol";
      return false;
    }
    if (sym < 256u) {
      if (out.size() >= expected_out_len) {
        error = "deflate: decompressed output exceeds expected length";
        return false;
      }
      out.push_back(static_cast<uint8_t>(sym));
      continue;
    }
    if (sym == 256u) {
      return true;
    }
    if (sym > 285u) {
      error = "deflate: invalid length symbol";
      return false;
    }
    const size_t len_idx = static_cast<size_t>(sym - 257u);
    uint32_t extra = 0u;
    if (!br.read_bits(kLenExtra[len_idx], extra)) {
      error = "deflate: truncated length extra bits";
      return false;
    }
    const size_t match_len = static_cast<size_t>(kLenBase[len_idx]) + extra;

    uint16_t dsym = 0u;
    if (!dist.decode(br, dsym) || dsym > 29u) {
      error = "deflate: invalid distance symbol";
      return false;
    }
    uint32_t dextra = 0u;
    if (!br.read_bits(kDistExtra[dsym], dextra)) {
      error = "deflate: truncated distance extra bits";
      return false;
    }
    const size_t distance = static_cast<size_t>(kDistBase[dsym]) + dextra;
    if (distance == 0u || distance > out.size()) {
      error = "deflate: back-reference distance exceeds output history";
      return false;
    }
    if (match_len > expected_out_len - out.size()) {
      error = "deflate: match copy exceeds expected output length";
      return false;
    }
    for (size_t i = 0u; i < match_len; ++i) {
      const uint8_t b = out[out.size() - distance];
      out.push_back(b);
    }
  }
}

bool decode_dynamic_tables(BitReader& br,
                           HuffmanTable& litlen,
                           HuffmanTable& dist,
                           std::string& error) {
  static constexpr uint8_t kCodeLenOrder[19] = {
      16u, 17u, 18u, 0u, 8u,  7u, 9u,  6u, 10u, 5u,
      11u, 4u,  12u, 3u, 13u, 2u, 14u, 1u, 15u};

  uint32_t hlit_raw = 0u;
  uint32_t hdist_raw = 0u;
  uint32_t hclen_raw = 0u;
  if (!br.read_bits(5u, hlit_raw) || !br.read_bits(5u, hdist_raw) ||
      !br.read_bits(4u, hclen_raw)) {
    error = "deflate: truncated dynamic Huffman header";
    return false;
  }
  const size_t hlit = static_cast<size_t>(hlit_raw) + 257u;
  const size_t hdist = static_cast<size_t>(hdist_raw) + 1u;
  const size_t hclen = static_cast<size_t>(hclen_raw) + 4u;
  if (hlit > 286u || hdist > 32u) {
    error = "deflate: dynamic table symbol count out of range";
    return false;
  }

  std::array<uint8_t, 19u> code_lens{};
  for (size_t i = 0u; i < hclen; ++i) {
    uint32_t v = 0u;
    if (!br.read_bits(3u, v)) {
      error = "deflate: truncated code-length table";
      return false;
    }
    code_lens[kCodeLenOrder[i]] = static_cast<uint8_t>(v);
  }

  HuffmanTable cl_table;
  if (!cl_table.build(code_lens.data(), code_lens.size(), false)) {
    error = "deflate: invalid code-length Huffman table";
    return false;
  }

  std::array<uint8_t, 320u> combined{};
  const size_t total = hlit + hdist;
  size_t idx = 0u;
  while (idx < total) {
    uint16_t sym = 0u;
    if (!cl_table.decode(br, sym)) {
      error = "deflate: invalid code-length symbol";
      return false;
    }
    if (sym < 16u) {
      combined[idx++] = static_cast<uint8_t>(sym);
    } else if (sym == 16u) {
      if (idx == 0u) {
        error = "deflate: repeat previous length at index 0";
        return false;
      }
      uint32_t rep = 0u;
      if (!br.read_bits(2u, rep)) {
        error = "deflate: truncated repeat count (16)";
        return false;
      }
      const size_t count = static_cast<size_t>(rep) + 3u;
      if (idx + count > total) {
        error = "deflate: code-length repeat overflows table";
        return false;
      }
      const uint8_t prev = combined[idx - 1u];
      for (size_t r = 0u; r < count; ++r) combined[idx++] = prev;
    } else if (sym == 17u) {
      uint32_t rep = 0u;
      if (!br.read_bits(3u, rep)) {
        error = "deflate: truncated zero repeat count (17)";
        return false;
      }
      const size_t count = static_cast<size_t>(rep) + 3u;
      if (idx + count > total) {
        error = "deflate: zero repeat overflows table";
        return false;
      }
      for (size_t r = 0u; r < count; ++r) combined[idx++] = 0u;
    } else if (sym == 18u) {
      uint32_t rep = 0u;
      if (!br.read_bits(7u, rep)) {
        error = "deflate: truncated zero repeat count (18)";
        return false;
      }
      const size_t count = static_cast<size_t>(rep) + 11u;
      if (idx + count > total) {
        error = "deflate: long zero repeat overflows table";
        return false;
      }
      for (size_t r = 0u; r < count; ++r) combined[idx++] = 0u;
    } else {
      error = "deflate: unexpected code-length symbol";
      return false;
    }
  }

  if (combined[256] == 0u) {
    error = "deflate: missing end-of-block symbol (256)";
    return false;
  }
  if (!litlen.build(combined.data(), hlit, false)) {
    error = "deflate: invalid literal/length Huffman table";
    return false;
  }
  if (!dist.build(combined.data() + hlit, hdist, true)) {
    error = "deflate: invalid distance Huffman table";
    return false;
  }
  return true;
}

bool parse_footer_bytes(const uint8_t footer_buf[kFooterSize],
                        uint64_t total_size,
                        ArchiveFooter& footer,
                        std::string& error) {
  if (std::memcmp(footer_buf, kArchiveMagic.data(), kArchiveMagic.size()) !=
      0) {
    error = "archive footer magic mismatch";
    return false;
  }
  footer.version = read_le32(footer_buf + 8u);
  footer.entry_count = read_le32(footer_buf + 12u);
  footer.payload_offset = read_le64(footer_buf + 16u);
  footer.payload_size = read_le64(footer_buf + 24u);
  std::memcpy(footer.archive_sha256.data(), footer_buf + 32u, 32u);

  if (footer.version != kArchiveVersion) {
    error = "unsupported archive format version";
    return false;
  }
  if (footer.entry_count == 0u || footer.entry_count > kMaxArchiveEntries) {
    error = "archive entry count out of valid bounds";
    return false;
  }
  if (total_size < kFooterSize) {
    error = "archive smaller than footer size";
    return false;
  }
  const uint64_t footer_offset = total_size - kFooterSize;
  if (footer.payload_offset > footer_offset) {
    error = "archive payload offset exceeds file size";
    return false;
  }
  if (footer.payload_size != footer_offset - footer.payload_offset) {
    error = "archive payload size inconsistent with file length";
    return false;
  }
  return true;
}

bool verify_entry_payload(const uint8_t* stored_ptr,
                          size_t stored_len,
                          const ArchiveEntryMeta& entry,
                          std::vector<uint8_t>& out,
                          std::string& error) {
  if (stored_len != entry.stored_size) {
    error = "entry stored byte length mismatch for " + entry.path;
    return false;
  }
  if (entry.raw_size > kMaxEntryBytes) {
    error = "entry raw size exceeds maximum allowed limit";
    return false;
  }
  const size_t raw_size = static_cast<size_t>(entry.raw_size);

  if (entry.compression == CompressionMethod::Store) {
    if (entry.stored_size != entry.raw_size) {
      error = "stored entry length mismatch for " + entry.path;
      return false;
    }
    out.assign(stored_ptr, stored_ptr + stored_len);
  } else if (entry.compression == CompressionMethod::Deflate) {
    if (!inflate_raw(stored_ptr, stored_len, raw_size, out, error)) {
      error = "entry " + entry.path + ": " + error;
      return false;
    }
  } else {
    error = "unsupported compression method for " + entry.path;
    return false;
  }

  const uint32_t actual_crc = crc32_ieee(out.data(), out.size());
  if (actual_crc != entry.raw_crc32) {
    error = "CRC-32 mismatch for entry " + entry.path;
    return false;
  }
  const auto actual_sha = sha256_bytes(out.data(), out.size());
  if (actual_sha != entry.raw_sha256) {
    error = "SHA-256 mismatch for entry " + entry.path;
    return false;
  }
  return true;
}

}  // namespace

Sha256State::Sha256State() { reset(); }

void Sha256State::reset() {
  h[0] = 0x6a09e667u;
  h[1] = 0xbb67ae85u;
  h[2] = 0x3c6ef372u;
  h[3] = 0xa54ff53au;
  h[4] = 0x510e527fu;
  h[5] = 0x9b05688cu;
  h[6] = 0x1f83d9abu;
  h[7] = 0x5be0cd19u;
  total_bytes = 0u;
  block_len = 0u;
}

void Sha256State::update(const uint8_t* data, size_t len) {
  if (len == 0u) return;
  total_bytes += static_cast<uint64_t>(len);
  size_t off = 0u;
  if (block_len > 0u) {
    const size_t need = 64u - block_len;
    if (len < need) {
      std::memcpy(block + block_len, data, len);
      block_len += len;
      return;
    }
    std::memcpy(block + block_len, data, need);
    sha256_compress(h, block);
    off += need;
    len -= need;
    block_len = 0u;
  }
  while (len >= 64u) {
    sha256_compress(h, data + off);
    off += 64u;
    len -= 64u;
  }
  if (len > 0u) {
    std::memcpy(block, data + off, len);
    block_len = len;
  }
}

std::array<uint8_t, 32> Sha256State::finish() {
  const uint64_t total_bits = total_bytes * 8u;
  size_t i = block_len;
  block[i++] = 0x80u;
  if (i > 56u) {
    std::memset(block + i, 0, 64u - i);
    sha256_compress(h, block);
    i = 0u;
  }
  std::memset(block + i, 0, 56u - i);
  for (unsigned int k = 0u; k < 8u; ++k) {
    block[63u - k] = static_cast<uint8_t>((total_bits >> (k * 8u)) & 0xffu);
  }
  sha256_compress(h, block);

  std::array<uint8_t, 32> digest{};
  for (unsigned int j = 0u; j < 8u; ++j) {
    digest[j * 4u + 0u] = static_cast<uint8_t>((h[j] >> 24u) & 0xffu);
    digest[j * 4u + 1u] = static_cast<uint8_t>((h[j] >> 16u) & 0xffu);
    digest[j * 4u + 2u] = static_cast<uint8_t>((h[j] >> 8u) & 0xffu);
    digest[j * 4u + 3u] = static_cast<uint8_t>(h[j] & 0xffu);
  }
  return digest;
}

uint32_t crc32_ieee(const uint8_t* data, size_t len) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0u; i < len; ++i) {
    crc ^= static_cast<uint32_t>(data[i]);
    for (unsigned int b = 0u; b < 8u; ++b) {
      const uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1u) ^ (0xedb88320u & mask);
    }
  }
  return crc ^ 0xffffffffu;
}

std::array<uint8_t, 32> sha256_bytes(const uint8_t* data, size_t len) {
  Sha256State st;
  st.update(data, len);
  return st.finish();
}

std::string sha256_hex(const std::array<uint8_t, 32>& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(64u, '0');
  for (size_t i = 0u; i < 32u; ++i) {
    out[i * 2u] = kHex[(digest[i] >> 4u) & 0x0fu];
    out[i * 2u + 1u] = kHex[digest[i] & 0x0fu];
  }
  return out;
}

bool validate_relative_path(const std::string& path, std::string& error) {
  if (path.empty()) {
    error = "archive entry path is empty";
    return false;
  }
  if (path.size() > 1024u) {
    error = "archive entry path exceeds maximum length";
    return false;
  }
  if (!is_valid_utf8(path)) {
    error = "archive entry path is not valid UTF-8";
    return false;
  }
  if (path.front() == '/' || path.back() == '/') {
    error = "archive entry path must be relative without leading/trailing slash";
    return false;
  }

  for (char ch : path) {
    const auto u = static_cast<unsigned char>(ch);
    if (u < 0x20u || u == 0x7fu) {
      error = "archive entry path contains control character";
      return false;
    }
    if (ch == '\\' || ch == ':' || ch == '<' || ch == '>' || ch == '"' ||
        ch == '|' || ch == '?' || ch == '*') {
      error = "archive entry path contains forbidden character";
      return false;
    }
  }

  size_t start = 0u;
  while (start < path.size()) {
    const size_t slash = path.find('/', start);
    const size_t end = (slash == std::string::npos) ? path.size() : slash;
    if (end == start) {
      error = "archive entry path contains empty segment";
      return false;
    }
    const std::string seg = path.substr(start, end - start);
    if (seg == "." || seg == "..") {
      error = "archive entry path contains traversal segment";
      return false;
    }
    if (seg.back() == '.' || seg.back() == ' ') {
      error = "archive entry path segment ends with dot or space";
      return false;
    }
    if (is_windows_reserved_name(seg)) {
      error = "archive entry path segment uses reserved Windows device name";
      return false;
    }
    if (slash == std::string::npos) break;
    start = slash + 1u;
  }
  return true;
}

bool inflate_raw(const uint8_t* src,
                 size_t src_len,
                 size_t expected_out_len,
                 std::vector<uint8_t>& out,
                 std::string& error) {
  out.clear();
  if (expected_out_len > kMaxEntryBytes) {
    error = "deflate: expected output size exceeds limit";
    return false;
  }
  out.reserve(expected_out_len);

  BitReader br;
  br.data = src;
  br.len = src_len;

  // Prepare static fixed Huffman tables once per call (tiny stack footprint).
  std::array<uint8_t, 288u> fixed_lit{};
  for (size_t i = 0u; i <= 143u; ++i) fixed_lit[i] = 8u;
  for (size_t i = 144u; i <= 255u; ++i) fixed_lit[i] = 9u;
  for (size_t i = 256u; i <= 279u; ++i) fixed_lit[i] = 7u;
  for (size_t i = 280u; i <= 287u; ++i) fixed_lit[i] = 8u;
  std::array<uint8_t, 32u> fixed_dist{};
  fixed_dist.fill(5u);

  HuffmanTable fixed_lit_tbl;
  HuffmanTable fixed_dist_tbl;
  fixed_lit_tbl.build(fixed_lit.data(), fixed_lit.size(), false);
  fixed_dist_tbl.build(fixed_dist.data(), fixed_dist.size(), false);

  uint32_t bfinal = 0u;
  do {
    uint32_t btype = 0u;
    if (!br.read_bits(1u, bfinal) || !br.read_bits(2u, btype)) {
      error = "deflate: truncated block header";
      return false;
    }
    if (btype == 0u) {
      uint8_t hdr[4]{};
      if (!br.read_aligned_bytes(hdr, 4u)) {
        error = "deflate: truncated stored block header";
        return false;
      }
      const uint16_t len = read_le16(hdr);
      const uint16_t nlen = read_le16(hdr + 2u);
      if (static_cast<uint16_t>(len ^ nlen) != 0xffffu) {
        error = "deflate: stored block LEN/NLEN complement mismatch";
        return false;
      }
      if (static_cast<size_t>(len) > expected_out_len - out.size()) {
        error = "deflate: stored block exceeds expected output length";
        return false;
      }
      const size_t old_size = out.size();
      out.resize(old_size + len);
      if (len > 0u && !br.read_aligned_bytes(out.data() + old_size, len)) {
        error = "deflate: truncated stored block payload";
        return false;
      }
    } else if (btype == 1u) {
      if (!decode_huffman_block(br, fixed_lit_tbl, fixed_dist_tbl,
                                expected_out_len, out, error)) {
        return false;
      }
    } else if (btype == 2u) {
      HuffmanTable dyn_lit;
      HuffmanTable dyn_dist;
      if (!decode_dynamic_tables(br, dyn_lit, dyn_dist, error)) {
        return false;
      }
      if (!decode_huffman_block(br, dyn_lit, dyn_dist, expected_out_len, out,
                                error)) {
        return false;
      }
    } else {
      error = "deflate: reserved block type 3";
      return false;
    }
  } while (bfinal == 0u);

  br.align_byte();
  if (br.bitcnt > 0u || br.pos != br.len) {
    error = "deflate: unexpected trailing data after final block";
    return false;
  }
  if (out.size() != expected_out_len) {
    error = "deflate: decompressed length does not match header raw_size";
    return false;
  }
  return true;
}

const ArchiveEntryMeta* ParsedArchive::find_entry(
    const std::string& path) const {
  const std::string needle = ascii_lower(path);
  for (const auto& e : entries) {
    if (ascii_lower(e.path) == needle) return &e;
  }
  return nullptr;
}

bool parse_and_verify_archive_buffer(const uint8_t* buf,
                                     size_t buf_len,
                                     ParsedArchive& out,
                                     std::string& error) {
  out = ParsedArchive{};
  if (buf == nullptr || buf_len < kFooterSize) {
    error = "buffer smaller than archive footer";
    return false;
  }
  const uint8_t* footer_ptr = buf + (buf_len - kFooterSize);
  if (!parse_footer_bytes(footer_ptr, static_cast<uint64_t>(buf_len),
                          out.footer, error)) {
    return false;
  }

  // Whole-archive integrity verification: SHA-256 covers the payload region
  // plus the first 32 bytes of the footer (magic, version, count, offsets).
  Sha256State whole_sha;
  whole_sha.update(buf + static_cast<size_t>(out.footer.payload_offset),
                   static_cast<size_t>(out.footer.payload_size));
  whole_sha.update(footer_ptr, 32u);
  const auto actual_whole = whole_sha.finish();
  if (actual_whole != out.footer.archive_sha256) {
    error = "whole-archive SHA-256 verification failed";
    return false;
  }

  const uint64_t payload_end =
      out.footer.payload_offset + out.footer.payload_size;
  uint64_t cursor = out.footer.payload_offset;
  std::unordered_set<std::string> seen_paths;
  out.entries.reserve(out.footer.entry_count);

  for (uint32_t i = 0u; i < out.footer.entry_count; ++i) {
    if (payload_end - cursor < kEntryHeaderSize) {
      error = "truncated entry header in archive";
      return false;
    }
    const uint8_t* hdr = buf + static_cast<size_t>(cursor);
    if (std::memcmp(hdr, kEntryMagic.data(), kEntryMagic.size()) != 0) {
      error = "invalid entry header magic";
      return false;
    }
    const uint16_t path_len = read_le16(hdr + 4u);
    const uint16_t comp_raw = read_le16(hdr + 6u);
    const uint64_t raw_size = read_le64(hdr + 8u);
    const uint64_t stored_size = read_le64(hdr + 16u);
    const uint32_t raw_crc32 = read_le32(hdr + 24u);
    const uint32_t flags = read_le32(hdr + 28u);
    if (flags != 0u) {
      error = "unsupported non-zero entry flags";
      return false;
    }
    if (comp_raw != static_cast<uint16_t>(CompressionMethod::Store) &&
        comp_raw != static_cast<uint16_t>(CompressionMethod::Deflate)) {
      error = "unsupported entry compression method";
      return false;
    }
    if (path_len == 0u || path_len > 1024u) {
      error = "invalid entry path length";
      return false;
    }
    if (raw_size > kMaxEntryBytes || stored_size > kMaxEntryBytes) {
      error = "entry size exceeds maximum allowed limit";
      return false;
    }
    if (out.total_unpacked_bytes > kMaxTotalUnpackedBytes - raw_size) {
      error = "total unpacked archive size exceeds maximum allowed limit";
      return false;
    }

    cursor += kEntryHeaderSize;
    if (payload_end - cursor < path_len) {
      error = "truncated entry path in archive";
      return false;
    }
    std::string path(reinterpret_cast<const char*>(buf + static_cast<size_t>(cursor)),
                     path_len);
    if (!validate_relative_path(path, error)) {
      return false;
    }
    const std::string lower_path = ascii_lower(path);
    if (!seen_paths.insert(lower_path).second) {
      error = "duplicate entry path in archive: " + path;
      return false;
    }
    cursor += path_len;

    if (payload_end - cursor < stored_size) {
      error = "truncated entry payload in archive";
      return false;
    }

    ArchiveEntryMeta meta;
    meta.path = std::move(path);
    meta.compression = static_cast<CompressionMethod>(comp_raw);
    meta.raw_size = raw_size;
    meta.stored_size = stored_size;
    meta.raw_crc32 = raw_crc32;
    std::memcpy(meta.raw_sha256.data(), hdr + 32u, 32u);
    meta.data_offset = cursor;

    out.entries.push_back(std::move(meta));
    out.total_unpacked_bytes += raw_size;
    cursor += stored_size;
  }

  if (cursor != payload_end) {
    error = "unexpected trailing bytes in archive payload";
    return false;
  }
  return true;
}

bool unpack_entry_from_buffer(const uint8_t* buf,
                              size_t buf_len,
                              const ArchiveEntryMeta& entry,
                              std::vector<uint8_t>& out,
                              std::string& error) {
  if (buf == nullptr || entry.data_offset > static_cast<uint64_t>(buf_len) ||
      entry.stored_size >
          static_cast<uint64_t>(buf_len) - entry.data_offset) {
    error = "entry offset out of buffer bounds";
    return false;
  }
  return verify_entry_payload(
      buf + static_cast<size_t>(entry.data_offset),
      static_cast<size_t>(entry.stored_size), entry, out, error);
}

bool unpack_all_from_buffer(
    const uint8_t* buf,
    size_t buf_len,
    std::vector<std::pair<std::string, std::vector<uint8_t>>>& out,
    std::string& error) {
  out.clear();
  ParsedArchive parsed;
  if (!parse_and_verify_archive_buffer(buf, buf_len, parsed, error)) {
    return false;
  }
  out.reserve(parsed.entries.size());
  for (const auto& entry : parsed.entries) {
    std::vector<uint8_t> data;
    if (!unpack_entry_from_buffer(buf, buf_len, entry, data, error)) {
      out.clear();
      return false;
    }
    out.emplace_back(entry.path, std::move(data));
  }
  return true;
}

bool parse_and_verify_archive_file(const std::filesystem::path& archive_path,
                                   ParsedArchive& out,
                                   std::string& error) {
  out = ParsedArchive{};
  std::ifstream in(archive_path, std::ios::binary | std::ios::ate);
  if (!in.is_open()) {
    error = "failed to open archive file";
    return false;
  }
  const std::streamoff end_pos = in.tellg();
  if (end_pos < static_cast<std::streamoff>(kFooterSize)) {
    error = "archive file smaller than footer size";
    return false;
  }
  const uint64_t file_size = static_cast<uint64_t>(end_pos);
  uint8_t footer_buf[kFooterSize]{};
  in.seekg(static_cast<std::streamoff>(file_size - kFooterSize), std::ios::beg);
  if (!in.read(reinterpret_cast<char*>(footer_buf),
               static_cast<std::streamsize>(kFooterSize))) {
    error = "failed to read archive footer";
    return false;
  }
  if (!parse_footer_bytes(footer_buf, file_size, out.footer, error)) {
    return false;
  }

  // Stream-hash the payload region so large installers never need double RAM.
  Sha256State whole_sha;
  in.seekg(static_cast<std::streamoff>(out.footer.payload_offset),
           std::ios::beg);
  std::vector<uint8_t> chunk(64u * 1024u);
  uint64_t remaining = out.footer.payload_size;
  while (remaining > 0u) {
    const size_t want = static_cast<size_t>(
        std::min<uint64_t>(remaining, static_cast<uint64_t>(chunk.size())));
    if (!in.read(reinterpret_cast<char*>(chunk.data()),
                 static_cast<std::streamsize>(want))) {
      error = "failed to read archive payload for SHA-256 check";
      return false;
    }
    whole_sha.update(chunk.data(), want);
    remaining -= want;
  }
  whole_sha.update(footer_buf, 32u);
  if (whole_sha.finish() != out.footer.archive_sha256) {
    error = "whole-archive SHA-256 verification failed";
    return false;
  }

  // Parse entry table sequentially.
  const uint64_t payload_end =
      out.footer.payload_offset + out.footer.payload_size;
  uint64_t cursor = out.footer.payload_offset;
  std::unordered_set<std::string> seen_paths;
  out.entries.reserve(out.footer.entry_count);

  for (uint32_t i = 0u; i < out.footer.entry_count; ++i) {
    if (payload_end - cursor < kEntryHeaderSize) {
      error = "truncated entry header in archive file";
      return false;
    }
    uint8_t hdr[kEntryHeaderSize]{};
    in.seekg(static_cast<std::streamoff>(cursor), std::ios::beg);
    if (!in.read(reinterpret_cast<char*>(hdr),
                 static_cast<std::streamsize>(kEntryHeaderSize))) {
      error = "failed to read entry header";
      return false;
    }
    if (std::memcmp(hdr, kEntryMagic.data(), kEntryMagic.size()) != 0) {
      error = "invalid entry header magic";
      return false;
    }
    const uint16_t path_len = read_le16(hdr + 4u);
    const uint16_t comp_raw = read_le16(hdr + 6u);
    const uint64_t raw_size = read_le64(hdr + 8u);
    const uint64_t stored_size = read_le64(hdr + 16u);
    const uint32_t raw_crc32 = read_le32(hdr + 24u);
    const uint32_t flags = read_le32(hdr + 28u);
    if (flags != 0u) {
      error = "unsupported non-zero entry flags";
      return false;
    }
    if (comp_raw != static_cast<uint16_t>(CompressionMethod::Store) &&
        comp_raw != static_cast<uint16_t>(CompressionMethod::Deflate)) {
      error = "unsupported entry compression method";
      return false;
    }
    if (path_len == 0u || path_len > 1024u) {
      error = "invalid entry path length";
      return false;
    }
    if (raw_size > kMaxEntryBytes || stored_size > kMaxEntryBytes) {
      error = "entry size exceeds maximum allowed limit";
      return false;
    }
    if (out.total_unpacked_bytes > kMaxTotalUnpackedBytes - raw_size) {
      error = "total unpacked archive size exceeds maximum allowed limit";
      return false;
    }
    cursor += kEntryHeaderSize;
    if (payload_end - cursor < path_len) {
      error = "truncated entry path in archive file";
      return false;
    }
    std::string path(path_len, '\0');
    if (!in.read(path.data(), static_cast<std::streamsize>(path_len))) {
      error = "failed to read entry path";
      return false;
    }
    if (!validate_relative_path(path, error)) {
      return false;
    }
    if (!seen_paths.insert(ascii_lower(path)).second) {
      error = "duplicate entry path in archive: " + path;
      return false;
    }
    cursor += path_len;
    if (payload_end - cursor < stored_size) {
      error = "truncated entry payload in archive file";
      return false;
    }

    ArchiveEntryMeta meta;
    meta.path = std::move(path);
    meta.compression = static_cast<CompressionMethod>(comp_raw);
    meta.raw_size = raw_size;
    meta.stored_size = stored_size;
    meta.raw_crc32 = raw_crc32;
    std::memcpy(meta.raw_sha256.data(), hdr + 32u, 32u);
    meta.data_offset = cursor;

    out.entries.push_back(std::move(meta));
    out.total_unpacked_bytes += raw_size;
    cursor += stored_size;
  }

  if (cursor != payload_end) {
    error = "unexpected trailing bytes in archive file payload";
    return false;
  }
  return true;
}

bool read_entry_from_file(const std::filesystem::path& archive_path,
                          const ArchiveEntryMeta& entry,
                          std::vector<uint8_t>& out,
                          std::string& error) {
  if (entry.stored_size > kMaxEntryBytes) {
    error = "entry stored size exceeds maximum limit";
    return false;
  }
  std::ifstream in(archive_path, std::ios::binary);
  if (!in.is_open()) {
    error = "failed to open archive file for entry read";
    return false;
  }
  std::vector<uint8_t> stored(static_cast<size_t>(entry.stored_size));
  in.seekg(static_cast<std::streamoff>(entry.data_offset), std::ios::beg);
  if (!stored.empty() &&
      !in.read(reinterpret_cast<char*>(stored.data()),
               static_cast<std::streamsize>(stored.size()))) {
    error = "failed to read stored entry bytes for " + entry.path;
    return false;
  }
  return verify_entry_payload(stored.data(), stored.size(), entry, out, error);
}

bool verify_archive_file_full(const std::filesystem::path& archive_path,
                              ParsedArchive& out,
                              std::string& error) {
  if (!parse_and_verify_archive_file(archive_path, out, error)) {
    return false;
  }
  std::vector<uint8_t> scratch;
  for (const auto& entry : out.entries) {
    if (!read_entry_from_file(archive_path, entry, scratch, error)) {
      return false;
    }
  }
  return true;
}

bool pack_archive_to_buffer(
    const std::vector<uint8_t>& stub,
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files,
    std::vector<uint8_t>& out,
    std::string& error) {
  if (files.empty() || files.size() > kMaxArchiveEntries) {
    error = "entry count must be between 1 and kMaxArchiveEntries";
    return false;
  }

  // Sort deterministically by path and validate all paths up front.
  std::vector<size_t> order(files.size());
  for (size_t i = 0u; i < files.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return files[a].first < files[b].first;
  });

  std::unordered_set<std::string> seen;
  uint64_t total_raw = 0u;
  for (size_t idx : order) {
    const auto& path = files[idx].first;
    const auto& data = files[idx].second;
    if (!validate_relative_path(path, error)) return false;
    if (!seen.insert(ascii_lower(path)).second) {
      error = "duplicate path in pack input: " + path;
      return false;
    }
    if (data.size() > kMaxEntryBytes ||
        total_raw > kMaxTotalUnpackedBytes - data.size()) {
      error = "input payload exceeds archive size limits";
      return false;
    }
    total_raw += data.size();
  }

  out = stub;
  const uint64_t payload_offset = static_cast<uint64_t>(out.size());
  Sha256State whole_sha;

  for (size_t idx : order) {
    const auto& path = files[idx].first;
    const auto& data = files[idx].second;
    const uint32_t crc = crc32_ieee(data.data(), data.size());
    const auto digest = sha256_bytes(data.data(), data.size());

    uint8_t hdr[kEntryHeaderSize]{};
    std::memcpy(hdr, kEntryMagic.data(), kEntryMagic.size());
    write_le16(hdr + 4u, static_cast<uint16_t>(path.size()));
    write_le16(hdr + 6u, static_cast<uint16_t>(CompressionMethod::Store));
    write_le64(hdr + 8u, static_cast<uint64_t>(data.size()));
    write_le64(hdr + 16u, static_cast<uint64_t>(data.size()));
    write_le32(hdr + 24u, crc);
    write_le32(hdr + 28u, 0u);
    std::memcpy(hdr + 32u, digest.data(), 32u);

    out.insert(out.end(), hdr, hdr + kEntryHeaderSize);
    whole_sha.update(hdr, kEntryHeaderSize);

    const auto* path_bytes = reinterpret_cast<const uint8_t*>(path.data());
    out.insert(out.end(), path_bytes, path_bytes + path.size());
    whole_sha.update(path_bytes, path.size());

    if (!data.empty()) {
      out.insert(out.end(), data.begin(), data.end());
      whole_sha.update(data.data(), data.size());
    }
  }

  const uint64_t payload_size =
      static_cast<uint64_t>(out.size()) - payload_offset;
  uint8_t footer[kFooterSize]{};
  std::memcpy(footer, kArchiveMagic.data(), kArchiveMagic.size());
  write_le32(footer + 8u, kArchiveVersion);
  write_le32(footer + 12u, static_cast<uint32_t>(files.size()));
  write_le64(footer + 16u, payload_offset);
  write_le64(footer + 24u, payload_size);
  whole_sha.update(footer, 32u);
  const auto whole_digest = whole_sha.finish();
  std::memcpy(footer + 32u, whole_digest.data(), 32u);

  out.insert(out.end(), footer, footer + kFooterSize);
  return true;
}

}  // namespace winlvm::installer
