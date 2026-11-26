#include "backend.h"

#include "mutation.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbt.h>
#include <winioctl.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../tools/lvm/win32_driver.h"
#include "../winlvm_ioctl.h"
#include "lvmfmt/allocator.h"
#include "lvmfmt/commit.h"
#include "lvmfmt/config.h"
#include "lvmfmt/label.h"
#include "lvmfmt/metadata.h"
#include "lvmfmt/source.h"

namespace winlvm::service {
namespace {

// Canonical Mount Manager user-mode IOCTL definitions (shared across MSVC and MinGW
// without pulling kernel DDK headers into user-mode builds).
#ifndef MOUNTMGRCONTROLTYPE
#define MOUNTMGRCONTROLTYPE ((ULONG)'m')
#endif
#ifndef IOCTL_MOUNTMGR_CREATE_POINT
#define IOCTL_MOUNTMGR_CREATE_POINT \
  CTL_CODE(MOUNTMGRCONTROLTYPE, 0, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif
#ifndef IOCTL_MOUNTMGR_DELETE_POINTS
#define IOCTL_MOUNTMGR_DELETE_POINTS \
  CTL_CODE(MOUNTMGRCONTROLTYPE, 1, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif

struct WinLvmMountMgrCreatePointInput {
  USHORT SymbolicLinkNameOffset;
  USHORT SymbolicLinkNameLength;
  USHORT DeviceNameOffset;
  USHORT DeviceNameLength;
};

struct WinLvmMountMgrMountPoint {
  ULONG SymbolicLinkNameOffset;
  USHORT SymbolicLinkNameLength;
  USHORT Reserved1;
  ULONG UniqueIdOffset;
  USHORT UniqueIdLength;
  USHORT Reserved2;
  ULONG DeviceNameOffset;
  USHORT DeviceNameLength;
  USHORT Reserved3;
};
static_assert(sizeof(WinLvmMountMgrCreatePointInput) == 8);
static_assert(sizeof(WinLvmMountMgrMountPoint) == 24);

std::wstring utf8_to_wide(std::string_view s) {
  if (s.empty()) return {};
  const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
  return out;
}

std::string wide_to_utf8(std::wstring_view s) {
  if (s.empty()) return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      out.data(), needed, nullptr, nullptr);
  return out;
}

std::string trim_output(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
    s.pop_back();
  }
  std::size_t start = 0;
  while (start < s.size() && (s[start] == '\r' || s[start] == '\n' || s[start] == ' ' || s[start] == '\t')) {
    ++start;
  }
  return s.substr(start);
}

std::string format_lvm_size_arg(std::uint64_t bytes) {
  if (bytes >= (1024ULL * 1024ULL) && (bytes % (1024ULL * 1024ULL)) == 0) {
    return std::to_string(bytes / (1024ULL * 1024ULL)) + "M";
  }
  if (bytes >= 1024ULL && (bytes % 1024ULL) == 0) {
    return std::to_string(bytes / 1024ULL) + "K";
  }
  return std::to_string(bytes) + "B";
}

std::wstring quote_win32_arg(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return arg;
  }
  std::wstring out;
  out.push_back(L'"');
  for (std::size_t i = 0; i < arg.size(); ++i) {
    std::size_t bs = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      ++bs;
      ++i;
    }
    if (i == arg.size()) {
      out.append(bs * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      out.append(bs * 2 + 1, L'\\');
      out.push_back(L'"');
    } else {
      out.append(bs, L'\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back(L'"');
  return out;
}

std::wstring locate_lvm_exe() {
  wchar_t self_path[MAX_PATH * 2]{};
  const DWORD len = GetModuleFileNameW(nullptr, self_path, static_cast<DWORD>(std::size(self_path)));
  if (len > 0 && len < std::size(self_path)) {
    std::wstring dir(self_path, len);
    const std::size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
      std::wstring sibling = dir.substr(0, slash + 1) + L"lvm.exe";
      if (GetFileAttributesW(sibling.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return sibling;
      }
    }
  }
  return {};
}

bool run_lvm_cli(const std::vector<std::wstring>& args, std::string& output, DWORD& exit_code) {
  output.clear();
  exit_code = 1;
  const std::wstring exe = locate_lvm_exe();
  if (exe.empty()) {
    output = "lvm.exe was not found next to the service binary";
    return false;
  }

  std::wstring cmdline = quote_win32_arg(exe);
  for (const auto& a : args) {
    cmdline.push_back(L' ');
    cmdline += quote_win32_arg(a);
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
    output = "failed to create output pipe for lvm.exe";
    return false;
  }
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = write_pipe;
  si.hStdError = write_pipe;

  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
  cmd_buf.push_back(L'\0');

  const BOOL created = CreateProcessW(
      exe.c_str(), cmd_buf.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(write_pipe);

  if (!created) {
    const DWORD err = GetLastError();
    CloseHandle(read_pipe);
    output = "failed to launch lvm.exe (Win32 error " + std::to_string(err) + ")";
    return false;
  }

  char chunk[4096];
  DWORD bytes_read = 0;
  while (ReadFile(read_pipe, chunk, sizeof(chunk), &bytes_read, nullptr) && bytes_read > 0) {
    if (output.size() < 64U * 1024U) {
      output.append(chunk, bytes_read);
    }
  }
  CloseHandle(read_pipe);

  const DWORD wait_rc = WaitForSingleObject(pi.hProcess, 60000);
  if (wait_rc != WAIT_OBJECT_0) {
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    output = "lvm.exe timed out";
    return false;
  }
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  output = trim_output(output);
  return true;
}

std::set<DWORD> query_windows_system_disks() {
  std::set<DWORD> result;
  wchar_t sys_dir[MAX_PATH]{};
  const UINT len = GetSystemDirectoryW(sys_dir, MAX_PATH);
  if (len < 2 || sys_dir[1] != L':') return result;
  wchar_t vol_path[8] = {L'\\', L'\\', L'.', L'\\', sys_dir[0], L':', L'\0', L'\0'};
  HANDLE h = CreateFileW(vol_path, 0,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return result;

  std::vector<std::uint8_t> buf(sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT), 0);
  DWORD returned = 0;
  if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                      buf.data(), static_cast<DWORD>(buf.size()), &returned, nullptr) &&
      returned >= sizeof(VOLUME_DISK_EXTENTS)) {
    const auto* vde = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buf.data());
    for (DWORD i = 0; i < vde->NumberOfDiskExtents; ++i) {
      result.insert(vde->Extents[i].DiskNumber);
    }
  } else {
    STORAGE_DEVICE_NUMBER sdn{};
    if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                        &sdn, sizeof(sdn), &returned, nullptr) &&
        sdn.DeviceType == FILE_DEVICE_DISK) {
      result.insert(sdn.DeviceNumber);
    }
  }
  CloseHandle(h);
  return result;
}

bool is_device_on_system_disk(const std::string& open_path, const std::set<DWORD>& sys_disks) {
  if (sys_disks.empty()) return false;
  const std::wstring wpath = utf8_to_wide(open_path);
  HANDLE h = CreateFileW(wpath.c_str(), 0,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  STORAGE_DEVICE_NUMBER sdn{};
  DWORD returned = 0;
  bool is_sys = false;
  if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                      &sdn, sizeof(sdn), &returned, nullptr) &&
      sdn.DeviceType == FILE_DEVICE_DISK) {
    is_sys = sys_disks.count(sdn.DeviceNumber) > 0;
  }
  CloseHandle(h);
  return is_sys;
}

struct VolumeMountInfo {
  std::string volume_guid_path;  // e.g. "\\\\?\\Volume{...}\\"
  std::string drive_letter;      // e.g. "E:\\"
  std::string filesystem;
  std::string volume_label;
};

bool extract_winlvm_uuid_from_nt_target(std::wstring_view target, std::string& out_raw_uuid) {
  constexpr std::wstring_view kPrefix = L"\\Device\\WinLvm";
  if (target.size() < kPrefix.size() + 32) return false;
  for (std::size_t i = 0; i < kPrefix.size(); ++i) {
    if (std::towlower(target[i]) != std::towlower(kPrefix[i])) return false;
  }
  const std::wstring_view tail = target.substr(kPrefix.size());
  if (tail.size() < 32) return false;
  std::string raw;
  raw.reserve(32);
  for (std::size_t i = 0; i < 32; ++i) {
    const wchar_t c = tail[i];
    if (c >= 128) return false;
    raw.push_back(static_cast<char>(c));
  }
  if (!is_valid_uuid_chars(raw)) return false;
  out_raw_uuid = raw;
  return true;
}

