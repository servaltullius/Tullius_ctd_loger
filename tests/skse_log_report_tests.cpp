// DLLs that SKSE refused at startup, from the captured session's skse64.log,
// are reported as context at the end of the checklist and never lead it.

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

AnalysisResult MakeCrashWithSkseLog(std::string status, std::uint32_t issueCount)
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261005_143810_399.dmp";
  r.exc_code = 0xC0000005u;
  r.exc_addr = 0x7FFE68312B24ull;
  r.fault_module_filename = L"SomeMod.dll";
  r.fault_module_path = L"C:\\Mods\\SomeMod.dll";
  r.fault_module_plus_offset = L"SomeMod.dll+0x1000";
  r.skse_log.status = std::move(status);
  r.skse_log.skse_version = "2.2.6";
  r.skse_log.checked_count = 322;
  r.skse_log.loaded_count = 320;
  r.skse_log.issue_count = issueCount;
  if (issueCount > 0) {
    r.skse_log.issues.push_back({ "NpcGhostFix.dll", "", "no version data", 0 });
    r.skse_log.issues.push_back({ "Missing Dep.dll", "Missing Dep", "couldn't load plugin", 126 });
  }
  return r;
}

const skydiag::dump_tool::EvidenceItem* FindEvidence(const AnalysisResult& r, std::wstring_view titlePart)
{
  for (const auto& e : r.evidence) {
    if (e.title.find(titlePart) != std::wstring::npos) {
      return &e;
    }
  }
  return nullptr;
}

void TestRefusedDllsAreListedAsContext()
{
  for (const auto lang : { i18n::Language::kKorean, i18n::Language::kEnglish }) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = MakeCrashWithSkseLog("matched", 2);
    BuildEvidenceAndSummary(r, lang);

    const auto* evidence = FindEvidence(r, en ? L"SKSE did not load 2 DLL(s)" : L"SKSE가 로드하지 않은 DLL 2개");
    Require(evidence != nullptr, "refused DLLs must be listed as evidence");
    Require(evidence->confidence_level == i18n::ConfidenceLevel::kLow, "refused DLLs are context, not a cause");
    Require(evidence->details.find(L"NpcGhostFix.dll") != std::wstring::npos, "the evidence names each DLL");
    Require(
      evidence->details.find(en ? L"a DLL it needs is missing (error 126)" : L"필요한 다른 DLL이 없음(오류 126)") !=
        std::wstring::npos,
      "the evidence explains SKSE's status");

    const std::wstring_view tag = L"[SKSE]";
    Require(!r.recommendations.empty(), "recommendations must be built");
    Require(r.recommendations.front().rfind(tag, 0) != 0, "refused DLLs must not lead the checklist");
    Require(r.recommendations.back().rfind(tag, 0) == 0, "refused DLLs come at the end of the checklist");
    Require(
      r.recommendations.back().find(en ? L"not evidence for the cause" : L"원인 근거는 아닙니다") != std::wstring::npos,
      "the recommendation must say this is not the incident's cause");
  }
}

void TestNothingWithoutAMatchedLogOrRefusals()
{
  for (const auto* status : { "no_matching_log", "not_found", "" }) {
    auto r = MakeCrashWithSkseLog(status, 0);
    r.skse_log.issues.push_back({ "Stale.dll", "", "no version data", 0 });
    BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
    Require(FindEvidence(r, L"SKSE did not load") == nullptr, "an unmatched log adds no evidence");
    for (const auto& line : r.recommendations) {
      Require(line.rfind(L"[SKSE]", 0) != 0, "an unmatched log adds no recommendation");
    }
  }

  auto clean = MakeCrashWithSkseLog("matched", 0);
  BuildEvidenceAndSummary(clean, i18n::Language::kEnglish);
  Require(FindEvidence(clean, L"SKSE did not load") == nullptr, "a session where every DLL loaded adds nothing");
}

}  // namespace

int main()
{
  try {
    TestRefusedDllsAreListedAsContext();
    TestNothingWithoutAMatchedLogOrRefusals();
    std::puts("skse log report tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
