#include "protocol.h"

#include <charconv>
#include <cstring>
#include <limits>
#include <map>
#include <utility>

namespace winlvm::service {
namespace {

constexpr std::size_t kMaxJsonDepth = 16;

void write_u32_le(std::vector<std::byte>& out, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::byte>((value >> (i * 8U)) & 0xFFU));
  }
}

void write_u64_le(std::vector<std::byte>& out, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::byte>((value >> (i * 8U)) & 0xFFU));
  }
}

std::uint32_t read_u32_le(const std::byte* p) {
  std::uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[i])) << (i * 8U);
  }
  return v;
}

std::uint64_t read_u64_le(const std::byte* p) {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i])) << (i * 8U);
  }
  return v;
}

std::string u64_to_ascii(std::uint64_t value) {
  char buf[32];
  auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
  if (ec != std::errc{}) return "0";
  return std::string(buf, static_cast<std::size_t>(ptr - buf));
}

void append_json_string(std::string& out, std::string_view s) {
  out.push_back('"');
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20U) {
          static constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[(c >> 4U) & 0x0FU]);
          out.push_back(kHex[c & 0x0FU]);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
}

void append_json_string_array(std::string& out, const std::vector<std::string>& items) {
  out.push_back('[');
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) out.push_back(',');
    append_json_string(out, items[i]);
  }
  out.push_back(']');
}

struct JsonValue {
  enum class Kind { Null, Bool, UInt, String, Array, Object };
  Kind kind = Kind::Null;
  bool bool_val = false;
  std::uint64_t uint_val = 0;
  std::string str_val;
  std::vector<JsonValue> arr_val;
  std::map<std::string, JsonValue> obj_val;