void query_volume_fs_info(const std::wstring& root_path,
                          std::string& out_fs,
                          std::string& out_label) {
  if (root_path.empty()) return;
  DWORD old_mode = 0;
  SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode);
  wchar_t label_buf[MAX_PATH + 1]{};
  wchar_t fs_buf[MAX_PATH + 1]{};
  if (GetVolumeInformationW(root_path.c_str(),
                            label_buf, static_cast<DWORD>(std::size(label_buf)),
                            nullptr, nullptr, nullptr,
                            fs_buf, static_cast<DWORD>(std::size(fs_buf)))) {
    out_label = wide_to_utf8(label_buf);
    out_fs = wide_to_utf8(fs_buf);
  }
  SetThreadErrorMode(old_mode, nullptr);
}

std::map<std::string, VolumeMountInfo> discover_winlvm_mounts() {
  std::map<std::string, VolumeMountInfo> mounts;

  wchar_t vol_name[MAX_PATH]{};
  HANDLE find = FindFirstVolumeW(vol_name, static_cast<DWORD>(std::size(vol_name)));
  if (find != INVALID_HANDLE_VALUE) {
    do {
      std::wstring guid_path(vol_name);
      // Strip leading "\\?\" and trailing "\" for QueryDosDeviceW.
      if (guid_path.size() > 5 && guid_path.rfind(L"\\\\?\\", 0) == 0) {
        std::wstring dos_key = guid_path.substr(4);
        if (!dos_key.empty() && dos_key.back() == L'\\') dos_key.pop_back();
        wchar_t target_buf[512]{};
        if (QueryDosDeviceW(dos_key.c_str(), target_buf,
                            static_cast<DWORD>(std::size(target_buf))) > 0) {
          std::string raw_uuid;
          if (extract_winlvm_uuid_from_nt_target(target_buf, raw_uuid)) {
            VolumeMountInfo info;
            info.volume_guid_path = wide_to_utf8(guid_path);

            std::vector<wchar_t> paths_buf(1024, L'\0');
            DWORD needed = 0;
            if (GetVolumePathNamesForVolumeNameW(guid_path.c_str(), paths_buf.data(),
                                                 static_cast<DWORD>(paths_buf.size()), &needed)) {
              const wchar_t* p = paths_buf.data();
              while (*p != L'\0') {
                std::wstring_view mp(p);
                if (mp.size() >= 2 && mp[1] == L':') {
                  info.drive_letter = wide_to_utf8(mp);
                  break;
                }
                p += mp.size() + 1;
              }
            }
            const std::wstring fs_root =
                !info.drive_letter.empty() ? utf8_to_wide(info.drive_letter) : guid_path;
            query_volume_fs_info(fs_root, info.filesystem, info.volume_label);
            mounts[raw_uuid] = std::move(info);
          }
        }
      }
    } while (FindNextVolumeW(find, vol_name, static_cast<DWORD>(std::size(vol_name))));
    FindVolumeClose(find);
  }

  // Also check drive letter DOS symlinks directly (covers volumes mounted via
  // DefineDosDeviceW or MountMgr before a Volume GUID mount path is listed).
  for (wchar_t letter = L'D'; letter <= L'Z'; ++letter) {
    wchar_t dos_name[3] = {letter, L':', L'\0'};
    wchar_t target_buf[512]{};
    if (QueryDosDeviceW(dos_name, target_buf, static_cast<DWORD>(std::size(target_buf))) > 0) {
      std::string raw_uuid;
      if (extract_winlvm_uuid_from_nt_target(target_buf, raw_uuid)) {
        auto& info = mounts[raw_uuid];
        if (info.drive_letter.empty()) {
          info.drive_letter = std::string(1, static_cast<char>(letter)) + ":\\";
        }
        if (info.filesystem.empty()) {
          std::wstring root = {letter, L':', L'\\'};
          query_volume_fs_info(root, info.filesystem, info.volume_label);
        }
      }
    }
  }

  return mounts;
}

void broadcast_volume_change(char drive_letter, bool arrived) {
  if (drive_letter < 'A' || drive_letter > 'Z') return;
  DEV_BROADCAST_VOLUME dbv{};
  dbv.dbcv_size = sizeof(dbv);
  dbv.dbcv_devicetype = DBT_DEVTYP_VOLUME;
  dbv.dbcv_flags = 0;
  dbv.dbcv_unitmask = 1UL << static_cast<unsigned>(drive_letter - 'A');
  DWORD_PTR res = 0;
  SendMessageTimeoutW(HWND_BROADCAST, WM_DEVICECHANGE,
                      arrived ? DBT_DEVICEARRIVAL : DBT_DEVICEREMOVECOMPLETE,
                      reinterpret_cast<LPARAM>(&dbv),
                      SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, &res);
}

bool dismount_volume_handle(const std::wstring& open_target) {
  HANDLE h = CreateFileW(open_target.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  FlushFileBuffers(h);
  DWORD returned = 0;
  DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &returned, nullptr);
  const BOOL ok = DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &returned, nullptr);
  CloseHandle(h);
  return ok != FALSE;
}

void dismount_lv_by_uuid(const std::string& raw_lv_uuid) {
  const auto mounts = discover_winlvm_mounts();
  auto it = mounts.find(raw_lv_uuid);
  if (it == mounts.end()) return;
  if (!it->second.drive_letter.empty()) {
    std::wstring target = L"\\\\.\\";
    target.push_back(static_cast<wchar_t>(it->second.drive_letter[0]));
    target.push_back(L':');
    if (dismount_volume_handle(target)) return;
  }
  if (!it->second.volume_guid_path.empty()) {
    std::wstring guid = utf8_to_wide(it->second.volume_guid_path);
    if (!guid.empty() && guid.back() == L'\\') guid.pop_back();
    dismount_volume_handle(guid);
  }
}

