#ifndef WINLVM_GUI_DPI_H
#define WINLVM_GUI_DPI_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace winlvm::gui {

inline void enable_per_monitor_dpi() {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (!user32) return;
  using SetCtxFn = BOOL(WINAPI*)(HANDLE);
  auto fn = reinterpret_cast<SetCtxFn>(
      reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
  if (fn) {
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (DPI_AWARENESS_CONTEXT)-4
    fn(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
  }
}

inline UINT get_window_dpi(HWND hwnd) {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 && hwnd) {
    using GetDpiFn = UINT(WINAPI*)(HWND);
    auto fn = reinterpret_cast<GetDpiFn>(
        reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")));
    if (fn) {
      const UINT dpi = fn(hwnd);
      if (dpi >= 96U) return dpi;
    }
  }
  HDC hdc = GetDC(hwnd);
  UINT dpi = 96U;
  if (hdc) {
    const int log_y = GetDeviceCaps(hdc, LOGPIXELSY);
    if (log_y > 0) dpi = static_cast<UINT>(log_y);
    ReleaseDC(hwnd, hdc);
  }
  return dpi;
}

inline int scale_dpi(int value_96, UINT dpi) {
  return MulDiv(value_96, static_cast<int>(dpi), 96);
}

inline HFONT create_ui_font(UINT dpi, bool bold = false) {
  LOGFONTW lf{};
  lf.lfHeight = -MulDiv(9, static_cast<int>(dpi), 72);
  lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfQuality = CLEARTYPE_QUALITY;
  lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
  return CreateFontIndirectW(&lf);
}

inline void apply_explorer_theme(HWND hwnd) {
  HMODULE ux = LoadLibraryW(L"uxtheme.dll");
  if (!ux) return;
  using SetThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
  auto fn = reinterpret_cast<SetThemeFn>(
      reinterpret_cast<void*>(GetProcAddress(ux, "SetWindowTheme")));
  if (fn) {
    fn(hwnd, L"Explorer", nullptr);
  }
  FreeLibrary(ux);
}

inline std::wstring utf8_to_wide(std::string_view s) {
  if (s.empty()) return {};
  const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
  return out;
}

inline std::string wide_to_utf8(std::wstring_view s) {
  if (s.empty()) return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      out.data(), needed, nullptr, nullptr);
  return out;
}

inline std::wstring format_bytes_w(std::uint64_t bytes) {
  static constexpr const wchar_t* kUnits[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB"};
  std::uint64_t denom = 1ULL;
  std::size_t unit = 0;
  while (unit + 1 < std::size(kUnits) && bytes / denom >= 1024ULL) {
    denom *= 1024ULL;
    ++unit;
  }
  if (unit == 0) {
    return std::to_wstring(bytes) + L" B";
  }
  const std::uint64_t whole = bytes / denom;
  const std::uint64_t rem = bytes % denom;
  if (whole >= 100ULL) {
    const std::uint64_t tenths = (rem * 10ULL + (denom / 2ULL)) / denom;
    if (tenths >= 10ULL) {
      return std::to_wstring(whole + 1ULL) + L".0 " + kUnits[unit];
    }
    return std::to_wstring(whole) + L"." + std::to_wstring(tenths) + L" " + kUnits[unit];
  }
  const std::uint64_t hundredths = (rem * 100ULL + (denom / 2ULL)) / denom;
  if (hundredths >= 100ULL) {
    return std::to_wstring(whole + 1ULL) + L".00 " + kUnits[unit];
  }
  std::wstring frac = std::to_wstring(hundredths);
  if (frac.size() < 2U) frac.insert(frac.begin(), L'0');
  return std::to_wstring(whole) + L"." + frac + L" " + kUnits[unit];
}

}  // namespace winlvm::gui
#endif
