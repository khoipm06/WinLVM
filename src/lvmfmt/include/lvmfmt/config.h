// Parser for LVM2's text configuration format, which is what VG metadata is
// stored in.
//
// The grammar is small:
//
//     # comment
//     name {                       section
//       key = "string"             scalar entry
//       key = 1234
//       key = ["a", 0]             array entry (mixed types are legal)
//       sub { ... }                nested section
//     }
//
// This layer deliberately knows nothing about volume groups. It produces a
// generic tree; interpreting that tree as LVM objects is metadata.h's job, and
// rejecting unsupported layouts is a policy decision made above both. Keeping
// the split means a segment type we do not support still *parses*, so we can
// report precisely what it was rather than failing the whole VG.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lvmfmt {

struct Value {
  enum class Kind { String, Integer, Float };

  Kind kind = Kind::Integer;
  std::string string_value;
  std::int64_t integer_value = 0;
  double float_value = 0.0;

  bool is_string() const { return kind == Kind::String; }
  bool is_integer() const { return kind == Kind::Integer; }
};

struct Section {
  std::string name;
  std::vector<std::pair<std::string, std::vector<Value>>> entries;
  std::vector<Section> sections;

  const Section* find_section(std::string_view name) const;
  const std::vector<Value>* find_entry(std::string_view key) const;

  std::optional<std::string> string(std::string_view key) const;
  std::optional<std::int64_t> integer(std::string_view key) const;
  // Every string in an array entry, in order. Empty if absent or not strings.
  std::vector<std::string> string_list(std::string_view key) const;
};

// Parses `text` into a root section whose `sections` are the top-level blocks.
// Returns nullopt and fills `error` with a line-numbered message on failure.
std::optional<Section> parse_config(std::string_view text, std::string& error);

}  // namespace lvmfmt
