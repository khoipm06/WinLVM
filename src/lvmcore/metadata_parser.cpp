#include "winlvm/metadata_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace winlvm {

namespace {

enum class TokenKind { identifier, string, number, lbrace, rbrace, lbracket, rbracket, equal, comma, end };

struct Token {
  TokenKind kind = TokenKind::end;
  std::string text;
  std::uint64_t number = 0;
};

struct Node;
using Object = std::map<std::string, Node>;
using Array = std::vector<Node>;

struct Node {
  using Value = std::variant<std::monostate, std::string, std::uint64_t, Array, Object>;
  Value value;
};

class Lexer {
 public:
  explicit Lexer(std::string_view input) : input_(input) {}

  Token next() {
    skip_space_and_comments();
    if (pos_ >= input_.size()) {
      return {TokenKind::end, ""};
    }

    const char c = input_[pos_];
    switch (c) {
      case '{':
        ++pos_;
        return {TokenKind::lbrace, "{"};
      case '}':
        ++pos_;
        return {TokenKind::rbrace, "}"};
      case '[':
        ++pos_;
        return {TokenKind::lbracket, "["};
      case ']':
        ++pos_;
        return {TokenKind::rbracket, "]"};
      case '=':
        ++pos_;
        return {TokenKind::equal, "="};
      case ',':
        ++pos_;
        return {TokenKind::comma, ","};
      case '"':
        return string_token();
      default:
        if (std::isdigit(static_cast<unsigned char>(c))) {
          return number_token();
        }
        if (is_identifier_start(c)) {
          return identifier_token();
        }
        throw std::runtime_error("unexpected character in LVM metadata text");
    }
  }

 private:
  static bool is_identifier_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
  }

  static bool is_identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '+';
  }

  void skip_space_and_comments() {
    while (pos_ < input_.size()) {
      const char c = input_[pos_];
      if (std::isspace(static_cast<unsigned char>(c))) {
        ++pos_;
        continue;
      }
      if (c == '#') {
        while (pos_ < input_.size() && input_[pos_] != '\n') {
          ++pos_;
        }
        continue;
      }
      break;
    }
  }

  Token string_token() {
    ++pos_;
    std::string out;
    while (pos_ < input_.size()) {
      const char c = input_[pos_++];
      if (c == '"') {
        return {TokenKind::string, out};
      }
      if (c == '\\' && pos_ < input_.size()) {
        out.push_back(input_[pos_++]);
      } else {
        out.push_back(c);
      }
    }
    throw std::runtime_error("unterminated string in LVM metadata text");
  }

  Token number_token() {
    const std::size_t start = pos_;
    while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
      ++pos_;
    }
    const std::string text(input_.substr(start, pos_ - start));
    return {TokenKind::number, text, static_cast<std::uint64_t>(std::stoull(text))};
  }

  Token identifier_token() {
    const std::size_t start = pos_;
    while (pos_ < input_.size() && is_identifier_char(input_[pos_])) {
      ++pos_;
    }
    return {TokenKind::identifier, std::string(input_.substr(start, pos_ - start))};
  }

  std::string_view input_;
  std::size_t pos_ = 0;
};

class Parser {
 public:
  explicit Parser(std::string_view input) : lexer_(input) { advance(); }

  Object parse_root() {
    Object root;
    while (current_.kind != TokenKind::end) {
      const std::string key = expect_name();
      root[key] = parse_assignment_value();
    }
    return root;
  }

 private:
  void advance() { current_ = lexer_.next(); }

  void expect(TokenKind kind) {
    if (current_.kind != kind) {
      throw std::runtime_error("unexpected token in LVM metadata text");
    }
    advance();
  }

  std::string expect_name() {
    if (current_.kind != TokenKind::identifier && current_.kind != TokenKind::string) {
      throw std::runtime_error("expected name in LVM metadata text");
    }
    std::string name = current_.text;
    advance();
    return name;
  }

  Node parse_value() {
    if (current_.kind == TokenKind::string) {
      Node node;
      node.value = current_.text;
      advance();
      return node;
    }
    if (current_.kind == TokenKind::number) {
      Node node;
      node.value = current_.number;
      advance();
      return node;
    }
    if (current_.kind == TokenKind::lbracket) {
      return parse_array();
    }
    if (current_.kind == TokenKind::lbrace) {
      return parse_object();
    }
    if (current_.kind == TokenKind::identifier) {
      Node node;
      node.value = current_.text;
      advance();
      return node;
    }
    throw std::runtime_error("expected value in LVM metadata text");
  }

