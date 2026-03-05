// Minimal cross-platform UTF-8 <-> wstring conversion for Linux tests.
// On Windows, the real Utf.cpp uses Win32 API; this stub uses standard C++ for testing.

#include "Utf.h"

#include <codecvt>
#include <locale>

namespace skydiag::dump_tool {

std::wstring Utf8ToWide(std::string_view s)
{
  if (s.empty()) {
    return {};
  }
  std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
  return conv.from_bytes(s.data(), s.data() + s.size());
}

std::string WideToUtf8(std::wstring_view w)
{
  if (w.empty()) {
    return {};
  }
  std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
  return conv.to_bytes(w.data(), w.data() + w.size());
}

}  // namespace skydiag::dump_tool