  const JsonValue* find(const std::string& key) const {
    if (kind != Kind::Object) return nullptr;
    auto it = obj_val.find(key);
    return it == obj_val.end() ? nullptr : &it->second;
  }
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view src) : src_(src) {}

  bool parse(JsonValue& out, std::string& error) {
    skip_ws();
    if (!parse_value(out, 0, error)) return false;
    skip_ws();
    if (pos_ != src_.size()) {
      error = "unexpected trailing characters in JSON payload";
      return false;
    }
    return true;
  }

 private:
  void skip_ws() {
    while (pos_ < src_.size()) {
      const char c = src_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool parse_value(JsonValue& out, std::size_t depth, std::string& error) {
    if (depth > kMaxJsonDepth) {
      error = "JSON nesting depth exceeded";
      return false;
    }
    skip_ws();
    if (pos_ >= src_.size()) {
      error = "unexpected end of JSON input";
      return false;
    }
    const char c = src_[pos_];
    if (c == '{') return parse_object(out, depth + 1, error);
    if (c == '[') return parse_array(out, depth + 1, error);
    if (c == '"') {
      out = JsonValue{};
      out.kind = JsonValue::Kind::String;
      return parse_string(out.str_val, error);
    }
    if (c == 't' && src_.substr(pos_, 4) == "true") {
      pos_ += 4;
      out = JsonValue{};
      out.kind = JsonValue::Kind::Bool;
      out.bool_val = true;
      return true;
    }
    if (c == 'f' && src_.substr(pos_, 5) == "false") {
      pos_ += 5;
      out = JsonValue{};
      out.kind = JsonValue::Kind::Bool;
      out.bool_val = false;
      return true;
    }
    if (c == 'n' && src_.substr(pos_, 4) == "null") {
      pos_ += 4;
      out = JsonValue{};
      out.kind = JsonValue::Kind::Null;
      return true;
    }
    if (c >= '0' && c <= '9') {
      return parse_uint(out, error);
    }
    error = "invalid token in JSON payload";
    return false;
  }

  bool parse_uint(JsonValue& out, std::string& error) {
    const std::size_t start = pos_;
    if (src_[pos_] == '0') {
      ++pos_;
      if (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
        error = "leading zero in JSON number";
        return false;
      }
    } else {
      while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
        ++pos_;
      }
    }
    if (pos_ < src_.size() && (src_[pos_] == '.' || src_[pos_] == 'e' || src_[pos_] == 'E')) {
      error = "floating-point numbers are not supported in protocol payloads";
      return false;
    }
    std::uint64_t val = 0;
    const auto [ptr, ec] = std::from_chars(src_.data() + start, src_.data() + pos_, val);
    if (ec != std::errc{} || ptr != src_.data() + pos_) {
      error = "integer out of range in JSON payload";
      return false;
    }
    out = JsonValue{};
    out.kind = JsonValue::Kind::UInt;
    out.uint_val = val;
    return true;
  }

  static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  static void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7FU) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFU) {
      out.push_back(static_cast<char>(0xC0U | ((cp >> 6U) & 0x1FU)));
      out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
      out.push_back(static_cast<char>(0xE0U | ((cp >> 12U) & 0x0FU)));
      out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
  }

  bool parse_string(std::string& out, std::string& error) {
    if (pos_ >= src_.size() || src_[pos_] != '"') {
      error = "expected '\"' in JSON string";
      return false;
    }
    ++pos_;
    out.clear();
    while (pos_ < src_.size()) {
      const unsigned char c = static_cast<unsigned char>(src_[pos_++]);
      if (c == '"') return true;
      if (c < 0x20U) {
        error = "unescaped control character in JSON string";
        return false;
      }
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        continue;
      }
      if (pos_ >= src_.size()) {
        error = "unterminated escape sequence in JSON string";
        return false;
      }
      const char esc = src_[pos_++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          if (pos_ + 4 > src_.size()) {
            error = "truncated \\u escape in JSON string";
            return false;
          }
          std::uint32_t cp = 0;
          for (int i = 0; i < 4; ++i) {
            const int d = hex_digit(src_[pos_++]);
            if (d < 0) {
              error = "invalid hex digit in \\u escape";
              return false;
            }
            cp = (cp << 4U) | static_cast<std::uint32_t>(d);
          }
          if (cp == 0) {
            error = "embedded NUL character in JSON string";
            return false;
          }
          append_utf8(out, cp);
          break;
        }
        default:
          error = "invalid escape character in JSON string";
          return false;
      }
    }
    error = "unterminated JSON string";
    return false;
  }

  bool parse_array(JsonValue& out, std::size_t depth, std::string& error) {
    ++pos_;  // '['
    out = JsonValue{};
    out.kind = JsonValue::Kind::Array;
    skip_ws();
    if (pos_ < src_.size() && src_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (pos_ < src_.size()) {
      JsonValue elem;
      if (!parse_value(elem, depth, error)) return false;
      out.arr_val.push_back(std::move(elem));
      skip_ws();
      if (pos_ >= src_.size()) break;
      if (src_[pos_] == ']') {
        ++pos_;
        return true;
      }
      if (src_[pos_] != ',') {
        error = "expected ',' or ']' in JSON array";
        return false;
      }
      ++pos_;
    }
    error = "unterminated JSON array";
    return false;
  }

  bool parse_object(JsonValue& out, std::size_t depth, std::string& error) {
    ++pos_;  // '{'
    out = JsonValue{};
    out.kind = JsonValue::Kind::Object;
    skip_ws();
    if (pos_ < src_.size() && src_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (pos_ < src_.size()) {
      skip_ws();
      std::string key;
      if (!parse_string(key, error)) return false;
      skip_ws();
      if (pos_ >= src_.size() || src_[pos_] != ':') {
        error = "expected ':' after JSON object key";
        return false;
      }
      ++pos_;
      JsonValue val;
      if (!parse_value(val, depth, error)) return false;
      out.obj_val[std::move(key)] = std::move(val);
      skip_ws();
      if (pos_ >= src_.size()) break;
      if (src_[pos_] == '}') {
        ++pos_;
        return true;
      }
      if (src_[pos_] != ',') {
        error = "expected ',' or '}' in JSON object";
        return false;
      }
      ++pos_;
    }
    error = "unterminated JSON object";
    return false;
  }

  std::string_view src_;
  std::size_t pos_ = 0;
};

bool parse_json_object(std::string_view json, JsonValue& out, std::string& error) {
  JsonParser parser(json);
  if (!parser.parse(out, error)) return false;
  if (out.kind != JsonValue::Kind::Object) {
    error = "expected JSON object";
    return false;
  }
  return true;
}

bool get_opt_string(const JsonValue& obj, const std::string& key, std::string& out,
                    std::string& error) {
  const JsonValue* v = obj.find(key);
  if (!v) return true;
  if (v->kind != JsonValue::Kind::String) {
    error = "field '" + key + "' must be a string";
    return false;
  }
  out = v->str_val;
  return true;
}

bool get_opt_bool(const JsonValue& obj, const std::string& key, bool& out, std::string& error) {
  const JsonValue* v = obj.find(key);
  if (!v) return true;
  if (v->kind != JsonValue::Kind::Bool) {
    error = "field '" + key + "' must be a boolean";
    return false;
  }
  out = v->bool_val;
  return true;
}

bool get_opt_u64(const JsonValue& obj, const std::string& key, std::uint64_t& out,
                 std::string& error) {
  const JsonValue* v = obj.find(key);
  if (!v) return true;
  if (v->kind != JsonValue::Kind::UInt) {
    error = "field '" + key + "' must be an unsigned integer";
    return false;
  }
  out = v->uint_val;
  return true;
}

bool get_opt_u32(const JsonValue& obj, const std::string& key, std::uint32_t& out,
                 std::string& error) {
  const JsonValue* v = obj.find(key);
  if (!v) return true;
  if (v->kind != JsonValue::Kind::UInt || v->uint_val > std::numeric_limits<std::uint32_t>::max()) {
    error = "field '" + key + "' must be a 32-bit unsigned integer";
    return false;
  }
  out = static_cast<std::uint32_t>(v->uint_val);
  return true;
}

bool get_opt_string_array(const JsonValue& obj, const std::string& key,
                          std::vector<std::string>& out, std::string& error) {
  const JsonValue* v = obj.find(key);
  if (!v) return true;
  if (v->kind != JsonValue::Kind::Array) {
    error = "field '" + key + "' must be an array";
    return false;
  }
  out.clear();
  out.reserve(v->arr_val.size());
  for (const auto& item : v->arr_val) {
    if (item.kind != JsonValue::Kind::String) {
      error = "elements of '" + key + "' must be strings";
      return false;
    }
    out.push_back(item.str_val);
  }
  return true;
}

bool validate_lv_target_fields(const std::string& vg_name,
                               const std::string& lv_name,
                               const std::string& lv_uuid,
                               std::string& error) {
  const bool has_uuid = !lv_uuid.empty();
  const bool has_name = !vg_name.empty() && !lv_name.empty();
  if (!has_uuid && !has_name) {
    error = "target LV requires either lv_uuid or both vg_name and lv_name";
    return false;
  }
  if (has_uuid && !is_valid_uuid_chars(lv_uuid)) {
    error = "invalid lv_uuid format";
    return false;
  }
  if (!vg_name.empty() && !is_valid_lvm_name(vg_name)) {
    error = "invalid vg_name";
    return false;
  }
  if (!lv_name.empty() && !is_valid_lvm_name(lv_name)) {
    error = "invalid lv_name";
    return false;
  }
  return true;
}

}  // namespace

bool is_valid_operation(std::uint32_t raw_op) noexcept {
  switch (static_cast<Operation>(raw_op)) {
    case Operation::Inventory:
    case Operation::Refresh:
    case Operation::GetTopology:
    case Operation::Rescan:
    case Operation::ActivateLv:
    case Operation::DeactivateLv:
    case Operation::SetDriveLetter:
    case Operation::SetLvReadOnly:
    case Operation::CreatePv:
    case Operation::CreateVg:
    case Operation::CreateLv:
    case Operation::ResizeLv:
    case Operation::RenameLv:
    case Operation::RemoveLv:
      return true;
  }
  return false;
}