bool mountmgr_delete_dos_link(char drive_letter) {
  HANDLE mgr = CreateFileW(L"\\\\.\\MountPointManager", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (mgr == INVALID_HANDLE_VALUE) return false;

  std::wstring link = L"\\DosDevices\\";
  link.push_back(static_cast<wchar_t>(drive_letter));
  link.push_back(L':');
  const USHORT link_bytes = static_cast<USHORT>(link.size() * sizeof(wchar_t));

  std::vector<std::uint8_t> in_buf(sizeof(WinLvmMountMgrMountPoint) + link_bytes, 0);
  auto* mp = reinterpret_cast<WinLvmMountMgrMountPoint*>(in_buf.data());
  mp->SymbolicLinkNameOffset = sizeof(WinLvmMountMgrMountPoint);
  mp->SymbolicLinkNameLength = link_bytes;
  std::memcpy(in_buf.data() + mp->SymbolicLinkNameOffset, link.data(), link_bytes);

  std::vector<std::uint8_t> out_buf(4096, 0);
  DWORD returned = 0;
  const BOOL ok = DeviceIoControl(mgr, IOCTL_MOUNTMGR_DELETE_POINTS,
                                  in_buf.data(), static_cast<DWORD>(in_buf.size()),
                                  out_buf.data(), static_cast<DWORD>(out_buf.size()),
                                  &returned, nullptr);
  CloseHandle(mgr);
  return ok != FALSE;
}

bool mountmgr_create_dos_link(char drive_letter, const std::wstring& nt_device_name) {
  HANDLE mgr = CreateFileW(L"\\\\.\\MountPointManager", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (mgr == INVALID_HANDLE_VALUE) return false;

  std::wstring link = L"\\DosDevices\\";
  link.push_back(static_cast<wchar_t>(drive_letter));
  link.push_back(L':');
  const USHORT link_bytes = static_cast<USHORT>(link.size() * sizeof(wchar_t));
  const USHORT dev_bytes = static_cast<USHORT>(nt_device_name.size() * sizeof(wchar_t));

  std::vector<std::uint8_t> in_buf(sizeof(WinLvmMountMgrCreatePointInput) + link_bytes + dev_bytes, 0);
  auto* hdr = reinterpret_cast<WinLvmMountMgrCreatePointInput*>(in_buf.data());
  hdr->SymbolicLinkNameOffset = sizeof(WinLvmMountMgrCreatePointInput);
  hdr->SymbolicLinkNameLength = link_bytes;
  hdr->DeviceNameOffset = static_cast<USHORT>(hdr->SymbolicLinkNameOffset + link_bytes);
  hdr->DeviceNameLength = dev_bytes;
  std::memcpy(in_buf.data() + hdr->SymbolicLinkNameOffset, link.data(), link_bytes);
  std::memcpy(in_buf.data() + hdr->DeviceNameOffset, nt_device_name.data(), dev_bytes);

  DWORD returned = 0;
  const BOOL ok = DeviceIoControl(mgr, IOCTL_MOUNTMGR_CREATE_POINT,
                                  in_buf.data(), static_cast<DWORD>(in_buf.size()),
                                  nullptr, 0, &returned, nullptr);
  CloseHandle(mgr);
  return ok != FALSE;
}

bool resolve_lv_info(const std::string& vg_name,
                     const std::string& lv_name,
                     const std::string& lv_uuid,
                     LvInfo& out) {
  const Topology topo = query_live_topology(true, true);
  const std::string norm_uuid = lv_uuid.empty() ? std::string() : lvmfmt::normalize_uuid_raw32(lv_uuid);
  for (const auto& lv : topo.lvs) {
    if (!norm_uuid.empty() && lvmfmt::normalize_uuid_raw32(lv.lv_uuid) == norm_uuid) {
      out = lv;
      return true;
    }
    if (!vg_name.empty() && !lv_name.empty() && lv.vg_name == vg_name && lv.lv_name == lv_name) {
      out = lv;
      return true;
    }
  }
  return false;
}

void probe_all_discovered_pvs() {
  lvmfmt::driver_refresh();
  for (const auto& cand : lvmfmt::discover_win32_candidate_devices()) {
    std::string err;
    auto src = lvmfmt::open_storage_source(cand.open_path, false, err);
    if (!src) continue;
    auto label = lvmfmt::read_pv_label(*src, err);
    if (label) {
      lvmfmt::driver_probe_device(cand.probe_path);
    }
  }
}

struct ExtraVgMetadata {
  std::vector<std::string> vg_tags;
  std::map<std::string, std::vector<std::string>> lv_tags;
  std::map<std::string, std::string> lv_backing;
  std::map<std::string, std::string> lv_seg_type_override;
};

ExtraVgMetadata extract_extra_metadata(std::string_view meta_text) {
  ExtraVgMetadata extra;
  std::string err;
  auto root = lvmfmt::parse_config(meta_text, err);
  if (!root) return extra;
  const lvmfmt::Section* vg_sec = nullptr;
  for (const auto& s : root->sections) {
    if (s.find_entry("id") && s.find_entry("extent_size")) {
      vg_sec = &s;
      break;
    }
  }
  if (!vg_sec) return extra;
  extra.vg_tags = vg_sec->string_list("tags");

  const lvmfmt::Section* lvs_sec = vg_sec->find_section("logical_volumes");
  if (!lvs_sec) return extra;
  for (const auto& lv_sec : lvs_sec->sections) {
    auto tags = lv_sec.string_list("tags");
    if (!tags.empty()) extra.lv_tags[lv_sec.name] = std::move(tags);

    std::string backing;
    for (const auto& seg_sec : lv_sec.sections) {
      if (seg_sec.name.rfind("segment", 0) != 0) continue;
      const std::string stype = seg_sec.string("type").value_or("");
      if (stype == "thin") {
        extra.lv_seg_type_override[lv_sec.name] = "thin";
        if (auto pool = seg_sec.string("thin_pool")) backing = "pool: " + *pool;
        break;
      }
      if (stype == "thin-pool") {
        extra.lv_seg_type_override[lv_sec.name] = "thin-pool";
        auto data = seg_sec.string("pool");
        if (!data) data = seg_sec.string("data");
        auto meta = seg_sec.string("metadata");
        if (!meta) meta = seg_sec.string("pool_metadata");
        if (data || meta) {
          backing = "data: " + data.value_or("?") + ", meta: " + meta.value_or("?");
        }
        break;
      }
      if (stype == "snapshot") {
        extra.lv_seg_type_override[lv_sec.name] = "snapshot";
        if (auto orig = seg_sec.string("origin")) backing = "origin: " + *orig;
        break;
      }
      if (stype == "raid1" || stype == "mirror") {
        extra.lv_seg_type_override[lv_sec.name] = stype;
        if (const auto* entry = seg_sec.find_entry("raids")) {
          std::string legs;
          for (const auto& v : *entry) {
            if (v.is_string()) {
              if (!legs.empty()) legs += ", ";
              legs += v.string_value;
            }
          }
          if (!legs.empty()) backing = legs;
        } else if (const auto* mentry = seg_sec.find_entry("mirrors")) {
          std::string legs;
          for (const auto& v : *mentry) {
            if (v.is_string()) {
              if (!legs.empty()) legs += ", ";
              legs += v.string_value;
            }
          }
          if (!legs.empty()) backing = legs;
        }
        break;
      }
    }
    if (!backing.empty()) extra.lv_backing[lv_sec.name] = std::move(backing);
  }
  return extra;
}

std::string classify_lv_segment_type(const lvmfmt::LogicalVolume& lv,
                                     const ExtraVgMetadata& extra) {
  if (auto it = extra.lv_seg_type_override.find(lv.name); it != extra.lv_seg_type_override.end()) {
    return it->second;
  }
  for (const auto& st : lv.status) {
    if (st == "SNAPSHOT") return "snapshot";
  }
  if (lv.segments.empty()) return "unknown";
  const auto& first = lv.segments.front();
  if (first.type == "striped") {
    return first.stripe_count > 1 ? "striped" : "linear";
  }
  if (first.type.empty()) return "linear";
  return first.type;
}

std::string summarize_lv_backing(const lvmfmt::LogicalVolume& lv,
                                 const ExtraVgMetadata& extra) {
  if (auto it = extra.lv_backing.find(lv.name); it != extra.lv_backing.end()) {
    return it->second;
  }
  std::vector<std::string> targets;
  std::set<std::string> seen;
  std::uint64_t max_stripes = 1;
  for (const auto& seg : lv.segments) {
    if (seg.stripe_count > max_stripes) max_stripes = seg.stripe_count;
    for (const auto& st : seg.stripes) {
      if (seen.insert(st.target).second) {
        targets.push_back(st.target);
      }
    }
  }
  std::string joined;
  for (std::size_t i = 0; i < targets.size(); ++i) {
    if (i > 0) joined += ", ";
    joined += targets[i];
  }
  if (max_stripes > 1 && !joined.empty()) {
    return std::to_string(max_stripes) + " stripes (" + joined + ")";
  }
  return joined;
}

std::string generate_lvm_uuid_32() {
  static constexpr char kChars[] =
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<std::size_t> dis(0, sizeof(kChars) - 2);
  std::string s;
  s.reserve(32);
  for (int i = 0; i < 32; ++i) s.push_back(kChars[dis(gen)]);
  return s;
}

}  // namespace

bool is_driver_available() {
  HANDLE h = lvmfmt::open_winlvm_control();
  if (h == INVALID_HANDLE_VALUE) return false;
  CloseHandle(h);
  return true;
}

Topology query_live_topology(bool service_connected, bool caller_elevated) {
  Topology topo;
  topo.service_connected = service_connected;
  topo.caller_elevated = caller_elevated;

  const auto sys_disks = query_windows_system_disks();
  const auto candidates = lvmfmt::discover_win32_candidate_devices();

  struct ParsedVgEntry {
    lvmfmt::VolumeGroup vg;
    ExtraVgMetadata extra;
  };
  std::map<std::string, ParsedVgEntry> vgs_by_uuid;
  std::map<std::string, PvInfo> pvs_by_uuid;

  for (const auto& cand : candidates) {
    std::wstring dos_name;
    if (cand.open_path.rfind("\\\\.\\", 0) == 0 || cand.open_path.rfind("\\\\?\\", 0) == 0) {
      dos_name = utf8_to_wide(cand.open_path.substr(4));
    }
    const std::wstring nt_target_w =
        dos_name.empty() ? std::wstring() : lvmfmt::detail::query_dos_target(dos_name);
    const std::string nt_target = wide_to_utf8(nt_target_w);

    std::string kind = "disk";
    if (lvmfmt::detail::starts_with_ci_w(nt_target_w, L"\\Device\\WinLuksVolume") ||
        lvmfmt::detail::starts_with_ci_w(dos_name, L"WinLuksVolume")) {
      kind = "winluks";
    } else if (lvmfmt::detail::is_harddisk_partition_dos_name(dos_name)) {
      kind = "partition";
    } else if (lvmfmt::detail::starts_with_ci_w(dos_name, L"Volume{")) {
      continue;  // Keep DiskInfo focused on physical disks, partitions, and WinLUKS volumes.
    }

    DiskInfo disk;
    disk.device_path = cand.open_path;
    disk.nt_target = nt_target;
    disk.kind = kind;
    disk.is_system_disk = is_device_on_system_disk(cand.open_path, sys_disks);

    std::string err;
    auto src = lvmfmt::open_storage_source(cand.open_path, false, err);
    if (src) {
      disk.size_bytes = src->size();
      disk.logical_sector = src->logical_sector_size();
      disk.physical_sector = src->physical_sector_size();

      auto label = lvmfmt::read_pv_label(*src, err);
      if (label) {
        const std::string raw_pv_uuid = lvmfmt::normalize_uuid_raw32(label->pv_uuid);
        const std::string dashed_pv_uuid = lvmfmt::format_uuid(raw_pv_uuid);
        disk.is_pv = true;
        disk.pv_uuid = dashed_pv_uuid;

        PvInfo pv_info;
        pv_info.device_path = cand.open_path;
        pv_info.pv_uuid = dashed_pv_uuid;
        pv_info.size_bytes = label->device_size > 0 ? label->device_size : disk.size_bytes;
        pv_info.free_bytes = pv_info.size_bytes;
        pv_info.mda_count = static_cast<std::uint32_t>(label->metadata_areas.size());
        pv_info.allocatable = true;

        std::string meta_err;
        if (auto meta_text = lvmfmt::read_vg_metadata(*src, *label, meta_err)) {
          std::string parse_err;
          if (auto vg = lvmfmt::parse_vg_metadata(*meta_text, parse_err)) {
            disk.vg_name = vg->name;
            pv_info.vg_name = vg->name;
            const std::string vg_raw = lvmfmt::normalize_uuid_raw32(vg->id);
            auto it = vgs_by_uuid.find(vg_raw);
            if (it == vgs_by_uuid.end() || vg->seqno > it->second.vg.seqno) {
              vgs_by_uuid[vg_raw] = ParsedVgEntry{std::move(*vg), extract_extra_metadata(*meta_text)};
            }
          }
        }
        if (pvs_by_uuid.find(raw_pv_uuid) == pvs_by_uuid.end()) {
          pvs_by_uuid[raw_pv_uuid] = std::move(pv_info);
        }
      }
    }

    topo.disks.push_back(std::move(disk));
  }

  // Enrich PV entries from highest-seqno VG metadata and populate VG + LV trees.
  std::map<std::string, std::size_t> lv_index_by_raw_uuid;
  for (const auto& [vg_raw, entry] : vgs_by_uuid) {
    (void)vg_raw;
    const auto& vg = entry.vg;
    const auto& extra = entry.extra;
    const std::uint64_t ext_bytes = vg.extent_size_sectors * 512ULL;
    const auto maps = lvmfmt::build_vg_extent_maps(vg);

    std::uint64_t total_extents = 0;
    std::uint64_t free_extents = 0;
    for (const auto& m : maps) {
      total_extents += m.total_extents;
      free_extents += m.free_extents;
      const std::string raw_pv = lvmfmt::normalize_uuid_raw32(m.pv_uuid);
      auto pit = pvs_by_uuid.find(raw_pv);
      if (pit != pvs_by_uuid.end()) {
        pit->second.vg_name = vg.name;
        pit->second.pv_key = m.pv_key;
        pit->second.free_bytes = m.free_extents * ext_bytes;
        pit->second.allocatable = m.allocatable;
        if (const auto* pv_meta = vg.find_pv_by_uuid(m.pv_uuid)) {
          pit->second.missing = pv_meta->is_missing();
        }
      } else {
        PvInfo missing_pv;
        missing_pv.pv_uuid = lvmfmt::format_uuid(raw_pv);
        missing_pv.vg_name = vg.name;
        missing_pv.pv_key = m.pv_key;
        missing_pv.size_bytes = m.total_extents * ext_bytes;
        missing_pv.free_bytes = m.free_extents * ext_bytes;
        missing_pv.allocatable = m.allocatable;
        missing_pv.missing = true;
        pvs_by_uuid[raw_pv] = std::move(missing_pv);
      }
    }

    std::uint32_t visible_lvs = 0;
    for (const auto& lv : vg.lvs) {
      if (!lv.internal) ++visible_lvs;
      LvInfo lvi;
      lvi.vg_name = vg.name;
      lvi.lv_name = lv.name;
      const std::string raw_lv = lvmfmt::normalize_uuid_raw32(lv.id);
      lvi.lv_uuid = lvmfmt::format_uuid(raw_lv);
      lvi.segment_type = classify_lv_segment_type(lv, extra);
      lvi.size_bytes = lv.extent_count() * ext_bytes;
      lvi.stripe_count = (!lv.segments.empty() && lv.segments.front().stripe_count > 1)
                             ? static_cast<std::uint32_t>(lv.segments.front().stripe_count)
                             : 1U;
      lvi.read_only = vg.is_read_only() || lv.is_read_only();
      lvi.internal = lv.internal;
      lvi.supported = lv.supported;
      lvi.backing_info = summarize_lv_backing(lv, extra);
      if (auto tit = extra.lv_tags.find(lv.name); tit != extra.lv_tags.end()) {
        lvi.tags = tit->second;
      }
      lv_index_by_raw_uuid[raw_lv] = topo.lvs.size();
      topo.lvs.push_back(std::move(lvi));
    }

    VgInfo vgi;
    vgi.name = vg.name;
    vgi.vg_uuid = lvmfmt::format_uuid(lvmfmt::normalize_uuid_raw32(vg.id));
    vgi.extent_size_bytes = ext_bytes;
    vgi.size_bytes = total_extents * ext_bytes;
    vgi.free_bytes = free_extents * ext_bytes;
    vgi.pv_count = static_cast<std::uint32_t>(vg.pvs.size());
    vgi.lv_count = visible_lvs;
    vgi.seqno = vg.seqno;
    vgi.read_only = vg.is_read_only();
    vgi.exported = vg.is_exported();
    vgi.system_id = vg.system_id;
    vgi.tags = extra.vg_tags;
    topo.vgs.push_back(std::move(vgi));
  }

  for (auto& [raw_pv, pvi] : pvs_by_uuid) {
    (void)raw_pv;
    topo.pvs.push_back(std::move(pvi));
  }

  // Merge live kernel driver inventory state.
  std::vector<WINLVM_INVENTORY_LV_ENTRY> inv_entries;
  std::uint32_t record_count = 0;
  if (lvmfmt::driver_query_inventory(inv_entries, record_count)) {
    topo.driver_available = true;
    topo.driver_vg_records = record_count;
    for (const auto& e : inv_entries) {
      const std::string raw_uuid = lvmfmt::normalize_uuid_raw32(
          std::string(e.lv_uuid, strnlen(e.lv_uuid, sizeof(e.lv_uuid))));
      const bool active = (e.flags & WINLVM_INV_LV_ACTIVE) != 0;
      const bool ro = (e.flags & WINLVM_INV_LV_READONLY) != 0;
      auto it = lv_index_by_raw_uuid.find(raw_uuid);
      if (it != lv_index_by_raw_uuid.end()) {
        auto& lvi = topo.lvs[it->second];
        lvi.active = active;
        lvi.read_only = lvi.read_only || ro;
        if (e.byte_size > 0) lvi.size_bytes = e.byte_size;
      } else if (raw_uuid.size() == 32) {
        LvInfo lvi;
        lvi.vg_name = std::string(e.vg_name, strnlen(e.vg_name, sizeof(e.vg_name)));
        lvi.lv_name = std::string(e.lv_name, strnlen(e.lv_name, sizeof(e.lv_name)));
        lvi.lv_uuid = lvmfmt::format_uuid(raw_uuid);
        lvi.segment_type = (e.flags & WINLVM_INV_LV_STRIPED) ? "striped" : "linear";
        lvi.size_bytes = e.byte_size;
        lvi.active = active;
        lvi.read_only = ro;
        lv_index_by_raw_uuid[raw_uuid] = topo.lvs.size();
        topo.lvs.push_back(std::move(lvi));
      }
    }
  } else {
    topo.driver_available = is_driver_available();
  }

  // Merge Windows volume mount points, drive letters, filesystems, and labels.
  const auto mounts = discover_winlvm_mounts();
  for (auto& lvi : topo.lvs) {
    const std::string raw_uuid = lvmfmt::normalize_uuid_raw32(lvi.lv_uuid);
    auto it = mounts.find(raw_uuid);
    if (it != mounts.end()) {
      lvi.volume_guid_path = it->second.volume_guid_path;
      lvi.drive_letter = it->second.drive_letter;
      lvi.filesystem = it->second.filesystem;
      lvi.volume_label = it->second.volume_label;
    }
  }

  return topo;
}

ActionResponse op_rescan() {
  if (!is_driver_available()) {
    return {ErrorCode::DriverUnavailable, "WinLVM kernel driver is not running", {}};
  }
  lvmfmt::driver_refresh();
  const auto candidates = lvmfmt::discover_win32_candidate_devices();
  std::size_t probed = 0;
  for (const auto& cand : candidates) {
    std::string err;
    auto src = lvmfmt::open_storage_source(cand.open_path, false, err);
    if (!src) continue;
    if (lvmfmt::read_pv_label(*src, err)) {
      if (lvmfmt::driver_probe_device(cand.probe_path)) {
        ++probed;
      }
    }
  }
  return {ErrorCode::Ok,
          "Scanned " + std::to_string(candidates.size()) + " devices; probed " +
              std::to_string(probed) + " physical volume(s).",
          {}};
}

ActionResponse op_activate_lv(const LvTargetRequest& req) {
  if (!is_driver_available()) {
    return {ErrorCode::DriverUnavailable, "WinLVM kernel driver is not running", {}};
  }
  LvInfo info;
  std::string raw_uuid;
  if (resolve_lv_info(req.vg_name, req.lv_name, req.lv_uuid, info)) {
    raw_uuid = lvmfmt::normalize_uuid_raw32(info.lv_uuid);
  } else if (!req.lv_uuid.empty()) {
    raw_uuid = lvmfmt::normalize_uuid_raw32(req.lv_uuid);
  } else {
    return {ErrorCode::NotFound, "Logical volume not found", {}};
  }

  MutationError lock_err = MutationError::Ok;
  const auto lock = MutationLock::acquire(raw_uuid, lock_err);
  if (!lock.owns_lock()) {
    return {ErrorCode::Busy, "Logical volume is busy with another operation", {}};
  }

  if (!lvmfmt::driver_activate_lv(raw_uuid, true)) {
    probe_all_discovered_pvs();
    if (!lvmfmt::driver_activate_lv(raw_uuid, true)) {
      return {ErrorCode::OperationFailed, "Driver failed to activate logical volume", {}};
    }
  }
  return {ErrorCode::Ok, "Logical volume activated", lvmfmt::format_uuid(raw_uuid)};
}

ActionResponse op_deactivate_lv(const LvTargetRequest& req) {
  if (!is_driver_available()) {
    return {ErrorCode::DriverUnavailable, "WinLVM kernel driver is not running", {}};
  }
  LvInfo info;
  std::string raw_uuid;
  if (resolve_lv_info(req.vg_name, req.lv_name, req.lv_uuid, info)) {
    raw_uuid = lvmfmt::normalize_uuid_raw32(info.lv_uuid);
  } else if (!req.lv_uuid.empty()) {
    raw_uuid = lvmfmt::normalize_uuid_raw32(req.lv_uuid);
  } else {
    return {ErrorCode::NotFound, "Logical volume not found", {}};
  }

  MutationError lock_err = MutationError::Ok;
  const auto lock = MutationLock::acquire(raw_uuid, lock_err);
  if (!lock.owns_lock()) {
    return {ErrorCode::Busy, "Logical volume is busy with another operation", {}};
  }

  dismount_lv_by_uuid(raw_uuid);
  if (!lvmfmt::driver_activate_lv(raw_uuid, false)) {
    return {ErrorCode::OperationFailed, "Driver failed to deactivate logical volume", {}};
  }
  return {ErrorCode::Ok, "Logical volume deactivated", lvmfmt::format_uuid(raw_uuid)};
}

ActionResponse op_set_drive_letter(const SetDriveLetterRequest& req) {
  if (!is_driver_available()) {
    return {ErrorCode::DriverUnavailable, "WinLVM kernel driver is not running", {}};
  }
  LvInfo info;
  if (!resolve_lv_info(req.vg_name, req.lv_name, req.lv_uuid, info)) {
    return {ErrorCode::NotFound, "Logical volume not found", {}};
  }
  if (!info.active) {
    return {ErrorCode::InvalidArgument, "Logical volume must be active before changing its drive letter", {}};
  }

  char target_letter = '\0';
  if (!normalize_drive_letter(req.drive_letter, target_letter)) {
    return {ErrorCode::InvalidArgument, "Drive letter must be empty or D through Z", {}};
  }

  const std::string raw_uuid = lvmfmt::normalize_uuid_raw32(info.lv_uuid);
  MutationError lock_err = MutationError::Ok;
  const auto lock = MutationLock::acquire(raw_uuid, lock_err);
  if (!lock.owns_lock()) {
    return {ErrorCode::Busy, "Logical volume is busy with another operation", {}};
  }

  const std::wstring nt_device = L"\\Device\\WinLvm" + utf8_to_wide(raw_uuid);

  if (target_letter != '\0') {
    wchar_t check_dos[3] = {static_cast<wchar_t>(target_letter), L':', L'\0'};
    wchar_t existing_target[512]{};
    if (QueryDosDeviceW(check_dos, existing_target,
                        static_cast<DWORD>(std::size(existing_target))) > 0) {
      std::string existing_uuid;
      if (!extract_winlvm_uuid_from_nt_target(existing_target, existing_uuid) ||
          existing_uuid != raw_uuid) {
        return {ErrorCode::Busy,
                std::string("Drive letter ") + target_letter + ": is already in use",
                {}};
      }
      return {ErrorCode::Ok,
              std::string("Drive letter ") + target_letter + ": is already assigned",
              info.lv_uuid};
    }
  }

  // Remove any current drive letter bound to this volume.
  if (!info.drive_letter.empty()) {
    const char old_letter = static_cast<char>(std::toupper(static_cast<unsigned char>(info.drive_letter[0])));
    std::wstring old_mp = {static_cast<wchar_t>(old_letter), L':', L'\\'};
    std::wstring old_dos = {static_cast<wchar_t>(old_letter), L':'};
    dismount_lv_by_uuid(raw_uuid);
    const BOOL del_mp = DeleteVolumeMountPointW(old_mp.c_str());
    const bool del_mm = mountmgr_delete_dos_link(old_letter);
    const BOOL del_dd = DefineDosDeviceW(
        DDD_REMOVE_DEFINITION | DDD_RAW_TARGET_PATH | DDD_EXACT_MATCH_ON_REMOVE,
        old_dos.c_str(), nt_device.c_str());
    if (!del_mp && !del_mm && !del_dd) {
      return {ErrorCode::OperationFailed,
              std::string("Failed to remove existing drive letter ") + old_letter + ":",
              {}};
    }
    broadcast_volume_change(old_letter, false);
  }

  if (target_letter == '\0') {
    return {ErrorCode::Ok, "Drive letter removed", info.lv_uuid};
  }

  std::wstring new_mp = {static_cast<wchar_t>(target_letter), L':', L'\\'};
  std::wstring new_dos = {static_cast<wchar_t>(target_letter), L':'};
  bool assigned = false;
  if (!info.volume_guid_path.empty()) {
    const std::wstring vol_guid = utf8_to_wide(info.volume_guid_path);
    assigned = SetVolumeMountPointW(new_mp.c_str(), vol_guid.c_str()) != FALSE;
  }
  if (!assigned) {
    assigned = mountmgr_create_dos_link(target_letter, nt_device);
  }
  if (!assigned) {
    assigned = DefineDosDeviceW(DDD_RAW_TARGET_PATH, new_dos.c_str(), nt_device.c_str()) != FALSE;
  }
  if (!assigned) {
    return {ErrorCode::OperationFailed,
            std::string("Failed to assign drive letter ") + target_letter + ":",
            {}};
  }
  broadcast_volume_change(target_letter, true);
  return {ErrorCode::Ok,
          std::string("Assigned drive letter ") + target_letter + ":\\",
          info.lv_uuid};
}

ActionResponse op_set_lv_read_only(const SetLvReadOnlyRequest& req) {
  LvInfo info;
  if (!resolve_lv_info(req.vg_name, req.lv_name, req.lv_uuid, info)) {
    return {ErrorCode::NotFound, "Logical volume not found", {}};
  }
  const std::string raw_uuid = lvmfmt::normalize_uuid_raw32(info.lv_uuid);
  MutationError lock_err = MutationError::Ok;
  const auto lock = MutationLock::acquire(raw_uuid, lock_err);
  if (!lock.owns_lock()) {
    return {ErrorCode::Busy, "Logical volume is busy with another operation", {}};
  }

  const std::wstring target = utf8_to_wide(info.vg_name + "/" + info.lv_name);
  const std::wstring mode = req.read_only ? L"r" : L"rw";
  std::string cli_out;
  DWORD exit_code = 1;
  if (run_lvm_cli({L"lvchange", L"-p", mode, target}, cli_out, exit_code)) {
    if (exit_code != 0) {
      return {ErrorCode::OperationFailed,
              cli_out.empty() ? "lvchange failed" : cli_out,
              {}};
    }
  } else {
    // In-process fallback when lvm.exe is absent.
    const auto pv_paths = lvmfmt::discover_win32_pv_paths_for_vg(info.vg_name);
    if (pv_paths.empty()) {
      return {ErrorCode::NotFound, "No physical volumes found for VG " + info.vg_name, {}};
    }
    std::vector<std::unique_ptr<lvmfmt::ByteSource>> sources;
    std::vector<lvmfmt::ByteSource*> raw_sources;
    std::optional<lvmfmt::VolumeGroup> best_vg;
    for (const auto& p : pv_paths) {
      std::string err;
      auto s = lvmfmt::open_storage_source(p, true, err);
      if (!s) return {ErrorCode::OperationFailed, err, {}};
      auto lbl = lvmfmt::read_pv_label(*s, err);
      if (lbl) {
        if (auto txt = lvmfmt::read_vg_metadata(*s, *lbl, err)) {
          if (auto vg = lvmfmt::parse_vg_metadata(*txt, err)) {
            if (!best_vg || vg->seqno > best_vg->seqno) best_vg = std::move(*vg);
          }
        }
      }
      raw_sources.push_back(s.get());
      sources.push_back(std::move(s));
    }
    if (!best_vg) return {ErrorCode::OperationFailed, "Failed to load VG metadata", {}};
    bool updated = false;
    for (auto& lv : best_vg->lvs) {
      if (lv.name == info.lv_name) {
        if (lv.status.empty()) lv.status = {"READ", "WRITE", "VISIBLE"};
        auto it = std::find(lv.status.begin(), lv.status.end(), "WRITE");
        if (req.read_only && it != lv.status.end()) lv.status.erase(it);
        if (!req.read_only && it == lv.status.end()) lv.status.push_back("WRITE");
        updated = true;
        break;
      }
    }
    if (!updated) return {ErrorCode::NotFound, "Logical volume not found in metadata", {}};
    best_vg->seqno++;
    std::string err;
    if (!lvmfmt::commit_vg_metadata(*best_vg, raw_sources, err)) {
      return {ErrorCode::OperationFailed, err, {}};
    }
  }

  if (is_driver_available()) {
    probe_all_discovered_pvs();
  }
  return {ErrorCode::Ok,
          req.read_only ? "Logical volume set to read-only" : "Logical volume set to read-write",
          info.lv_uuid};
}

ActionResponse op_create_pv(const CreatePvRequest& req) {
  const auto sys_disks = query_windows_system_disks();
  if (is_device_on_system_disk(req.device_path, sys_disks)) {
    return {ErrorCode::AccessDenied,
            "Refusing to initialize a physical volume on the active Windows system disk",
            {}};
  }

  std::vector<std::wstring> args = {L"pvcreate"};
  if (req.force) args.push_back(L"-f");
  args.push_back(utf8_to_wide(req.device_path));

  std::string cli_out;
  DWORD exit_code = 1;
  std::string created_uuid;
  if (run_lvm_cli(args, cli_out, exit_code)) {
    if (exit_code != 0) {
      return {ErrorCode::OperationFailed, cli_out.empty() ? "pvcreate failed" : cli_out, {}};
    }
    std::string err;
    if (auto src = lvmfmt::open_storage_source(req.device_path, false, err)) {
      if (auto lbl = lvmfmt::read_pv_label(*src, err)) {
        created_uuid = lvmfmt::format_uuid(lvmfmt::normalize_uuid_raw32(lbl->pv_uuid));
      }
    }
  } else {
    std::string err;
    auto src = lvmfmt::open_storage_source(req.device_path, true, err);
    if (!src) return {ErrorCode::OperationFailed, err, {}};
    lvmfmt::PvCreateOptions opts;
    std::string raw_uuid;
    if (!lvmfmt::pvcreate_initialize(*src, opts, raw_uuid, err)) {
      return {ErrorCode::OperationFailed, err, {}};
    }
    created_uuid = lvmfmt::format_uuid(lvmfmt::normalize_uuid_raw32(raw_uuid));
  }

  if (is_driver_available()) {
    lvmfmt::driver_probe_device(lvmfmt::resolve_win32_probe_path(req.device_path));
  }
  return {ErrorCode::Ok, "Physical volume initialized on " + req.device_path, created_uuid};
}

ActionResponse op_create_vg(const CreateVgRequest& req) {
  std::vector<std::wstring> args = {L"vgcreate"};
  if (req.force) args.push_back(L"-f");
  if (req.extent_size_bytes > 0) {
    args.push_back(L"-s");
    args.push_back(utf8_to_wide(format_lvm_size_arg(req.extent_size_bytes)));
  }
  args.push_back(utf8_to_wide(req.vg_name));
  for (const auto& pv : req.pv_paths) {
    args.push_back(utf8_to_wide(pv));
  }

  std::string cli_out;
  DWORD exit_code = 1;
  std::string created_uuid;
  if (run_lvm_cli(args, cli_out, exit_code)) {
    if (exit_code != 0) {
      return {ErrorCode::OperationFailed, cli_out.empty() ? "vgcreate failed" : cli_out, {}};
    }
  } else {
    const std::uint64_t ext_bytes =
        req.extent_size_bytes > 0 ? req.extent_size_bytes : (4ULL * 1024ULL * 1024ULL);
    lvmfmt::VolumeGroup vg;
    vg.name = req.vg_name;
    vg.id = generate_lvm_uuid_32();
    vg.seqno = 1;
    vg.extent_size_sectors = ext_bytes / 512ULL;
    vg.status = {"RESIZEABLE", "READ", "WRITE"};

    std::vector<std::unique_ptr<lvmfmt::ByteSource>> sources;
    std::vector<lvmfmt::ByteSource*> raw_sources;
    std::size_t idx = 0;
    for (const auto& p : req.pv_paths) {
      std::string err;
      auto src = lvmfmt::open_storage_source(p, true, err);
      if (!src) return {ErrorCode::OperationFailed, err, {}};
      auto lbl = lvmfmt::read_pv_label(*src, err);
      if (!lbl) return {ErrorCode::OperationFailed, err, {}};
      lvmfmt::PhysicalVolume pv;
      pv.key = "pv" + std::to_string(idx++);
      pv.id = lbl->pv_uuid;
      pv.device = p;
      pv.status = {"ALLOCATABLE"};
      pv.dev_size_sectors = lbl->device_size / 512ULL;
      pv.pe_start_sectors =
          (!lbl->data_areas.empty() && lbl->data_areas[0].offset > 0)
              ? (lbl->data_areas[0].offset / 512ULL)
              : 2048ULL;
      if (pv.dev_size_sectors <= pv.pe_start_sectors) {
        return {ErrorCode::InvalidArgument, "PV " + p + " is too small", {}};
      }
      pv.pe_count = (pv.dev_size_sectors - pv.pe_start_sectors) / vg.extent_size_sectors;
      vg.pvs.push_back(std::move(pv));
      raw_sources.push_back(src.get());
      sources.push_back(std::move(src));
    }
    std::string err;
    if (!lvmfmt::commit_vg_metadata(vg, raw_sources, err)) {
      return {ErrorCode::OperationFailed, err, {}};
    }
    created_uuid = lvmfmt::format_uuid(vg.id);
  }

  if (is_driver_available()) {
    probe_all_discovered_pvs();
  }
  return {ErrorCode::Ok, "Volume group \"" + req.vg_name + "\" created", created_uuid};
}

ActionResponse op_create_lv(const CreateLvRequest& req) {
  std::vector<std::wstring> args = {L"lvcreate"};
  if (req.segment_type == "linear") {
    args.push_back(L"-L");
    args.push_back(utf8_to_wide(format_lvm_size_arg(req.size_bytes)));
    args.push_back(L"-n");
    args.push_back(utf8_to_wide(req.lv_name));
    if (req.read_only) {
      args.push_back(L"-p");
      args.push_back(L"r");
    }
    args.push_back(utf8_to_wide(req.vg_name));
  } else if (req.segment_type == "striped") {
    args.push_back(L"-L");
    args.push_back(utf8_to_wide(format_lvm_size_arg(req.size_bytes)));
    args.push_back(L"-i");
    args.push_back(std::to_wstring(req.stripes));
    if (req.stripe_size_bytes > 0) {
      args.push_back(L"-I");
      args.push_back(utf8_to_wide(format_lvm_size_arg(req.stripe_size_bytes)));
    }
    args.push_back(L"-n");
    args.push_back(utf8_to_wide(req.lv_name));
    if (req.read_only) {
      args.push_back(L"-p");
      args.push_back(L"r");
    }
    args.push_back(utf8_to_wide(req.vg_name));
  } else if (req.segment_type == "thin-pool") {
    args.push_back(L"--type");
    args.push_back(L"thin-pool");
    args.push_back(L"-L");
    args.push_back(utf8_to_wide(format_lvm_size_arg(req.size_bytes)));
    args.push_back(L"-n");
    args.push_back(utf8_to_wide(req.lv_name));
    args.push_back(utf8_to_wide(req.vg_name));
  } else if (req.segment_type == "thin") {
    args.push_back(L"-V");
    args.push_back(utf8_to_wide(format_lvm_size_arg(req.virtual_size_bytes)));
    args.push_back(L"-T");
    args.push_back(utf8_to_wide(req.vg_name + "/" + req.pool_name));
    args.push_back(L"-n");
    args.push_back(utf8_to_wide(req.lv_name));
  } else if (req.segment_type == "snapshot") {
    args.push_back(L"-s");
    args.push_back(L"-L");
    args.push_back(utf8_to_wide(format_lvm_size_arg(req.size_bytes)));
    args.push_back(L"-n");
    args.push_back(utf8_to_wide(req.lv_name));
    args.push_back(utf8_to_wide(req.vg_name + "/" + req.origin_name));
  } else {
    return {ErrorCode::InvalidArgument, "Unsupported LV segment type: " + req.segment_type, {}};
  }

  std::string cli_out;
  DWORD exit_code = 1;
  if (!run_lvm_cli(args, cli_out, exit_code)) {
    return {ErrorCode::OperationFailed, cli_out, {}};
  }
  if (exit_code != 0) {
    return {ErrorCode::OperationFailed, cli_out.empty() ? "lvcreate failed" : cli_out, {}};
  }

  std::string created_uuid;
  if (is_driver_available()) {
    probe_all_discovered_pvs();
  }
  LvInfo created;
  if (resolve_lv_info(req.vg_name, req.lv_name, {}, created)) {
    created_uuid = created.lv_uuid;
    if (req.activate && req.segment_type != "thin-pool" && is_driver_available()) {
      lvmfmt::driver_activate_lv(lvmfmt::normalize_uuid_raw32(created_uuid), true);
    }
  }
  return {ErrorCode::Ok,
          "Logical volume \"" + req.vg_name + "/" + req.lv_name + "\" created",
          created_uuid};
}

ActionResponse op_resize_lv(const ResizeLvRequest& req) {
  LvInfo info;
  if (!resolve_lv_info(req.vg_name, req.lv_name, req.lv_uuid, info)) {
    return {ErrorCode::NotFound, "Logical volume not found", {}};
  }
  const std::string raw_uuid = lvmfmt::normalize_uuid_raw32(info.lv_uuid);
  const Geometry geom{info.size_bytes, req.new_size_bytes, 0, 512};

  std::string mut_err;
  const MutationError me = execute_mutation(
      MutationKind::Extend, raw_uuid, geom,
      [&](std::string& exec_err) -> bool {
        const std::wstring target = utf8_to_wide(info.vg_name + "/" + info.lv_name);
        const std::wstring size_arg = utf8_to_wide(format_lvm_size_arg(req.new_size_bytes));
        std::string cli_out;
        DWORD exit_code = 1;
        if (run_lvm_cli({L"lvextend", L"-L", size_arg, target}, cli_out, exit_code)) {
          if (exit_code != 0) {
            exec_err = cli_out.empty() ? "lvextend failed" : cli_out;
            return false;
          }
        } else {
          exec_err = cli_out;
          return false;
        }
        if (is_driver_available()) {
          LvInfo refreshed;
          std::uint64_t final_bytes = req.new_size_bytes;
          if (resolve_lv_info(info.vg_name, info.lv_name, raw_uuid, refreshed) &&
              refreshed.size_bytes > 0) {
            final_bytes = refreshed.size_bytes;
          }
          lvmfmt::driver_update_lv(raw_uuid, final_bytes, 512, 512);
          lvmfmt::driver_refresh();
        }
        return true;
      },
      mut_err);

  if (me == MutationError::InvalidGeometry) {
    return {ErrorCode::InvalidArgument,
            "New size must be strictly larger than current size and sector-aligned",
            {}};
  }
  if (me == MutationError::Busy) {
    return {ErrorCode::Busy, mut_err, {}};
  }
  if (me != MutationError::Ok) {
    return {ErrorCode::OperationFailed, mut_err, {}};
  }
  return {ErrorCode::Ok,
          "Logical volume \"" + info.vg_name + "/" + info.lv_name + "\" extended",
          info.lv_uuid};
}

ActionResponse op_rename_lv(const RenameLvRequest& req) {
  LvInfo info;
  if (!resolve_lv_info(req.vg_name, req.old_lv_name, {}, info)) {
    return {ErrorCode::NotFound, "Logical volume not found", {}};
  }
  const std::string raw_uuid = lvmfmt::normalize_uuid_raw32(info.lv_uuid);
  MutationError lock_err = MutationError::Ok;
  const auto lock = MutationLock::acquire(raw_uuid, lock_err);
  if (!lock.owns_lock()) {
    return {ErrorCode::Busy, "Logical volume is busy with another operation", {}};
  }

  std::string cli_out;
  DWORD exit_code = 1;
  if (!run_lvm_cli({L"lvrename", utf8_to_wide(req.vg_name + "/" + req.old_lv_name),
                    utf8_to_wide(req.new_lv_name)},
                   cli_out, exit_code)) {
    return {ErrorCode::OperationFailed, cli_out, {}};
  }
  if (exit_code != 0) {
    return {ErrorCode::OperationFailed, cli_out.empty() ? "lvrename failed" : cli_out, {}};
  }
  if (is_driver_available()) {
    probe_all_discovered_pvs();
  }
  return {ErrorCode::Ok,
          "Renamed \"" + req.old_lv_name + "\" to \"" + req.new_lv_name + "\"",
          {}};
}

ActionResponse op_remove_lv(const LvTargetRequest& req) {
  LvInfo info;
  if (!resolve_lv_info(req.vg_name, req.lv_name, req.lv_uuid, info)) {
    return {ErrorCode::NotFound, "Logical volume not found", {}};
  }
  const std::string raw_uuid = lvmfmt::normalize_uuid_raw32(info.lv_uuid);
  MutationError lock_err = MutationError::Ok;
  const auto lock = MutationLock::acquire(raw_uuid, lock_err);
  if (!lock.owns_lock()) {
    return {ErrorCode::Busy, "Logical volume is busy with another operation", {}};
  }

  if (info.active && is_driver_available()) {
    if (!info.drive_letter.empty()) {
      const char old_letter =
          static_cast<char>(std::toupper(static_cast<unsigned char>(info.drive_letter[0])));
      std::wstring old_mp = {static_cast<wchar_t>(old_letter), L':', L'\\'};
      dismount_lv_by_uuid(raw_uuid);
      DeleteVolumeMountPointW(old_mp.c_str());
      mountmgr_delete_dos_link(old_letter);
      broadcast_volume_change(old_letter, false);
    } else {
      dismount_lv_by_uuid(raw_uuid);
    }
    lvmfmt::driver_activate_lv(raw_uuid, false);
    Sleep(200);
  }

  std::string cli_out;
  DWORD exit_code = 1;
  if (!run_lvm_cli({L"lvremove", utf8_to_wide(info.vg_name + "/" + info.lv_name)},
                   cli_out, exit_code)) {
    return {ErrorCode::OperationFailed, cli_out, {}};
  }
  if (exit_code != 0) {
    return {ErrorCode::OperationFailed, cli_out.empty() ? "lvremove failed" : cli_out, {}};
  }
  if (is_driver_available()) {
    probe_all_discovered_pvs();
  }
  return {ErrorCode::Ok,
          "Removed logical volume \"" + info.vg_name + "/" + info.lv_name + "\"",
          info.lv_uuid};
}

void dispatch_operation(const Request& request,
                        std::string_view request_payload,
                        bool caller_elevated,
                        bool service_connected,
                        ErrorCode& out_error,
                        std::string& out_payload) {
  const bool drv = is_driver_available();
  std::string err_msg;
  const ErrorCode precheck =
      validate_request(request, request_payload, drv, caller_elevated, err_msg);
  if (precheck != ErrorCode::Ok) {
    out_error = precheck;
    out_payload = encode_action_response({precheck, err_msg, {}});
    return;
  }

  if (request.operation == Operation::GetTopology) {
    const Topology topo = query_live_topology(service_connected, caller_elevated);
    out_error = ErrorCode::Ok;
    out_payload = encode_topology(topo);
    return;
  }

  if (request.operation == Operation::Inventory) {
    std::vector<WINLVM_INVENTORY_LV_ENTRY> entries;
    std::uint32_t records = 0;
    if (!lvmfmt::driver_query_inventory(entries, records)) {
      out_error = ErrorCode::DriverUnavailable;
      out_payload = encode_action_response(
          {ErrorCode::DriverUnavailable, "Failed to query WinLVM driver inventory", {}});
      return;
    }
    out_error = ErrorCode::Ok;
    out_payload = encode_action_response(
        {ErrorCode::Ok,
         "VGs=" + std::to_string(records) + ", LVs=" + std::to_string(entries.size()),
         {}});
    return;
  }

  ActionResponse resp{};
  std::string dec_err;
  switch (request.operation) {
    case Operation::Refresh:
    case Operation::Rescan:
      resp = op_rescan();
      break;
    case Operation::ActivateLv: {
      LvTargetRequest req;
      resp = decode_lv_target(request_payload, req, dec_err)
                 ? op_activate_lv(req)
                 : ActionResponse{ErrorCode::InvalidArgument, dec_err, {}};
      break;
    }
    case Operation::DeactivateLv: {
      LvTargetRequest req;
      resp = decode_lv_target(request_payload, req, dec_err)
                 ? op_deactivate_lv(req)
                 : ActionResponse{ErrorCode::InvalidArgument, dec_err, {}};
      break;
    }
    case Operation::SetDriveLetter: {
      SetDriveLetterRequest req;
      resp = decode_set_drive_letter(request_payload, req, dec_err)
                 ? op_set_drive_letter(req)
                 : ActionResponse{ErrorCode::InvalidArgument, dec_err, {}};
      break;
    }
    case Operation::SetLvReadOnly: {
      SetLvReadOnlyRequest req;
      resp = decode_set_lv_read_only(request_payload, req, dec_err)
                 ? op_set_lv_read_only(req)
                 : ActionResponse{ErrorCode::InvalidArgument, dec_err, {}};
      break;
    }
    case Operation::CreatePv: {
      CreatePvRequest req;
      resp = decode_create_pv(request_payload, req, dec_err)
                 ? op_create_pv(req)
                 : ActionResponse{ErrorCode::InvalidArgument, dec_err, {}};
      break;
    }
    case Operation::CreateVg: {
      CreateVgRequest req;
      resp = decode_create_vg(request_payload, req, dec_err)
                 ? op_create_vg(req)
                 : ActionResponse{ErrorCode::InvalidArgument, dec_err, {}};
      break;
    }
    case Operation::CreateLv: {
      CreateLvRequest req;
      resp = decode_create_lv(request_payload, req, dec_err)
                 ? op_create_lv(req)
                 : ActionResponse{ErrorCode::InvalidArgument, dec_err, {}};
      break;
    }
    case Operation::ResizeLv: {
      ResizeLvRequest req;
      resp = decode_resize_lv(request_payload, req, dec_err)
                 ? op_resize_lv(req)
                 : ActionResponse{ErrorCode::InvalidArgument, dec_err, {}};
      break;
    }
    case Operation::RenameLv: {
      RenameLvRequest req;
      resp = decode_rename_lv(request_payload, req, dec_err)
                 ? op_rename_lv(req)
                 : ActionResponse{ErrorCode::InvalidArgument, dec_err, {}};
      break;
    }
    case Operation::RemoveLv: {
      LvTargetRequest req;
      resp = decode_lv_target(request_payload, req, dec_err)
                 ? op_remove_lv(req)
                 : ActionResponse{ErrorCode::InvalidArgument, dec_err, {}};
      break;
    }
    case Operation::GetTopology:
    case Operation::Inventory:
      break;
  }

  out_error = resp.error;
  out_payload = encode_action_response(resp);
}

}  // namespace winlvm::service

