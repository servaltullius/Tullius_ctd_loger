#include "I18nCore.h"

#include <cassert>
#include <string>

using skydiag::dump_tool::i18n::ConfidenceLabel;
using skydiag::dump_tool::i18n::ConfidenceLevel;
using skydiag::dump_tool::i18n::Language;
using skydiag::dump_tool::i18n::ParseLanguageTokenAscii;

static void Test_DefaultLanguage_IsEnglish()
{
  assert(skydiag::dump_tool::i18n::DefaultLanguage() == Language::kEnglish);
}

static void Test_ParseLanguageToken()
{
  assert(ParseLanguageTokenAscii("en") == Language::kEnglish);
  assert(ParseLanguageTokenAscii("english") == Language::kEnglish);
  assert(ParseLanguageTokenAscii("ko") == Language::kKorean);
  assert(ParseLanguageTokenAscii("korean") == Language::kKorean);
}

// Headless CLI runs (hang reports) have no --lang; they must follow the
// Windows display language like the viewer instead of always using English.
static void Test_LanguageFromWindowsUiLangId()
{
  using skydiag::dump_tool::i18n::LanguageFromWindowsUiLangId;
  assert(LanguageFromWindowsUiLangId(0x0412) == Language::kKorean);   // ko-KR
  assert(LanguageFromWindowsUiLangId(0x0012) == Language::kKorean);   // ko (neutral)
  assert(LanguageFromWindowsUiLangId(0x0409) == Language::kEnglish);  // en-US
  assert(LanguageFromWindowsUiLangId(0x0411) == Language::kEnglish);  // ja-JP
  assert(LanguageFromWindowsUiLangId(0x0000) == Language::kEnglish);
}

static void Test_ConfidenceLabels()
{
  assert(ConfidenceLabel(Language::kEnglish, ConfidenceLevel::kHigh) == L"High");
  assert(ConfidenceLabel(Language::kEnglish, ConfidenceLevel::kMedium) == L"Medium");
  assert(ConfidenceLabel(Language::kEnglish, ConfidenceLevel::kLow) == L"Low");

  assert(ConfidenceLabel(Language::kKorean, ConfidenceLevel::kHigh) == L"높음");
  assert(ConfidenceLabel(Language::kKorean, ConfidenceLevel::kMedium) == L"중간");
  assert(ConfidenceLabel(Language::kKorean, ConfidenceLevel::kLow) == L"낮음");
}

int main()
{
  Test_LanguageFromWindowsUiLangId();
  Test_DefaultLanguage_IsEnglish();
  Test_ParseLanguageToken();
  Test_ConfidenceLabels();
  return 0;
}

