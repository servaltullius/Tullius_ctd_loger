#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace skydiag::dump_tool::i18n {

enum class Language : std::uint8_t {
  kEnglish = 0,
  kKorean = 1,
};

inline Language DefaultLanguage()
{
  return Language::kEnglish;
}

inline std::string AsciiLower(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

inline Language ParseLanguageTokenAscii(std::string_view token)
{
  const std::string t = AsciiLower(token);
  if (t == "en" || t == "eng" || t == "english") {
    return Language::kEnglish;
  }
  if (t == "ko" || t == "kor" || t == "korean") {
    return Language::kKorean;
  }
  return DefaultLanguage();
}

enum class ConfidenceLevel : std::uint8_t {
  kUnknown = 0,
  kHigh = 1,
  kMedium = 2,
  kLow = 3,
};

inline std::wstring_view ConfidenceLabel(Language lang, ConfidenceLevel level)
{
  if (lang == Language::kEnglish) {
    switch (level) {
      case ConfidenceLevel::kHigh: return L"High";
      case ConfidenceLevel::kMedium: return L"Medium";
      case ConfidenceLevel::kLow: return L"Low";
      default: return L"Unknown";
    }
  }

  switch (level) {
    case ConfidenceLevel::kHigh: return L"높음";
    case ConfidenceLevel::kMedium: return L"중간";
    case ConfidenceLevel::kLow: return L"낮음";
    default: return L"(unknown)";
  }
}

inline std::wstring_view LanguageLabel(Language lang)
{
  return (lang == Language::kKorean) ? L"한국어" : L"English";
}

inline std::wstring_view LanguageCode(Language lang)
{
  return (lang == Language::kKorean) ? L"ko" : L"en";
}

}  // namespace skydiag::dump_tool::i18n