  Node parse_assignment_value() {
    if (current_.kind == TokenKind::lbrace) {
      return parse_object();
    }
    expect(TokenKind::equal);
    return parse_value();
  }

  Node parse_array() {
    expect(TokenKind::lbracket);
    Array items;
    while (current_.kind != TokenKind::rbracket) {
      if (current_.kind == TokenKind::end) {
        throw std::runtime_error("unterminated array in LVM metadata text");
      }
      items.push_back(parse_value());
      if (current_.kind == TokenKind::comma) {
        advance();
      }
    }
    expect(TokenKind::rbracket);
    Node node;
    node.value = std::move(items);
    return node;
  }

  Node parse_object() {
    expect(TokenKind::lbrace);
    Object object;
    while (current_.kind != TokenKind::rbrace) {
      if (current_.kind == TokenKind::end) {
        throw std::runtime_error("unterminated object in LVM metadata text");
      }
      const std::string key = expect_name();
      object[key] = parse_assignment_value();
    }
    expect(TokenKind::rbrace);
    Node node;
    node.value = std::move(object);
    return node;
  }

  Lexer lexer_;
  Token current_;
};

const Object* as_object(const Node& node) {
  return std::get_if<Object>(&node.value);
}

const Array* as_array(const Node& node) {
  return std::get_if<Array>(&node.value);
}

const std::string* as_string(const Node& node) {
  return std::get_if<std::string>(&node.value);
}

const std::uint64_t* as_number(const Node& node) {
  return std::get_if<std::uint64_t>(&node.value);
}

const Node* find_node(const Object& object, std::string_view key) {
  const auto it = object.find(std::string(key));
  if (it == object.end()) {
    return nullptr;
  }
  return &it->second;
}

std::string get_string(const Object& object, std::string_view key, const std::string& fallback = {}) {
  if (const Node* node = find_node(object, key)) {
    if (const std::string* value = as_string(*node)) {
      return *value;
    }
  }
  return fallback;
}

std::uint64_t get_number(const Object& object, std::string_view key, std::uint64_t fallback = 0) {
  if (const Node* node = find_node(object, key)) {
    if (const std::uint64_t* value = as_number(*node)) {
      return *value;
    }
  }
  return fallback;
}

std::vector<std::string> get_string_array(const Object& object, std::string_view key) {
  std::vector<std::string> out;
  const Node* node = find_node(object, key);
  if (!node) {
    return out;
  }
  const Array* array = as_array(*node);
  if (!array) {
    return out;
  }
  for (const Node& item : *array) {
    if (const std::string* value = as_string(item)) {
      out.push_back(*value);
    }
  }
  return out;
}

bool status_contains(const std::vector<std::string>& status, const std::set<std::string>& unsupported) {
  return std::any_of(status.begin(), status.end(), [&](const std::string& item) {
    return unsupported.contains(item);
  });
}

std::string unsupported_status_reason(const std::vector<std::string>& status) {
  static const std::set<std::string> unsupported = {
      "SNAPSHOT", "MIRRORED", "MIRROR", "MIRROR_LOG", "MIRROR_IMAGE", "RAID", "RAID_META",
      "RAID_IMAGE", "THIN_VOLUME", "THIN_POOL", "THIN_POOL_DATA", "THIN_POOL_METADATA",
      "CACHE", "CACHE_POOL", "CACHE_POOL_DATA", "CACHE_POOL_METADATA", "WRITECACHE",
      "INTEGRITY", "VIRTUAL", "PVMOVE"};
  for (const std::string& item : status) {
    if (unsupported.contains(item)) {
      return "unsupported LV status " + item;
    }
  }
  return {};
}

}  // namespace