bool is_valid_error_code(std::uint32_t raw_error) noexcept {
  switch (static_cast<ErrorCode>(raw_error)) {
    case ErrorCode::Ok:
    case ErrorCode::MalformedFrame:
    case ErrorCode::Unsupported:
    case ErrorCode::DriverUnavailable:
    case ErrorCode::InvalidArgument:
    case ErrorCode::AccessDenied:
    case ErrorCode::NotFound:
    case ErrorCode::Busy:
    case ErrorCode::OperationFailed:
      return true;
  }
  return false;
}

bool operation_requires_elevation(Operation operation) noexcept {
  switch (operation) {
    case Operation::Inventory:
    case Operation::Refresh:
    case Operation::GetTopology:
    case Operation::Rescan:
      return false;
    case Operation::ActivateLv:
    case Operation::DeactivateLv:
    case Operation::SetDriveLetter:
    case Operation::SetLvReadOnly:
    case Operation::CreatePv:
    case Operation::CreateVg:
    case Operation::CreateLv:
    case Operation::ResizeLv:
    case Operation::RenameLv:
    case Operation::RemoveLv:
      return true;
  }
  return true;
}

bool operation_requires_driver(Operation operation) noexcept {
  switch (operation) {
    case Operation::Inventory:
    case Operation::Refresh:
    case Operation::Rescan:
    case Operation::ActivateLv:
    case Operation::DeactivateLv:
    case Operation::SetDriveLetter:
      return true;
    case Operation::GetTopology:
    case Operation::SetLvReadOnly:
    case Operation::CreatePv:
    case Operation::CreateVg:
    case Operation::CreateLv:
    case Operation::ResizeLv:
    case Operation::RenameLv:
    case Operation::RemoveLv:
      return false;
  }
  return true;
}

bool is_valid_lvm_name(std::string_view name) {
  if (name.empty() || name.size() > 128U) return false;
  if (name.front() == '-' || name == "." || name == "..") return false;
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-' || c == '.';
    if (!ok) return false;
  }
  return true;
}

bool is_valid_uuid_chars(std::string_view uuid) {
  std::size_t alnum = 0;
  for (char c : uuid) {
    if (c == '-') continue;
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!ok) return false;
    ++alnum;
  }
  return alnum == 32U;
}

bool is_valid_device_path(std::string_view path) {
  if (path.empty() || path.size() >= 256U || path.front() == '-') return false;
  for (char ch : path) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c < 0x20U || c == '"' || c == ';' || c == '|' || c == '&' || c == '<' || c == '>') {
      return false;
    }
  }
  return true;
}

bool is_valid_create_segment_type(std::string_view segment_type) {
  return segment_type == "linear" || segment_type == "striped" ||
         segment_type == "thin-pool" || segment_type == "thin" ||
         segment_type == "snapshot";
}

bool normalize_drive_letter(std::string_view input, char& out_letter) {
  out_letter = '\0';
  if (input.empty() || input == "-" || input == "none" || input == "None" || input == "NONE") {
    return true;
  }
  if (input.size() > 3U) return false;
  if (input.size() >= 2U && input[1] != ':') return false;
  if (input.size() == 3U && input[2] != '\\' && input[2] != '/') return false;
  char c = input[0];
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
  if (c < 'D' || c > 'Z') return false;
  out_letter = c;
  return true;
}

std::vector<std::byte> encode_request_frame(Operation operation,
                                            std::uint64_t request_id,
                                            std::string_view json_payload) {
  if (json_payload.size() > kMaxFramePayload) return {};
  std::vector<std::byte> frame;
  frame.reserve(kRequestHeaderSize + json_payload.size());
  write_u32_le(frame, kProtocolVersion);
  write_u32_le(frame, static_cast<std::uint32_t>(operation));
  write_u32_le(frame, static_cast<std::uint32_t>(json_payload.size()));
  write_u64_le(frame, request_id);
  for (char c : json_payload) {
    frame.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  }
  return frame;
}

ParseResult parse_frame(std::span<const std::byte> frame) {
  if (frame.size() < kRequestHeaderSize) {
    return {false, {}, {}, ErrorCode::MalformedFrame};
  }
  const std::uint32_t version = read_u32_le(frame.data());
  const std::uint32_t raw_op = read_u32_le(frame.data() + 4);
  const std::uint32_t payload_size = read_u32_le(frame.data() + 8);
  const std::uint64_t request_id = read_u64_le(frame.data() + 12);

  if (version != kProtocolVersion || !is_valid_operation(raw_op) ||
      payload_size > kMaxFramePayload ||
      static_cast<std::size_t>(payload_size) + kRequestHeaderSize != frame.size()) {
    return {false, {}, {}, ErrorCode::MalformedFrame};
  }

  std::string payload;
  if (payload_size > 0) {
    payload.resize(payload_size);
    std::memcpy(payload.data(), frame.data() + kRequestHeaderSize, payload_size);
  }
  return {true,
          Request{static_cast<Operation>(raw_op), request_id, payload_size},
          std::move(payload),
          ErrorCode::Ok};
}

std::vector<std::byte> encode_response_frame(ErrorCode error,
                                             std::string_view json_payload,
                                             std::uint32_t flags) {
  if (json_payload.size() > kMaxFramePayload) return {};
  std::vector<std::byte> frame;
  frame.reserve(kResponseHeaderSize + json_payload.size());
  write_u32_le(frame, kProtocolVersion);
  write_u32_le(frame, static_cast<std::uint32_t>(error));
  write_u32_le(frame, static_cast<std::uint32_t>(json_payload.size()));
  write_u32_le(frame, flags);
  for (char c : json_payload) {
    frame.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  }
  return frame;
}

