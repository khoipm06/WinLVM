#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <set>
#include "../../winlvm_ioctl.h"
#include "lvmfmt/label.h"
#include "lvmfmt/metadata.h"
#include "lvmfmt/source.h"
#endif

namespace lvmfmt {

inline std::string normalize_uuid_raw32(std::string_view uuid) {
  std::string out;
  out.reserve(32);
  for (char c : uuid) {
    if (c != '-') out.push_back(c);
  }
  return out;
}

#ifdef _WIN32

struct Win32CandidateDevice {
  std::string open_path;    // e.g. "\\\\.\\PhysicalDrive11"
  std::wstring probe_path;  // e.g. L"\\Device\\WinLuksVolume11" or L"\\\\.\\PhysicalDrive1"
};

namespace detail {

inline std::wstring to_lower_w(std::wstring_view s) {
  std::wstring out;
  out.reserve(s.size());
  for (wchar_t c : s) out.push_back(static_cast<wchar_t>(std::towlower(c)));
  return out;
}

inline std::wstring ascii_to_wide(std::string_view s) {
  std::wstring out;
  out.reserve(s.size());
  for (unsigned char c : s) out.push_back(static_cast<wchar_t>(c));
  return out;
}

inline std::string wide_to_ascii(std::wstring_view s) {
  std::string out;
  out.reserve(s.size());
  for (wchar_t c : s) {
    out.push_back((static_cast<unsigned int>(c) < 128u) ? static_cast<char>(c) : '?');
  }
  return out;
}

inline bool is_digits_w(std::wstring_view s) {
  if (s.empty()) return false;
  for (wchar_t c : s) {
    if (c < L'0' || c > L'9') return false;
  }
  return true;
}

inline bool starts_with_ci_w(std::wstring_view s, std::wstring_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (std::towlower(s[i]) != std::towlower(prefix[i])) return false;
  }
  return true;
}

inline bool contains_ci_w(std::wstring_view s, std::wstring_view needle) {
  const std::wstring ls = to_lower_w(s);
  const std::wstring ln = to_lower_w(needle);
  return ls.find(ln) != std::wstring::npos;
}

inline std::wstring query_dos_target(const std::wstring& dos_name) {
  std::vector<wchar_t> target(512, L'\0');
  DWORD len = QueryDosDeviceW(dos_name.c_str(), target.data(), static_cast<DWORD>(target.size()));
  if (len == 0) return {};
  return std::wstring(target.data());
}

inline bool should_skip_target(std::wstring_view nt_target) {
  if (nt_target.empty()) return false;
  return contains_ci_w(nt_target, L"winlvm") ||
         contains_ci_w(nt_target, L"cdrom") ||
         contains_ci_w(nt_target, L"volumesnapshot");
}

}  // namespace detail

inline std::wstring resolve_win32_probe_path(const std::string& path) {
  std::string dos_name;
  if (path.rfind("\\\\.\\", 0) == 0 || path.rfind("\\\\?\\", 0) == 0) {
    dos_name = path.substr(4);
  }
  if (!dos_name.empty()) {
    const std::wstring target = detail::query_dos_target(detail::ascii_to_wide(dos_name));
    if (detail::starts_with_ci_w(target, L"\\Device\\WinLuksVolume")) {
      return target;
    }
  }
  return detail::ascii_to_wide(path);
}

inline std::vector<Win32CandidateDevice> discover_win32_candidate_devices() {
  std::vector<Win32CandidateDevice> result;
  std::set<std::wstring> seen_targets;
  std::set<std::string> seen_paths;

  auto try_add = [&](const std::wstring& dos_name) {
    const std::wstring target = detail::query_dos_target(dos_name);
    if (detail::should_skip_target(target)) return;

    const std::string open_path = "\\\\.\\" + detail::wide_to_ascii(dos_name);
    if (seen_paths.count(open_path)) return;

    const std::wstring target_key = detail::to_lower_w(target);
    if (!target_key.empty() && seen_targets.count(target_key)) return;

    std::wstring probe_path = detail::ascii_to_wide(open_path);
    if (detail::starts_with_ci_w(target, L"\\Device\\WinLuksVolume")) {
      probe_path = target;
    }

    seen_paths.insert(open_path);
    if (!target_key.empty()) seen_targets.insert(target_key);
    result.push_back({open_path, probe_path});
  };

  // Enumerate PhysicalDrive0..63 first so PhysicalDrive aliases take priority
  // over raw WinLuksVolume symlinks when deduplicating by NT target.
  for (unsigned i = 0; i < 64; ++i) {
    const std::wstring dos_name = L"PhysicalDrive" + std::to_wstring(i);
    if (!detail::query_dos_target(dos_name).empty()) {
      try_add(dos_name);
    }
  }

  // Enumerate all DOS device entries via QueryDosDeviceW for high-numbered
  // PhysicalDrive*, WinLuksVolume*, and Volume{*} entries.
  std::vector<wchar_t> names_buf(65536, L'\0');
  DWORD chars = 0;
  for (int attempt = 0; attempt < 6; ++attempt) {
    chars = QueryDosDeviceW(nullptr, names_buf.data(), static_cast<DWORD>(names_buf.size()));
    if (chars != 0) break;
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
    if (names_buf.size() >= 2U * 1024U * 1024U) break;
    names_buf.resize(names_buf.size() * 2U, L'\0');
  }

  if (chars > 0) {
    const wchar_t* p = names_buf.data();
    const wchar_t* end = p + chars;
    while (p < end && *p != L'\0') {
      const std::wstring_view name(p);
      bool match = false;
      if (detail::starts_with_ci_w(name, L"PhysicalDrive") &&
          detail::is_digits_w(name.substr(13))) {
        match = true;
      } else if (detail::starts_with_ci_w(name, L"WinLuksVolume") &&
                 detail::is_digits_w(name.substr(13))) {
        match = true;
      } else if (detail::starts_with_ci_w(name, L"Volume{") && name.back() == L'}') {
        match = true;
      }
      if (match) {
        try_add(std::wstring(name));
      }
      p += name.size() + 1;
    }
  }

  return result;
}

