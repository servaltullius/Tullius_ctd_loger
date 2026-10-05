// Recommendations for the exception shapes seen in the v0.2.59-rc2 field CTD:
// an assert breakpoint raised from a mod, and a fault inside CrashLogger that
// was CrashLogger probing memory while it reported that breakpoint.

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

#include "Analyzer.h"
#include "EvidenceBuilder.h"

namespace {

using skydiag::dump_tool::AnalysisResult;
using skydiag::dump_tool::BuildEvidenceAndSummary;
namespace i18n = skydiag::dump_tool::i18n;

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool AnyRecommendationStartsWith(const AnalysisResult& r, std::wstring_view prefix, std::wstring_view contains)
{
  for (const auto& line : r.recommendations) {
    if (line.rfind(prefix, 0) == 0 && line.find(contains) != std::wstring::npos) {
      return true;
    }
  }
  return false;
}

AnalysisResult MakeCrash(std::uint32_t code, const wchar_t* faultModule)
{
  AnalysisResult r{};
  r.exc_code = code;
  r.exc_addr = 0x7FFE68312B24ull;
  r.fault_module_filename = faultModule;
  r.fault_module_path = std::wstring(L"C:\\Mods\\") + faultModule;
  r.fault_module_plus_offset = std::wstring(faultModule) + L"+0x1000";
  return r;
}

void TestBreakpointGetsItsOwnExplanation()
{
  for (const auto lang : { i18n::Language::kKorean, i18n::Language::kEnglish }) {
    auto r = MakeCrash(0x80000003u, L"DragonWar.dll");
    BuildEvidenceAndSummary(r, lang);
    const bool en = lang == i18n::Language::kEnglish;
    Require(
      AnyRecommendationStartsWith(r, en ? L"[Basics]" : L"[기본]", en ? L"(breakpoint)" : L"(브레이크포인트)"),
      "a breakpoint crash must explain what a breakpoint exception means");
  }
}

void TestFaultInsideCrashLoggerIsExplainedAsSecondary()
{
  for (const auto* module : { L"CrashLogger.dll", L"CrashLoggerSSE.dll" }) {
    auto r = MakeCrash(0xC0000005u, module);
    BuildEvidenceAndSummary(r, i18n::Language::kKorean);
    Require(
      AnyRecommendationStartsWith(r, L"[해석]", L"2차 예외"),
      "a fault inside CrashLogger must be explained as a likely secondary fault");
    Require(
      !AnyRecommendationStartsWith(r, L"[훅 프레임워크]", L""),
      "a fault inside CrashLogger must not get the generic hook-framework advice");
  }

  auto other = MakeCrash(0xC0000005u, L"EngineFixes.dll");
  BuildEvidenceAndSummary(other, i18n::Language::kKorean);
  Require(
    !AnyRecommendationStartsWith(other, L"[해석]", L"2차 예외"),
    "only a fault inside CrashLogger is a CrashLogger secondary fault");
}

}  // namespace

int main()
{
  try {
    TestBreakpointGetsItsOwnExplanation();
    TestFaultInsideCrashLoggerIsExplainedAsSecondary();
    std::puts("exception code recommendation tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
