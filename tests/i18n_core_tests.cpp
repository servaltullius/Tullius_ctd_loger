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
  Test_DefaultLanguage_IsEnglish();
  Test_ParseLanguageToken();
  Test_ConfidenceLabels();
  return 0;
}