Result<VolumeGroup> parse_vg_metadata_text(const std::string& text) {
  try {
    Parser parser(text);
    Object root = parser.parse_root();
    if (root.empty()) {
      return Result<VolumeGroup>::failure("metadata text does not contain a volume group");
    }

    auto vg_it = root.end();
    for (auto it = root.begin(); it != root.end(); ++it) {
      const Object* object = as_object(it->second);
      if (object && (find_node(*object, "physical_volumes") || find_node(*object, "logical_volumes"))) {
        vg_it = it;
        break;
      }
    }
    if (vg_it == root.end()) {
      return Result<VolumeGroup>::failure("metadata text does not contain a VG object");
    }

    const std::string vg_name = vg_it->first;
    const Object* vg_object = as_object(vg_it->second);
    if (!vg_object) {
      return Result<VolumeGroup>::failure("top-level volume group is not an object");
    }

    VolumeGroup vg;
    vg.name = vg_name;
    vg.uuid = get_string(*vg_object, "id");
    vg.seqno = get_number(*vg_object, "seqno");
    vg.extent_size_sectors = get_number(*vg_object, "extent_size");
    vg.extent_size_bytes = vg.extent_size_sectors * kSectorSize;
    vg.status = get_string_array(*vg_object, "status");

    const Node* pvs_node = find_node(*vg_object, "physical_volumes");
    if (pvs_node) {
      if (const Object* pvs_object = as_object(*pvs_node)) {
        for (const auto& [pv_name, pv_node] : *pvs_object) {
          const Object* pv_object = as_object(pv_node);
          if (!pv_object) {
            continue;
          }
          PhysicalVolume pv;
          pv.name = pv_name;
          pv.uuid = get_string(*pv_object, "id");
          pv.device_size_bytes = get_number(*pv_object, "dev_size") * kSectorSize;
          pv.pe_start_bytes = get_number(*pv_object, "pe_start") * kSectorSize;
          pv.pe_count = get_number(*pv_object, "pe_count");
          vg.pvs.push_back(std::move(pv));
        }
      }
    }

    const Node* lvs_node = find_node(*vg_object, "logical_volumes");
    if (lvs_node) {
      if (const Object* lvs_object = as_object(*lvs_node)) {
        for (const auto& [lv_name, lv_node] : *lvs_object) {
          const Object* lv_object = as_object(lv_node);
          if (!lv_object) {
            continue;
          }
          LogicalVolume lv;
          lv.name = lv_name;
          lv.vg_name = vg.name;
          lv.uuid = get_string(*lv_object, "id");
          lv.status = get_string_array(*lv_object, "status");
          lv.visible = std::find(lv.status.begin(), lv.status.end(), "VISIBLE") != lv.status.end() &&
                       !lv.name.empty() && lv.name.front() != '[';
          lv.unsupported_reason = unsupported_status_reason(lv.status);
          if (!lv.unsupported_reason.empty()) {
            lv.supported = false;
          }

          for (const auto& [segment_name, segment_node] : *lv_object) {
            if (segment_name.rfind("segment", 0) != 0) {
              continue;
            }
            const Object* segment_object = as_object(segment_node);
            if (!segment_object) {
              continue;
            }

            Segment segment;
            segment.start_extent = get_number(*segment_object, "start_extent");
            segment.extent_count = get_number(*segment_object, "extent_count");
            segment.type = get_string(*segment_object, "type");
            segment.lv_start_bytes = segment.start_extent * vg.extent_size_bytes;
            segment.length_bytes = segment.extent_count * vg.extent_size_bytes;

            std::uint64_t stripe_count = get_number(*segment_object, "stripe_count", 0);
            if (segment.type == "linear") {
              stripe_count = 1;
            }

            const Node* stripes_node = find_node(*segment_object, "stripes");
            const Array* stripes = stripes_node ? as_array(*stripes_node) : nullptr;
            if ((segment.type == "striped" || segment.type == "linear") && stripe_count == 1 && stripes &&
                stripes->size() >= 2) {
              if (const std::string* pv_name = as_string((*stripes)[0])) {
                segment.pv_name = *pv_name;
              }
              if (const std::uint64_t* pv_start = as_number((*stripes)[1])) {
                segment.pv_start_extent = *pv_start;
              }
            } else {
              lv.supported = false;
              lv.unsupported_reason = "unsupported segment type " + segment.type;
            }
            lv.size_bytes += segment.length_bytes;
            lv.segments.push_back(std::move(segment));
          }
          vg.lvs.push_back(std::move(lv));
        }
      }
    }

    return Result<VolumeGroup>::success(std::move(vg));
  } catch (const std::exception& e) {
    return Result<VolumeGroup>::failure(e.what());
  }
}

}  // namespace winlvm
