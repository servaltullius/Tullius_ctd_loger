// Minimal cross-platform UTF-8 <-> wstring conversion for Linux tests.
// On Windows, the real Utf.cpp uses Win32 API; this stub uses standard C++ for testing.

#include "Utf.h"

#include <codecvt>
#include <locale>
#include <stdexcept>

namespace skydiag::dump_tool {

std::wstring Utf8ToWide(std::string_view s)
{
  if (s.empty()) {
    return {};
  }
  std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
  return conv.from_bytes(s.data(), s.data() + s.size());
}

// No ANSI code page here: bytes that are not UTF-8 are read as Latin-1.
std::wstring Utf8OrAnsiToWide(std::string_view s)
{
  try {
    return Utf8ToWide(s);
  } catch (const std::range_error&) {
    std::wstring out;
    out.reserve(s.size());
    for (const char c : s) {
      out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return out;
  }
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
