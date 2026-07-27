#include "lvmfmt/config.h"

#include <cctype>
#include <cstdlib>

namespace lvmfmt {

const Section* Section::find_section(std::string_view n) const {
  for (const auto& s : sections) {
    if (s.name == n) return &s;
  }
  return nullptr;
}

const std::vector<Value>* Section::find_entry(std::string_view key) const {
  for (const auto& e : entries) {
    if (e.first == key) return &e.second;
  }
  return nullptr;
}

std::optional<std::string> Section::string(std::string_view key) const {
  const auto* v = find_entry(key);
  if (!v || v->empty() || !v->front().is_string()) return std::nullopt;
  return v->front().string_value;
}

std::optional<std::int64_t> Section::integer(std::string_view key) const {
  const auto* v = find_entry(key);
  if (!v || v->empty() || !v->front().is_integer()) return std::nullopt;
  return v->front().integer_value;
}

std::vector<std::string> Section::string_list(std::string_view key) const {
  std::vector<std::string> out;
  const auto* v = find_entry(key);
  if (!v) return out;
  for (const auto& item : *v) {
    if (item.is_string()) out.push_back(item.string_value);
  }
  return out;
}

namespace {

class Parser {
 public:
  Parser(std::string_view text, std::string& error) : t_(text), err_(error) {}

  bool parse_root(Section& root) {
    skip_trivia();
    while (pos_ < t_.size()) {
      if (!parse_member(root)) return false;
      skip_trivia();
    }
    return true;
  }

 private:
  std::string_view t_;
  std::string& err_;
  std::size_t pos_ = 0;
  std::size_t line_ = 1;

  bool fail(const std::string& msg) {
    err_ = "line " + std::to_string(line_) + ": " + msg;
    return false;
  }

  void advance() {
    if (pos_ < t_.size() && t_[pos_] == '\n') ++line_;
    ++pos_;
  }

  void skip_trivia() {
    while (pos_ < t_.size()) {
      const char c = t_[pos_];
      if (c == '#') {
        while (pos_ < t_.size() && t_[pos_] != '\n') advance();
      } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0') {
        advance();
      } else {
        return;
      }
    }
  }

  static bool ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
           c == '.' || c == '+' || c == '/';
  }

  bool parse_identifier(std::string& out) {
    const std::size_t start = pos_;
    while (pos_ < t_.size() && ident_char(t_[pos_])) advance();
    if (pos_ == start) return fail("expected identifier");
    out.assign(t_.substr(start, pos_ - start));
    return true;
  }

  bool parse_string(std::string& out) {
    if (pos_ >= t_.size() || t_[pos_] != '"') return fail("expected '\"'");
    advance();
    out.clear();
    while (pos_ < t_.size() && t_[pos_] != '"') {
      if (t_[pos_] == '\\' && pos_ + 1 < t_.size()) {
        advance();
      }
      out.push_back(t_[pos_]);
      advance();
    }
    if (pos_ >= t_.size()) return fail("unterminated string");
    advance();  // closing quote
    return true;
  }

  bool parse_number(Value& out) {
    const std::size_t start = pos_;
    if (pos_ < t_.size() && (t_[pos_] == '-' || t_[pos_] == '+')) advance();
    bool is_float = false;
    while (pos_ < t_.size()) {
      const char c = t_[pos_];
      if (std::isdigit(static_cast<unsigned char>(c))) {
        advance();
      } else if (c == '.' || c == 'e' || c == 'E') {
        is_float = true;
        advance();
      } else {
        break;
      }
    }
    if (pos_ == start) return fail("expected number");
    const std::string text{t_.substr(start, pos_ - start)};
    if (is_float) {
      out.kind = Value::Kind::Float;
      out.float_value = std::strtod(text.c_str(), nullptr);
    } else {
      out.kind = Value::Kind::Integer;
      out.integer_value = std::strtoll(text.c_str(), nullptr, 10);
    }
    return true;
  }

  bool parse_value(Value& out) {
    skip_trivia();
    if (pos_ >= t_.size()) return fail("expected value");
    if (t_[pos_] == '"') {
      out.kind = Value::Kind::String;
      return parse_string(out.string_value);
    }
    if (t_[pos_] == '-' || t_[pos_] == '+' ||
        std::isdigit(static_cast<unsigned char>(t_[pos_]))) {
      return parse_number(out);
    }
    // Bare word: LVM emits these for a few enum-ish values. Keep as a string.
    out.kind = Value::Kind::String;
    return parse_identifier(out.string_value);
  }

  bool parse_member(Section& parent) {
    std::string name;
    if (!parse_identifier(name)) return false;
    skip_trivia();
    if (pos_ >= t_.size()) return fail("unexpected end after '" + name + "'");

    if (t_[pos_] == '{') {
      advance();
      Section child;
      child.name = name;
      skip_trivia();
      while (pos_ < t_.size() && t_[pos_] != '}') {
        if (!parse_member(child)) return false;
        skip_trivia();
      }
      if (pos_ >= t_.size()) return fail("unterminated section '" + name + "'");
      advance();  // '}'
      parent.sections.push_back(std::move(child));
      return true;
    }

    if (t_[pos_] != '=') return fail("expected '=' or '{' after '" + name + "'");
    advance();
    skip_trivia();

    std::vector<Value> values;
    if (pos_ < t_.size() && t_[pos_] == '[') {
      advance();
      skip_trivia();
      while (pos_ < t_.size() && t_[pos_] != ']') {
        Value v;
        if (!parse_value(v)) return false;
        values.push_back(std::move(v));
        skip_trivia();
        if (pos_ < t_.size() && t_[pos_] == ',') {
          advance();
          skip_trivia();
        }
      }
      if (pos_ >= t_.size()) return fail("unterminated array for '" + name + "'");
      advance();  // ']'
    } else {
      Value v;
      if (!parse_value(v)) return false;
      values.push_back(std::move(v));
    }

    parent.entries.emplace_back(std::move(name), std::move(values));
    return true;
  }
};

}  // namespace

std::optional<Section> parse_config(std::string_view text, std::string& error) {
  Section root;
  Parser parser(text, error);
  if (!parser.parse_root(root)) return std::nullopt;
  return root;
}

}  // namespace lvmfmt
