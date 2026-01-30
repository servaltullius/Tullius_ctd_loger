#include "Utf.h"

#include <Windows.h>

namespace skydiag::dump_tool {

std::wstring Utf8ToWide(std::string_view s)
{
  if (s.empty()) {
    return {};
  }

  const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }

  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
  return out;
}

std::string WideToUtf8(std::wstring_view w)
{
  if (w.empty()) {
    return {};
  }
  const int needed =
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
  return out;
}

}  // namespace skydiag::dump_tool

