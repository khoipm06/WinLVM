// The binary layer of the LVM2 on-disk format: the PV label, the physical
// volume header, and the metadata-area ring buffer that holds the VG text.
//
// Everything here operates on untrusted input. A corrupt PV must produce an
// error, never an out-of-range read or a wrapped offset.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lvmfmt/source.h"

namespace lvmfmt {

inline constexpr std::uint32_t kLabelSize = 512;
inline constexpr std::uint32_t kMdaHeaderSize = 512;

// A (offset, size) pair as stored in the PV header. Offsets are relative to
// the start of the physical volume.
struct DiskArea {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

struct PvLabel {
  std::uint64_t label_offset = 0;  // byte offset where LABELONE was found
  std::uint64_t label_sector = 0;  // sector number the label claims to be at
  std::string pv_uuid;             // 32 raw characters, no dashes
  std::uint64_t device_size = 0;
  std::vector<DiskArea> data_areas;
  std::vector<DiskArea> metadata_areas;
};

// The subset of mda_header we act on.
struct MdaHeader {
  std::uint64_t start = 0;
  std::uint64_t size = 0;
  // raw_locn slot 0 is the committed metadata; slot 1 is a pending precommit.
  std::uint64_t commit_offset = 0;  // relative to `start`
  std::uint64_t commit_size = 0;
  std::uint32_t commit_checksum = 0;
};

// Scans the head of the device for a LABELONE label and parses the PV header.
std::optional<PvLabel> read_pv_label(ByteSource& source, std::string& error);

// Reads and validates the mda_header at `area.offset`.
std::optional<MdaHeader> read_mda_header(ByteSource& source, const DiskArea& area,
                                         std::string& error);

// Reads the committed VG metadata text, following the ring buffer's wrap and
// verifying the stored CRC.
std::optional<std::string> read_metadata_text(ByteSource& source, const MdaHeader& mda,
                                              std::string& error);

// Convenience: label -> first usable metadata area -> text.
std::optional<std::string> read_vg_metadata(ByteSource& source, const PvLabel& label,
                                            std::string& error);

// Formats a raw 32-character LVM UUID as the canonical dashed form, e.g.
// "MMmapV-do0d-x2Tu-6PrQ-PRL3-hgD6-BJR3Od". Returns the input unchanged if it
// is not 32 characters.
std::string format_uuid(const std::string& raw);

}  // namespace lvmfmt
