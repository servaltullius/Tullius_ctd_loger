#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>

namespace skydiag {

inline std::wstring WideLower(std::wstring_view s)
{
  std::wstring out(s);
  std::transform(out.begin(), out.end(), out.begin(),
    [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return out;
}

}  // namespace skydiag