ResponseParseResult parse_response_frame(std::span<const std::byte> frame) {
  if (frame.size() < kResponseHeaderSize) {
    return {false, ErrorCode::MalformedFrame, 0, {}};
  }
  const std::uint32_t version = read_u32_le(frame.data());
  const std::uint32_t raw_err = read_u32_le(frame.data() + 4);
  const std::uint32_t payload_size = read_u32_le(frame.data() + 8);
  const std::uint32_t flags = read_u32_le(frame.data() + 12);

  if (version != kProtocolVersion || !is_valid_error_code(raw_err) ||
      payload_size > kMaxFramePayload ||
      static_cast<std::size_t>(payload_size) + kResponseHeaderSize != frame.size()) {
    return {false, ErrorCode::MalformedFrame, 0, {}};
  }

  std::string payload;
  if (payload_size > 0) {
    payload.resize(payload_size);
    std::memcpy(payload.data(), frame.data() + kResponseHeaderSize, payload_size);
  }
  return {true, static_cast<ErrorCode>(raw_err), flags, std::move(payload)};
}

ErrorCode validate_request(const Request& request,
                           std::string_view payload,
                           bool driver_available,
                           bool caller_elevated,
                           std::string& error_out) {
  error_out.clear();
  if (!is_valid_operation(static_cast<std::uint32_t>(request.operation))) {
    error_out = "unknown operation";
    return ErrorCode::MalformedFrame;
  }
  if (operation_requires_elevation(request.operation) && !caller_elevated) {
    error_out = "administrator privileges are required for this operation";
    return ErrorCode::AccessDenied;
  }
  if (operation_requires_driver(request.operation) && !driver_available) {
    error_out = "WinLVM kernel driver is unavailable";
    return ErrorCode::DriverUnavailable;
  }

  switch (request.operation) {
    case Operation::Inventory:
    case Operation::Refresh:
    case Operation::GetTopology:
    case Operation::Rescan:
      if (!payload.empty() && payload != "{}") {
        JsonValue dummy;
        if (!parse_json_object(payload, dummy, error_out)) {
          return ErrorCode::InvalidArgument;
        }
      }
      return ErrorCode::Ok;
    case Operation::ActivateLv:
    case Operation::DeactivateLv:
    case Operation::RemoveLv: {
      LvTargetRequest req;
      return decode_lv_target(payload, req, error_out) ? ErrorCode::Ok : ErrorCode::InvalidArgument;
    }
    case Operation::SetDriveLetter: {
      SetDriveLetterRequest req;
      return decode_set_drive_letter(payload, req, error_out) ? ErrorCode::Ok
                                                              : ErrorCode::InvalidArgument;
    }
    case Operation::SetLvReadOnly: {
      SetLvReadOnlyRequest req;
      return decode_set_lv_read_only(payload, req, error_out) ? ErrorCode::Ok
                                                              : ErrorCode::InvalidArgument;
    }
    case Operation::CreatePv: {
      CreatePvRequest req;
      return decode_create_pv(payload, req, error_out) ? ErrorCode::Ok : ErrorCode::InvalidArgument;
    }
    case Operation::CreateVg: {
      CreateVgRequest req;
      return decode_create_vg(payload, req, error_out) ? ErrorCode::Ok : ErrorCode::InvalidArgument;
    }
    case Operation::CreateLv: {
      CreateLvRequest req;
      return decode_create_lv(payload, req, error_out) ? ErrorCode::Ok : ErrorCode::InvalidArgument;
    }
    case Operation::ResizeLv: {
      ResizeLvRequest req;
      return decode_resize_lv(payload, req, error_out) ? ErrorCode::Ok : ErrorCode::InvalidArgument;
    }
    case Operation::RenameLv: {
      RenameLvRequest req;
      return decode_rename_lv(payload, req, error_out) ? ErrorCode::Ok : ErrorCode::InvalidArgument;
    }
  }
  error_out = "unsupported operation";
  return ErrorCode::Unsupported;
}

std::string_view operation_name(Operation operation) noexcept {
  switch (operation) {
    case Operation::Inventory: return "inventory";
    case Operation::Refresh: return "refresh";
    case Operation::GetTopology: return "get-topology";
    case Operation::Rescan: return "rescan";
    case Operation::ActivateLv: return "activate-lv";
    case Operation::DeactivateLv: return "deactivate-lv";
    case Operation::SetDriveLetter: return "set-drive-letter";
    case Operation::SetLvReadOnly: return "set-lv-read-only";
    case Operation::CreatePv: return "create-pv";
    case Operation::CreateVg: return "create-vg";
    case Operation::CreateLv: return "create-lv";
    case Operation::ResizeLv: return "resize-lv";
    case Operation::RenameLv: return "rename-lv";
    case Operation::RemoveLv: return "remove-lv";
  }
  return "unknown";
}

std::string_view error_name(ErrorCode error) noexcept {
  switch (error) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::MalformedFrame: return "malformed-frame";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::DriverUnavailable: return "driver-unavailable";
    case ErrorCode::InvalidArgument: return "invalid-argument";
    case ErrorCode::AccessDenied: return "access-denied";
    case ErrorCode::NotFound: return "not-found";
    case ErrorCode::Busy: return "busy";
    case ErrorCode::OperationFailed: return "operation-failed";
  }
  return "unknown-error";
}

