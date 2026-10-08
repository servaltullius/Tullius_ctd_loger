// Guidance for the Address Library dialog seen in the v0.2.59 field test:
// SmoothCam.dll stopped the game with "Unsupported address library format: 2"
// and the report only said to read the dialog.

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

AnalysisResult MakeModalHang(std::string issue, bool callerResolved)
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Hang_20261005_155303_207.dmp";
  r.game_version = "1.6.1170.0";
  auto& modal = r.modal_dialog_wait;
  modal.detected = true;
  modal.window_evidence = true;
  modal.stack_evidence = true;
  modal.main_thread_id = 24328;
  modal.wait_api = L"user32.dll!MessageBoxW";
  modal.dialog_title = L"SmoothCam.dll";
  modal.dialog_text = L"REL/ID.h(223): Unsupported address library format: 2 / This means ...";
  if (callerResolved) {
    modal.caller_module_filename = L"SmoothCam.dll";
    modal.caller_inferred_mod_name = L"SmoothCam";
    modal.caller_kind = "plugin";
  }
  modal.address_library_issue = std::move(issue);
  return r;
}

void TestIncompatiblePluginGetsSpecificGuidance()
{
  for (const auto lang : { i18n::Language::kKorean, i18n::Language::kEnglish }) {
    const bool en = lang == i18n::Language::kEnglish;
    const std::wstring_view tag = en ? L"[Modal dialog]" : L"[Modal 대화상자]";
    auto r = MakeModalHang("plugin_incompatible", true);
    BuildEvidenceAndSummary(r, lang);
    Require(
      AnyRecommendationStartsWith(r, tag, en ? L"SmoothCam (SmoothCam.dll) was built for a different game version"
                                             : L"SmoothCam (SmoothCam.dll)은(는) 다른 게임 버전용으로 빌드되어"),
      "an Address Library format error must name the plugin as built for another game version");
    Require(AnyRecommendationStartsWith(r, tag, L"(1.6.1170.0)"), "the guidance must name the game version");
    Require(
      !AnyRecommendationStartsWith(r, tag, en ? L"Follow its message" : L"메시지(누락된 선행 모드"),
      "the generic 'follow the message' advice is replaced by the specific one");
    Require(
      r.summary_sentence.find(en ? L"does not support the Address Library" : L"Address Library를 지원하지 않습니다") !=
        std::wstring::npos,
      "the summary must say what the dialog means");
    Require(
      r.recommendations.front().rfind(tag, 0) == 0,
      "a modal dialog hang still leads with closing the dialog");
  }
}

void TestMissingAddressLibraryUsesDialogTitleWhenCallerIsUnknown()
{
  auto r = MakeModalHang("address_library_missing", false);
  BuildEvidenceAndSummary(r, i18n::Language::kKorean);
  Require(
    AnyRecommendationStartsWith(r, L"[Modal 대화상자]", L"SmoothCam.dll이(가) 현재 게임 버전 (1.6.1170.0)용 Address Library 파일을 찾지 못했습니다"),
    "the dialog title names the plugin when the stack walk did not resolve the caller");
  Require(
    AnyRecommendationStartsWith(r, L"[Modal 대화상자]", L"다른 판(SE 1.5.97 / AE 1.6 이상)"),
    "a missing database can also mean a plugin built for the other edition");
}

void TestOtherDialogsKeepTheGenericGuidance()
{
  auto r = MakeModalHang("", true);
  BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
  Require(
    AnyRecommendationStartsWith(r, L"[Modal dialog]", L"Follow its message"),
    "dialogs without an Address Library error keep the generic advice");
  Require(
    !AnyRecommendationStartsWith(r, L"[Modal dialog]", L"Address Library"),
    "no Address Library advice without the matching dialog text");
}

}  // namespace

int main()
{
  try {
    TestIncompatiblePluginGetsSpecificGuidance();
    TestMissingAddressLibraryUsesDialogTitleWhenCallerIsUnknown();
    TestOtherDialogsKeepTheGenericGuidance();
    std::puts("modal dialog recommendation tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