inline HANDLE open_winlvm_control() {
  return CreateFileW(WINLVM_USER_DEVICE_NAME,
                     GENERIC_READ | GENERIC_WRITE,
                     0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

inline bool driver_probe_device(const std::wstring& device_path) {
  if (device_path.empty() || device_path.size() >= WINLVM_PATH_WIRE_LEN) return false;
  HANDLE h = open_winlvm_control();
  if (h == INVALID_HANDLE_VALUE) return false;

  WINLVM_PROBE_DEVICE_REQUEST req{};
  req.header.version = WINLVM_ABI_VERSION;
  req.header.size = sizeof(req);
  for (std::size_t i = 0; i < device_path.size(); ++i) {
    req.device_path[i] = static_cast<WINLVM_WCHAR>(device_path[i]);
  }
  req.device_path[device_path.size()] = 0;

  DWORD returned = 0;
  const BOOL ok = DeviceIoControl(h, IOCTL_WINLVM_PROBE_DEVICE,
                                  &req, sizeof(req), nullptr, 0, &returned, nullptr);
  CloseHandle(h);
  return ok != FALSE;
}

inline bool driver_refresh() {
  HANDLE h = open_winlvm_control();
  if (h == INVALID_HANDLE_VALUE) return false;

  WINLVM_CONTROL_REQUEST req{};
  req.version = WINLVM_ABI_VERSION;
  req.size = sizeof(req);
  DWORD returned = 0;
  const BOOL ok = DeviceIoControl(h, IOCTL_WINLVM_REFRESH,
                                  &req, sizeof(req), nullptr, 0, &returned, nullptr);
  CloseHandle(h);
  return ok != FALSE;
}

inline bool driver_query_inventory(std::vector<WINLVM_INVENTORY_LV_ENTRY>& out_entries,
                                   std::uint32_t& out_record_count) {
  out_entries.clear();
  out_record_count = 0;

  HANDLE h = open_winlvm_control();
  if (h == INVALID_HANDLE_VALUE) return false;

  constexpr std::size_t kMaxEntries = 64;
  std::vector<std::uint8_t> out_buf(
      sizeof(WINLVM_INVENTORY) + kMaxEntries * sizeof(WINLVM_INVENTORY_LV_ENTRY), 0);

  WINLVM_CONTROL_REQUEST req{};
  req.version = WINLVM_ABI_VERSION;
  req.size = sizeof(req);
  DWORD returned = 0;
  const BOOL ok = DeviceIoControl(h, IOCTL_WINLVM_INVENTORY,
                                  &req, sizeof(req),
                                  out_buf.data(), static_cast<DWORD>(out_buf.size()),
                                  &returned, nullptr);
  CloseHandle(h);
  if (!ok || returned < sizeof(WINLVM_INVENTORY)) return false;

  const auto* inv = reinterpret_cast<const WINLVM_INVENTORY*>(out_buf.data());
  out_record_count = inv->record_count;

  const std::size_t avail_bytes = returned - sizeof(WINLVM_INVENTORY);
  const std::size_t avail_entries = avail_bytes / sizeof(WINLVM_INVENTORY_LV_ENTRY);
  const std::size_t count = std::min<std::size_t>(inv->lv_entry_count, avail_entries);
  if (count > 0) {
    const auto* entries = reinterpret_cast<const WINLVM_INVENTORY_LV_ENTRY*>(
        out_buf.data() + sizeof(WINLVM_INVENTORY));
    out_entries.assign(entries, entries + count);
  }
  return true;
}

inline bool driver_activate_lv(const std::string& lv_uuid, bool activate) {
  const std::string raw = normalize_uuid_raw32(lv_uuid);
  if (raw.size() != 32) return false;

  HANDLE h = open_winlvm_control();
  if (h == INVALID_HANDLE_VALUE) return false;

  WINLVM_LV_REQUEST req{};
  req.header.version = WINLVM_ABI_VERSION;
  req.header.size = sizeof(req);
  std::memcpy(req.lv_uuid, raw.data(), 32);
  req.lv_uuid[32] = '\0';

  DWORD returned = 0;
  const DWORD code = activate ? IOCTL_WINLVM_ACTIVATE_LV : IOCTL_WINLVM_DEACTIVATE_LV;
  const BOOL ok = DeviceIoControl(h, code, &req, sizeof(req), nullptr, 0, &returned, nullptr);
  CloseHandle(h);
  return ok != FALSE;
}

inline bool driver_update_lv(const std::string& lv_uuid,
                             std::uint64_t new_size_bytes,
                             std::uint32_t logical_sector,
                             std::uint32_t physical_sector) {
  const std::string raw = normalize_uuid_raw32(lv_uuid);
  if (raw.size() != 32) return false;

  HANDLE h = open_winlvm_control();
  if (h == INVALID_HANDLE_VALUE) return false;

  WINLVM_UPDATE_LV_REQUEST req{};
  req.header.version = WINLVM_ABI_VERSION;
  req.header.size = sizeof(req);
  std::memcpy(req.lv_uuid, raw.data(), 32);
  req.lv_uuid[32] = '\0';
  req.byte_size = new_size_bytes;
  req.logical_sector = logical_sector ? logical_sector : 512u;
  req.physical_sector = physical_sector ? physical_sector : req.logical_sector;

  DWORD returned = 0;
  const BOOL ok = DeviceIoControl(h, IOCTL_WINLVM_UPDATE_LV,
                                  &req, sizeof(req), nullptr, 0, &returned, nullptr);
  CloseHandle(h);
  return ok != FALSE;
}

inline bool driver_quiesce_vg(const std::string& vg_name, bool quiesce) {
  if (vg_name.empty() || vg_name.size() >= WINLVM_NAME_WIRE_LEN) return false;
  HANDLE h = open_winlvm_control();
  if (h == INVALID_HANDLE_VALUE) return false;

  WINLVM_VG_REQUEST req{};
  req.header.version = WINLVM_ABI_VERSION;
  req.header.size = sizeof(req);
  std::memcpy(req.vg_name, vg_name.data(), vg_name.size());
  req.vg_name[vg_name.size()] = '\0';

  DWORD returned = 0;
  const DWORD code = quiesce ? IOCTL_WINLVM_QUIESCE_VG : IOCTL_WINLVM_RESUME_VG;
  const BOOL ok = DeviceIoControl(h, code, &req, sizeof(req), nullptr, 0, &returned, nullptr);
  CloseHandle(h);
  return ok != FALSE;
}

inline std::vector<std::string> discover_win32_pv_paths_for_vg(std::string_view vg_name) {
  std::vector<std::string> paths;
  for (const auto& cand : discover_win32_candidate_devices()) {
    std::string err;
    auto src = open_storage_source(cand.open_path, false, err);
    if (!src) continue;
    auto label = read_pv_label(*src, err);
    if (!label) continue;
    if (vg_name.empty()) {
      paths.push_back(cand.open_path);
      continue;
    }
    auto meta = read_vg_metadata(*src, *label, err);
    if (!meta) continue;
    auto vg = parse_vg_metadata(*meta, err);
    if (vg && vg->name == vg_name) {
      paths.push_back(cand.open_path);
    }
  }
  return paths;
}

inline std::vector<std::string> discover_win32_pv_paths_by_uuids(
    const std::vector<std::string>& needed_raw_uuids) {
  std::vector<std::string> found;
  if (needed_raw_uuids.empty()) return found;
  std::set<std::string> needed(needed_raw_uuids.begin(), needed_raw_uuids.end());

  for (const auto& cand : discover_win32_candidate_devices()) {
    if (needed.empty()) break;
    std::string err;
    auto src = open_storage_source(cand.open_path, false, err);
    if (!src) continue;
    auto label = read_pv_label(*src, err);
    if (!label) continue;
    const std::string raw = normalize_uuid_raw32(label->pv_uuid);
    if (needed.erase(raw) > 0) {
      found.push_back(cand.open_path);
    }
  }
  return found;
}

#endif  // _WIN32

}  // namespace lvmfmt