std::string encode_topology(const Topology& t) {
  std::string out;
  out.reserve(1024);
  out += "{\"driver_available\":";
  out += t.driver_available ? "true" : "false";
  out += ",\"service_connected\":";
  out += t.service_connected ? "true" : "false";
  out += ",\"caller_elevated\":";
  out += t.caller_elevated ? "true" : "false";
  out += ",\"driver_vg_records\":";
  out += u64_to_ascii(t.driver_vg_records);

  out += ",\"disks\":[";
  for (std::size_t i = 0; i < t.disks.size(); ++i) {
    if (i > 0) out.push_back(',');
    const auto& d = t.disks[i];
    out += "{\"device_path\":";
    append_json_string(out, d.device_path);
    out += ",\"nt_target\":";
    append_json_string(out, d.nt_target);
    out += ",\"kind\":";
    append_json_string(out, d.kind);
    out += ",\"size_bytes\":" + u64_to_ascii(d.size_bytes);
    out += ",\"logical_sector\":" + u64_to_ascii(d.logical_sector);
    out += ",\"physical_sector\":" + u64_to_ascii(d.physical_sector);
    out += ",\"is_pv\":";
    out += d.is_pv ? "true" : "false";
    out += ",\"is_system_disk\":";
    out += d.is_system_disk ? "true" : "false";
    out += ",\"pv_uuid\":";
    append_json_string(out, d.pv_uuid);
    out += ",\"vg_name\":";
    append_json_string(out, d.vg_name);
    out.push_back('}');
  }

  out += "],\"pvs\":[";
  for (std::size_t i = 0; i < t.pvs.size(); ++i) {
    if (i > 0) out.push_back(',');
    const auto& p = t.pvs[i];
    out += "{\"device_path\":";
    append_json_string(out, p.device_path);
    out += ",\"pv_uuid\":";
    append_json_string(out, p.pv_uuid);
    out += ",\"vg_name\":";
    append_json_string(out, p.vg_name);
    out += ",\"pv_key\":";
    append_json_string(out, p.pv_key);
    out += ",\"size_bytes\":" + u64_to_ascii(p.size_bytes);
    out += ",\"free_bytes\":" + u64_to_ascii(p.free_bytes);
    out += ",\"mda_count\":" + u64_to_ascii(p.mda_count);
    out += ",\"allocatable\":";
    out += p.allocatable ? "true" : "false";
    out += ",\"missing\":";
    out += p.missing ? "true" : "false";
    out.push_back('}');
  }

  out += "],\"vgs\":[";
  for (std::size_t i = 0; i < t.vgs.size(); ++i) {
    if (i > 0) out.push_back(',');
    const auto& v = t.vgs[i];
    out += "{\"name\":";
    append_json_string(out, v.name);
    out += ",\"vg_uuid\":";
    append_json_string(out, v.vg_uuid);
    out += ",\"extent_size_bytes\":" + u64_to_ascii(v.extent_size_bytes);
    out += ",\"size_bytes\":" + u64_to_ascii(v.size_bytes);
    out += ",\"free_bytes\":" + u64_to_ascii(v.free_bytes);
    out += ",\"pv_count\":" + u64_to_ascii(v.pv_count);
    out += ",\"lv_count\":" + u64_to_ascii(v.lv_count);
    out += ",\"seqno\":" + u64_to_ascii(v.seqno);
    out += ",\"read_only\":";
    out += v.read_only ? "true" : "false";
    out += ",\"exported\":";
    out += v.exported ? "true" : "false";
    out += ",\"system_id\":";
    append_json_string(out, v.system_id);
    out += ",\"tags\":";
    append_json_string_array(out, v.tags);
    out.push_back('}');
  }

  out += "],\"lvs\":[";
  for (std::size_t i = 0; i < t.lvs.size(); ++i) {
    if (i > 0) out.push_back(',');
    const auto& lv = t.lvs[i];
    out += "{\"vg_name\":";
    append_json_string(out, lv.vg_name);
    out += ",\"lv_name\":";
    append_json_string(out, lv.lv_name);
    out += ",\"lv_uuid\":";
    append_json_string(out, lv.lv_uuid);
    out += ",\"segment_type\":";
    append_json_string(out, lv.segment_type);
    out += ",\"size_bytes\":" + u64_to_ascii(lv.size_bytes);
    out += ",\"stripe_count\":" + u64_to_ascii(lv.stripe_count);
    out += ",\"active\":";
    out += lv.active ? "true" : "false";
    out += ",\"read_only\":";
    out += lv.read_only ? "true" : "false";
    out += ",\"internal\":";
    out += lv.internal ? "true" : "false";
    out += ",\"supported\":";
    out += lv.supported ? "true" : "false";
    out += ",\"backing_info\":";
    append_json_string(out, lv.backing_info);
    out += ",\"filesystem\":";
    append_json_string(out, lv.filesystem);
    out += ",\"volume_label\":";
    append_json_string(out, lv.volume_label);
    out += ",\"drive_letter\":";
    append_json_string(out, lv.drive_letter);
    out += ",\"volume_guid_path\":";
    append_json_string(out, lv.volume_guid_path);
    out += ",\"tags\":";
    append_json_string_array(out, lv.tags);
    out.push_back('}');
  }

  out += "]}";
  return out;
}

