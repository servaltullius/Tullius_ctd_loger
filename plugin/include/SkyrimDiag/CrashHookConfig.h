#pragma once

#include <cstdint>
#include <string_view>

namespace skydiag::plugin {

// What CrashHookMode and EnableUnsafeCrashHookMode2 in SkyrimDiag.ini come to.
struct CrashHookModeSetting
{
  std::uint32_t mode = 1;
  bool invalid_value = false;  // CrashHookMode was not 0, 1 or 2; the default 1 is used
  bool mode2_refused = false;  // 2 without EnableUnsafeCrashHookMode2=1; 1 is used
};

// An INI integer as written: spaces around it, and a ";" or "#" comment after
// it, are allowed; anything else (a word, a second number) is no value.
// GetPrivateProfileIntW reads "abc" as 0 and "3x" as 3, which is how a typo
// turned the crash hook off or into mode 2.
constexpr bool ParseIniInteger(std::wstring_view text, std::int64_t& out) noexcept
{
  std::size_t i = 0;
  while (i < text.size() && (text[i] == L' ' || text[i] == L'\t')) {
    ++i;
  }
  bool negative = false;
  if (i < text.size() && (text[i] == L'-' || text[i] == L'+')) {
    negative = text[i] == L'-';
    ++i;
  }
  const std::size_t digitsAt = i;
  std::int64_t value = 0;
  while (i < text.size() && text[i] >= L'0' && text[i] <= L'9') {
    if (value > 100000) {
      return false;  // far past any setting
    }
    value = value * 10 + (text[i] - L'0');
    ++i;
  }
  if (i == digitsAt) {
    return false;
  }
  while (i < text.size() && (text[i] == L' ' || text[i] == L'\t')) {
    ++i;
  }
  if (i < text.size() && text[i] != L';' && text[i] != L'#') {
    return false;
  }
  out = negative ? -value : value;
  return true;
}

// Only 0 (off), 1 (fatal exceptions, the default) and 2 (every exception) are
// modes; a missing key is 1, and any other value is 1 too, never off or mode 2.
// Mode 2 also needs EnableUnsafeCrashHookMode2 to be exactly 1.
constexpr CrashHookModeSetting ResolveCrashHookMode(std::wstring_view modeText, std::wstring_view consentText) noexcept
{
  CrashHookModeSetting setting{};
  std::int64_t mode = 1;
  if (!modeText.empty() && (!ParseIniInteger(modeText, mode) || mode < 0 || mode > 2)) {
    setting.invalid_value = true;
    return setting;
  }
  std::int64_t consent = 0;
  const bool consented = ParseIniInteger(consentText, consent) && consent == 1;
  if (mode == 2 && !consented) {
    setting.mode2_refused = true;
    return setting;
  }
  setting.mode = static_cast<std::uint32_t>(mode);
  return setting;
}

}  // namespace skydiag::plugin