#else

namespace winlvm::service {

bool is_driver_available() { return false; }

Topology query_live_topology(bool service_connected, bool caller_elevated) {
  Topology t;
  t.service_connected = service_connected;
  t.caller_elevated = caller_elevated;
  return t;
}

ActionResponse op_rescan() {
  return {ErrorCode::DriverUnavailable, "WinLVM kernel driver requires Windows", {}};
}
ActionResponse op_activate_lv(const LvTargetRequest&) { return op_rescan(); }
ActionResponse op_deactivate_lv(const LvTargetRequest&) { return op_rescan(); }
ActionResponse op_set_drive_letter(const SetDriveLetterRequest&) { return op_rescan(); }
ActionResponse op_set_lv_read_only(const SetLvReadOnlyRequest&) { return op_rescan(); }
ActionResponse op_create_pv(const CreatePvRequest&) { return op_rescan(); }
ActionResponse op_create_vg(const CreateVgRequest&) { return op_rescan(); }
ActionResponse op_create_lv(const CreateLvRequest&) { return op_rescan(); }
ActionResponse op_resize_lv(const ResizeLvRequest&) { return op_rescan(); }
ActionResponse op_rename_lv(const RenameLvRequest&) { return op_rescan(); }
ActionResponse op_remove_lv(const LvTargetRequest&) { return op_rescan(); }

void dispatch_operation(const Request& request,
                        std::string_view request_payload,
                        bool caller_elevated,
                        bool service_connected,
                        ErrorCode& out_error,
                        std::string& out_payload) {
  std::string err_msg;
  const ErrorCode rc = validate_request(request, request_payload, false, caller_elevated, err_msg);
  if (rc != ErrorCode::Ok) {
    out_error = rc;
    out_payload = encode_action_response({rc, err_msg, {}});
    return;
  }
  if (request.operation == Operation::GetTopology) {
    out_error = ErrorCode::Ok;
    out_payload = encode_topology(query_live_topology(service_connected, caller_elevated));
    return;
  }
  out_error = ErrorCode::DriverUnavailable;
  out_payload = encode_action_response(
      {ErrorCode::DriverUnavailable, "WinLVM service backend requires Windows", {}});
}

}  // namespace winlvm::service

#endif