bool decode_topology(std::string_view json, Topology& out, std::string& error) {
  JsonValue root;
  if (!parse_json_object(json, root, error)) return false;

  Topology t;
  if (!get_opt_bool(root, "driver_available", t.driver_available, error) ||
      !get_opt_bool(root, "service_connected", t.service_connected, error) ||
      !get_opt_bool(root, "caller_elevated", t.caller_elevated, error) ||
      !get_opt_u32(root, "driver_vg_records", t.driver_vg_records, error)) {
    return false;
  }

  if (const JsonValue* disks = root.find("disks")) {
    if (disks->kind != JsonValue::Kind::Array) {
      error = "'disks' must be an array";
      return false;
    }
    for (const auto& item : disks->arr_val) {
      if (item.kind != JsonValue::Kind::Object) {
        error = "disk entry must be an object";
        return false;
      }
      DiskInfo d;
      if (!get_opt_string(item, "device_path", d.device_path, error) ||
          !get_opt_string(item, "nt_target", d.nt_target, error) ||
          !get_opt_string(item, "kind", d.kind, error) ||
          !get_opt_u64(item, "size_bytes", d.size_bytes, error) ||
          !get_opt_u32(item, "logical_sector", d.logical_sector, error) ||
          !get_opt_u32(item, "physical_sector", d.physical_sector, error) ||
          !get_opt_bool(item, "is_pv", d.is_pv, error) ||
          !get_opt_bool(item, "is_system_disk", d.is_system_disk, error) ||
          !get_opt_string(item, "pv_uuid", d.pv_uuid, error) ||
          !get_opt_string(item, "vg_name", d.vg_name, error)) {
        return false;
      }
      t.disks.push_back(std::move(d));
    }
  }

  if (const JsonValue* pvs = root.find("pvs")) {
    if (pvs->kind != JsonValue::Kind::Array) {
      error = "'pvs' must be an array";
      return false;
    }
    for (const auto& item : pvs->arr_val) {
      if (item.kind != JsonValue::Kind::Object) {
        error = "pv entry must be an object";
        return false;
      }
      PvInfo p;
      if (!get_opt_string(item, "device_path", p.device_path, error) ||
          !get_opt_string(item, "pv_uuid", p.pv_uuid, error) ||
          !get_opt_string(item, "vg_name", p.vg_name, error) ||
          !get_opt_string(item, "pv_key", p.pv_key, error) ||
          !get_opt_u64(item, "size_bytes", p.size_bytes, error) ||
          !get_opt_u64(item, "free_bytes", p.free_bytes, error) ||
          !get_opt_u32(item, "mda_count", p.mda_count, error) ||
          !get_opt_bool(item, "allocatable", p.allocatable, error) ||
          !get_opt_bool(item, "missing", p.missing, error)) {
        return false;
      }
      t.pvs.push_back(std::move(p));
    }
  }

  if (const JsonValue* vgs = root.find("vgs")) {
    if (vgs->kind != JsonValue::Kind::Array) {
      error = "'vgs' must be an array";
      return false;
    }
    for (const auto& item : vgs->arr_val) {
      if (item.kind != JsonValue::Kind::Object) {
        error = "vg entry must be an object";
        return false;
      }
      VgInfo v;
      if (!get_opt_string(item, "name", v.name, error) ||
          !get_opt_string(item, "vg_uuid", v.vg_uuid, error) ||
          !get_opt_u64(item, "extent_size_bytes", v.extent_size_bytes, error) ||
          !get_opt_u64(item, "size_bytes", v.size_bytes, error) ||
          !get_opt_u64(item, "free_bytes", v.free_bytes, error) ||
          !get_opt_u32(item, "pv_count", v.pv_count, error) ||
          !get_opt_u32(item, "lv_count", v.lv_count, error) ||
          !get_opt_u64(item, "seqno", v.seqno, error) ||
          !get_opt_bool(item, "read_only", v.read_only, error) ||
          !get_opt_bool(item, "exported", v.exported, error) ||
          !get_opt_string(item, "system_id", v.system_id, error) ||
          !get_opt_string_array(item, "tags", v.tags, error)) {
        return false;
      }
      t.vgs.push_back(std::move(v));
    }
  }

  if (const JsonValue* lvs = root.find("lvs")) {
    if (lvs->kind != JsonValue::Kind::Array) {
      error = "'lvs' must be an array";
      return false;
    }
    for (const auto& item : lvs->arr_val) {
      if (item.kind != JsonValue::Kind::Object) {
        error = "lv entry must be an object";
        return false;
      }
      LvInfo lv;
      if (!get_opt_string(item, "vg_name", lv.vg_name, error) ||
          !get_opt_string(item, "lv_name", lv.lv_name, error) ||
          !get_opt_string(item, "lv_uuid", lv.lv_uuid, error) ||
          !get_opt_string(item, "segment_type", lv.segment_type, error) ||
          !get_opt_u64(item, "size_bytes", lv.size_bytes, error) ||
          !get_opt_u32(item, "stripe_count", lv.stripe_count, error) ||
          !get_opt_bool(item, "active", lv.active, error) ||
          !get_opt_bool(item, "read_only", lv.read_only, error) ||
          !get_opt_bool(item, "internal", lv.internal, error) ||
          !get_opt_bool(item, "supported", lv.supported, error) ||
          !get_opt_string(item, "backing_info", lv.backing_info, error) ||
          !get_opt_string(item, "filesystem", lv.filesystem, error) ||
          !get_opt_string(item, "volume_label", lv.volume_label, error) ||
          !get_opt_string(item, "drive_letter", lv.drive_letter, error) ||
          !get_opt_string(item, "volume_guid_path", lv.volume_guid_path, error) ||
          !get_opt_string_array(item, "tags", lv.tags, error)) {
        return false;
      }
      t.lvs.push_back(std::move(lv));
    }
  }

  out = std::move(t);
  return true;
}

std::string encode_action_response(const ActionResponse& r) {
  std::string out = "{\"error\":" + u64_to_ascii(static_cast<std::uint32_t>(r.error));
  out += ",\"message\":";
  append_json_string(out, r.message);
  out += ",\"created_uuid\":";
  append_json_string(out, r.created_uuid);
  out.push_back('}');
  return out;
}

bool decode_action_response(std::string_view json, ActionResponse& out, std::string& error) {
  JsonValue root;
  if (!parse_json_object(json, root, error)) return false;
  ActionResponse r;
  std::uint32_t err_num = 0;
  if (!get_opt_u32(root, "error", err_num, error) ||
      !get_opt_string(root, "message", r.message, error) ||
      !get_opt_string(root, "created_uuid", r.created_uuid, error)) {
    return false;
  }
  if (!is_valid_error_code(err_num)) {
    error = "invalid error code in action response";
    return false;
  }
  r.error = static_cast<ErrorCode>(err_num);
  out = std::move(r);
  return true;
}

std::string encode_lv_target(const LvTargetRequest& req) {
  std::string out = "{\"vg_name\":";
  append_json_string(out, req.vg_name);
  out += ",\"lv_name\":";
  append_json_string(out, req.lv_name);
  out += ",\"lv_uuid\":";
  append_json_string(out, req.lv_uuid);
  out.push_back('}');
  return out;
}

bool decode_lv_target(std::string_view json, LvTargetRequest& out, std::string& error) {
  JsonValue root;
  if (!parse_json_object(json, root, error)) return false;
  LvTargetRequest r;
  if (!get_opt_string(root, "vg_name", r.vg_name, error) ||
      !get_opt_string(root, "lv_name", r.lv_name, error) ||
      !get_opt_string(root, "lv_uuid", r.lv_uuid, error)) {
    return false;
  }
  if (!validate_lv_target_fields(r.vg_name, r.lv_name, r.lv_uuid, error)) return false;
  out = std::move(r);
  return true;
}

