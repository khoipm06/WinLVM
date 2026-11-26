#pragma once

#include <string>
#include <vector>

namespace winlvm::installer {

namespace detail {

inline std::wstring trim_ws(const std::wstring& s) {
  size_t b = 0u;
  while (b < s.size() && (s[b] == L' ' || s[b] == L'\t')) ++b;
  size_t e = s.size();
  while (e > b && (s[e - 1u] == L' ' || s[e - 1u] == L'\t')) --e;
  return s.substr(b, e - b);
}

inline std::wstring normalize_path_entry(const std::wstring& raw) {
  std::wstring s = trim_ws(raw);
  if (s.size() >= 2u && s.front() == L'"' && s.back() == L'"') {
    s = trim_ws(s.substr(1u, s.size() - 2u));
  }
  for (wchar_t& ch : s) {
    if (ch == L'/') ch = L'\\';
    if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
  }
  while (s.size() > 3u && s.back() == L'\\') {
    s.pop_back();
  }
  if (s.size() > 1u && s.back() == L'\\' && s[1] != L':') {
    while (s.size() > 1u && s.back() == L'\\') s.pop_back();
  }
  return s;
}

inline std::vector<std::wstring> split_semicolons(const std::wstring& list) {
  std::vector<std::wstring> parts;
  size_t start = 0u;
  while (start <= list.size()) {
    const size_t sep = list.find(L';', start);
    const size_t end = (sep == std::wstring::npos) ? list.size() : sep;
    std::wstring item = trim_ws(list.substr(start, end - start));
    if (!item.empty()) {
      parts.push_back(std::move(item));
    }
    if (sep == std::wstring::npos) break;
    start = sep + 1u;
  }
  return parts;
}

inline std::wstring join_semicolons(const std::vector<std::wstring>& parts) {
  std::wstring out;
  for (size_t i = 0u; i < parts.size(); ++i) {
    if (i > 0u) out.push_back(L';');
    out += parts[i];
  }
  return out;
}

}  // namespace detail

// Adds `dir` to a semicolon-separated PATH string if no case-insensitive
// equivalent entry is already present. Returns true if the value changed.
inline bool path_list_add(const std::wstring& current,
                          const std::wstring& dir,
                          std::wstring& updated) {
  const std::wstring target_norm = detail::normalize_path_entry(dir);
  if (target_norm.empty()) {
    updated = current;
    return false;
  }
  auto parts = detail::split_semicolons(current);
  for (const auto& item : parts) {
    if (detail::normalize_path_entry(item) == target_norm) {
      updated = current;
      return false;
    }
  }
  std::wstring clean_dir = detail::trim_ws(dir);
  while (clean_dir.size() > 3u &&
         (clean_dir.back() == L'\\' || clean_dir.back() == L'/')) {
    clean_dir.pop_back();
  }
  parts.push_back(std::move(clean_dir));
  updated = detail::join_semicolons(parts);
  return true;
}

// Removes all case-insensitive equivalent entries matching `dir` from a
// semicolon-separated PATH string. Returns true if the value changed.
inline bool path_list_remove(const std::wstring& current,
                             const std::wstring& dir,
                             std::wstring& updated) {
  const std::wstring target_norm = detail::normalize_path_entry(dir);
  if (target_norm.empty()) {
    updated = current;
    return false;
  }
  auto parts = detail::split_semicolons(current);
  std::vector<std::wstring> kept;
  kept.reserve(parts.size());
  bool removed = false;
  for (auto& item : parts) {
    if (detail::normalize_path_entry(item) == target_norm) {
      removed = true;
    } else {
      kept.push_back(std::move(item));
    }
  }
  if (!removed) {
    updated = current;
    return false;
  }
  updated = detail::join_semicolons(kept);
  return true;
}

}  // namespace winlvm::installer
