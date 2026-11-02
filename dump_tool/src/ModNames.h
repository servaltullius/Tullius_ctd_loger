#pragma once

#include <cwctype>
#include <string>
#include <string_view>

namespace skydiag::dump_tool {

// An MO2 mod folder name as a player reads it: the tags and the order number
// a modlist puts in front ("[NoDelete] 0444 Refraction on Critical Hit") are
// dropped. Only text for people uses this; matching, history and candidate
// keys keep the folder name.
inline std::wstring ReadableModName(std::wstring_view name)
{
  const auto trimFront = [](std::wstring_view s) {
    while (!s.empty() && s.front() == L' ') {
      s.remove_prefix(1);
    }
    return s;
  };
  std::wstring_view rest = trimFront(name);
  bool tagged = false;
  while (!rest.empty() && rest.front() == L'[') {
    const auto close = rest.find(L']');
    if (close == std::wstring_view::npos) {
      break;
    }
    const auto after = trimFront(rest.substr(close + 1));
    if (after.empty()) {
      break;  // the tag is all there is
    }
    rest = after;
    tagged = true;
  }
  if (tagged) {
    std::size_t digits = 0;
    while (digits < rest.size() && std::iswdigit(rest[digits]) != 0) {
      ++digits;
    }
    if (digits >= 2 && digits <= 5 && digits < rest.size() && rest[digits] == L' ') {
      if (const auto after = trimFront(rest.substr(digits)); !after.empty()) {
        rest = after;
      }
    }
  }
  return std::wstring(rest);
}

}  // namespace skydiag::dump_tool