std::string encode_set_drive_letter(const SetDriveLetterRequest& req) {
  std::string out = "{\"vg_name\":";
  append_json_string(out, req.vg_name);
  out += ",\"lv_name\":";
  append_json_string(out, req.lv_name);
  out += ",\"lv_uuid\":";
  append_json_string(out, req.lv_uuid);
  out += ",\"drive_letter\":";
  append_json_string(out, req.drive_letter);
  out.push_back('}');
  return out;
}

bool decode_set_drive_letter(std::string_view json,
                             SetDriveLetterRequest& out,
                             std::string& error) {
  JsonValue root;
  if (!parse_json_object(json, root, error)) return false;
  SetDriveLetterRequest r;
  if (!get_opt_string(root, "vg_name", r.vg_name, error) ||
      !get_opt_string(root, "lv_name", r.lv_name, error) ||
      !get_opt_string(root, "lv_uuid", r.lv_uuid, error) ||
      !get_opt_string(root, "drive_letter", r.drive_letter, error)) {
    return false;
  }
  if (!validate_lv_target_fields(r.vg_name, r.lv_name, r.lv_uuid, error)) return false;
  char norm = '\0';
  if (!normalize_drive_letter(r.drive_letter, norm)) {
    error = "drive_letter must be empty (remove) or D..Z";
    return false;
  }
  r.drive_letter = norm == '\0' ? std::string() : std::string(1, norm);
  out = std::move(r);
  return true;
}

std::string encode_set_lv_read_only(const SetLvReadOnlyRequest& req) {
  std::string out = "{\"vg_name\":";
  append_json_string(out, req.vg_name);
  out += ",\"lv_name\":";
  append_json_string(out, req.lv_name);
  out += ",\"lv_uuid\":";
  append_json_string(out, req.lv_uuid);
  out += ",\"read_only\":";
  out += req.read_only ? "true" : "false";
  out.push_back('}');
  return out;
}

bool decode_set_lv_read_only(std::string_view json,
                             SetLvReadOnlyRequest& out,
                             std::string& error) {
  JsonValue root;
  if (!parse_json_object(json, root, error)) return false;
  SetLvReadOnlyRequest r;
  if (!get_opt_string(root, "vg_name", r.vg_name, error) ||
      !get_opt_string(root, "lv_name", r.lv_name, error) ||
      !get_opt_string(root, "lv_uuid", r.lv_uuid, error) ||
      !get_opt_bool(root, "read_only", r.read_only, error)) {
    return false;
  }
  if (!root.find("read_only")) {
    error = "missing required boolean field 'read_only'";
    return false;
  }
  if (!validate_lv_target_fields(r.vg_name, r.lv_name, r.lv_uuid, error)) return false;
  out = std::move(r);
  return true;
}

std::string encode_create_pv(const CreatePvRequest& req) {
  std::string out = "{\"device_path\":";
  append_json_string(out, req.device_path);
  out += ",\"force\":";
  out += req.force ? "true" : "false";
  out.push_back('}');
  return out;
}

bool decode_create_pv(std::string_view json, CreatePvRequest& out, std::string& error) {
  JsonValue root;
  if (!parse_json_object(json, root, error)) return false;
  CreatePvRequest r;
  if (!get_opt_string(root, "device_path", r.device_path, error) ||
      !get_opt_bool(root, "force", r.force, error)) {
    return false;
  }
  if (!is_valid_device_path(r.device_path)) {
    error = "invalid device_path for pvcreate";
    return false;
  }
  out = std::move(r);
  return true;
}

std::string encode_create_vg(const CreateVgRequest& req) {
  std::string out = "{\"vg_name\":";
  append_json_string(out, req.vg_name);
  out += ",\"pv_paths\":";
  append_json_string_array(out, req.pv_paths);
  out += ",\"extent_size_bytes\":" + u64_to_ascii(req.extent_size_bytes);
  out += ",\"force\":";
  out += req.force ? "true" : "false";
  out.push_back('}');
  return out;
}

bool decode_create_vg(std::string_view json, CreateVgRequest& out, std::string& error) {
  JsonValue root;
  if (!parse_json_object(json, root, error)) return false;
  CreateVgRequest r;
  if (!get_opt_string(root, "vg_name", r.vg_name, error) ||
      !get_opt_string_array(root, "pv_paths", r.pv_paths, error) ||
      !get_opt_u64(root, "extent_size_bytes", r.extent_size_bytes, error) ||
      !get_opt_bool(root, "force", r.force, error)) {
    return false;
  }
  if (!is_valid_lvm_name(r.vg_name)) {
    error = "invalid vg_name";
    return false;
  }
  if (r.pv_paths.empty()) {
    error = "pv_paths must contain at least one physical volume";
    return false;
  }
  for (const auto& p : r.pv_paths) {
    if (!is_valid_device_path(p)) {
      error = "invalid device path in pv_paths";
      return false;
    }
  }
  if (r.extent_size_bytes != 0 &&
      (r.extent_size_bytes < 4096ULL || (r.extent_size_bytes % 512ULL) != 0 ||
       (r.extent_size_bytes & (r.extent_size_bytes - 1ULL)) != 0)) {
    error = "extent_size_bytes must be zero (default) or a power-of-two multiple of 4096";
    return false;
  }
  out = std::move(r);
  return true;
}

std::string encode_create_lv(const CreateLvRequest& req) {
  std::string out = "{\"vg_name\":";
  append_json_string(out, req.vg_name);
  out += ",\"lv_name\":";
  append_json_string(out, req.lv_name);
  out += ",\"segment_type\":";
  append_json_string(out, req.segment_type);
  out += ",\"size_bytes\":" + u64_to_ascii(req.size_bytes);
  out += ",\"virtual_size_bytes\":" + u64_to_ascii(req.virtual_size_bytes);
  out += ",\"stripes\":" + u64_to_ascii(req.stripes);
  out += ",\"stripe_size_bytes\":" + u64_to_ascii(req.stripe_size_bytes);
  out += ",\"pool_name\":";
  append_json_string(out, req.pool_name);
  out += ",\"origin_name\":";
  append_json_string(out, req.origin_name);
  out += ",\"read_only\":";
  out += req.read_only ? "true" : "false";
  out += ",\"activate\":";
  out += req.activate ? "true" : "false";
  out.push_back('}');
  return out;
}

bool decode_create_lv(std::string_view json, CreateLvRequest& out, std::string& error) {
  JsonValue root;
  if (!parse_json_object(json, root, error)) return false;
  CreateLvRequest r;
  if (!get_opt_string(root, "vg_name", r.vg_name, error) ||
      !get_opt_string(root, "lv_name", r.lv_name, error) ||
      !get_opt_string(root, "segment_type", r.segment_type, error) ||
      !get_opt_u64(root, "size_bytes", r.size_bytes, error) ||
      !get_opt_u64(root, "virtual_size_bytes", r.virtual_size_bytes, error) ||
      !get_opt_u32(root, "stripes", r.stripes, error) ||
      !get_opt_u64(root, "stripe_size_bytes", r.stripe_size_bytes, error) ||
      !get_opt_string(root, "pool_name", r.pool_name, error) ||
      !get_opt_string(root, "origin_name", r.origin_name, error) ||
      !get_opt_bool(root, "read_only", r.read_only, error) ||
      !get_opt_bool(root, "activate", r.activate, error)) {
    return false;
  }
  if (!is_valid_lvm_name(r.vg_name)) {
    error = "invalid vg_name";
    return false;
  }
  if (!is_valid_lvm_name(r.lv_name)) {
    error = "invalid lv_name";
    return false;
  }
  if (!is_valid_create_segment_type(r.segment_type)) {
    error = "segment_type must be linear, striped, thin-pool, thin, or snapshot";
    return false;
  }
  if (r.segment_type == "thin") {
    if (r.virtual_size_bytes == 0) {
      error = "thin LV requires virtual_size_bytes > 0";
      return false;
    }
    if (!is_valid_lvm_name(r.pool_name)) {
      error = "thin LV requires a valid pool_name";
      return false;
    }
  } else {
    if (r.size_bytes == 0) {
      error = "size_bytes must be > 0";
      return false;
    }
  }
  if (r.segment_type == "striped") {
    if (r.stripes < 2U || r.stripes > 128U) {
      error = "striped LV requires stripes in [2, 128]";
      return false;
    }
    if (r.stripe_size_bytes != 0 &&
        (r.stripe_size_bytes < 4096ULL || (r.stripe_size_bytes % 512ULL) != 0 ||
         (r.stripe_size_bytes & (r.stripe_size_bytes - 1ULL)) != 0)) {
      error = "stripe_size_bytes must be a power of two >= 4096";
      return false;
    }
  }
  if (r.segment_type == "snapshot" && !is_valid_lvm_name(r.origin_name)) {
    error = "snapshot LV requires a valid origin_name";
    return false;
  }
  out = std::move(r);
  return true;
}

std::string encode_resize_lv(const ResizeLvRequest& req) {
  std::string out = "{\"vg_name\":";
  append_json_string(out, req.vg_name);
  out += ",\"lv_name\":";
  append_json_string(out, req.lv_name);
  out += ",\"lv_uuid\":";
  append_json_string(out, req.lv_uuid);
  out += ",\"new_size_bytes\":" + u64_to_ascii(req.new_size_bytes);
  out.push_back('}');
  return out;
}

bool decode_resize_lv(std::string_view json, ResizeLvRequest& out, std::string& error) {
  JsonValue root;
  if (!parse_json_object(json, root, error)) return false;
  ResizeLvRequest r;
  if (!get_opt_string(root, "vg_name", r.vg_name, error) ||
      !get_opt_string(root, "lv_name", r.lv_name, error) ||
      !get_opt_string(root, "lv_uuid", r.lv_uuid, error) ||
      !get_opt_u64(root, "new_size_bytes", r.new_size_bytes, error)) {
    return false;
  }
  if (!validate_lv_target_fields(r.vg_name, r.lv_name, r.lv_uuid, error)) return false;
  if (r.new_size_bytes == 0) {
    error = "new_size_bytes must be > 0";
    return false;
  }
  out = std::move(r);
  return true;
}

std::string encode_rename_lv(const RenameLvRequest& req) {
  std::string out = "{\"vg_name\":";
  append_json_string(out, req.vg_name);
  out += ",\"old_lv_name\":";
  append_json_string(out, req.old_lv_name);
  out += ",\"new_lv_name\":";
  append_json_string(out, req.new_lv_name);
  out.push_back('}');
  return out;
}

bool decode_rename_lv(std::string_view json, RenameLvRequest& out, std::string& error) {
  JsonValue root;
  if (!parse_json_object(json, root, error)) return false;
  RenameLvRequest r;
  if (!get_opt_string(root, "vg_name", r.vg_name, error) ||
      !get_opt_string(root, "old_lv_name", r.old_lv_name, error) ||
      !get_opt_string(root, "new_lv_name", r.new_lv_name, error)) {
    return false;
  }
  if (!is_valid_lvm_name(r.vg_name)) {
    error = "invalid vg_name";
    return false;
  }
  if (!is_valid_lvm_name(r.old_lv_name)) {
    error = "invalid old_lv_name";
    return false;
  }
  if (!is_valid_lvm_name(r.new_lv_name)) {
    error = "invalid new_lv_name";
    return false;
  }
  if (r.old_lv_name == r.new_lv_name) {
    error = "new_lv_name must differ from old_lv_name";
    return false;
  }
  out = std::move(r);
  return true;
}

}  // namespace winlvm::service
