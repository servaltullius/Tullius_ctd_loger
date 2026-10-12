// A loaded SkyrimCrashGuard.dll is named in every capture: it swallows
// crashes, so what the dump shows may not be the real cause (Crash Logger's
// ProblematicModules.cpp warns about the same DLL).

#include <cstdio>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "Analyzer.h"
#include "EvidenceBuilder.h"
#include "MinidumpUtil.h"
#include "NextAction.h"
#include "OutputWriterPipeline.h"
#include "ProblematicModules.h"

namespace {

using skydiag::dump_tool::AnalysisResult;
using skydiag::dump_tool::BuildEvidenceAndSummary;
using skydiag::dump_tool::FindProblematicModuleRule;
using skydiag::dump_tool::FindProblematicModules;
using skydiag::dump_tool::minidump::ModuleInfo;
namespace i18n = skydiag::dump_tool::i18n;

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool Contains(const std::wstring& text, std::wstring_view part)
{
  return text.find(part) != std::wstring::npos;
}

bool AnyStartsWith(const std::vector<std::wstring>& lines, std::wstring_view prefix, std::wstring_view part = L"")
{
  for (const auto& line : lines) {
    if (line.rfind(prefix, 0) == 0 && Contains(line, part)) {
      return true;
    }
  }
  return false;
}

ModuleInfo Module(std::wstring filename, std::wstring modName = {})
{
  ModuleInfo m{};
  m.filename = std::move(filename);
  m.path = L"G:\\TAKEALOOK\\mods\\" + modName + L"\\SKSE\\Plugins\\" + m.filename;
  m.inferred_mod_name = std::move(modName);
  return m;
}

void TestMatching()
{
  Require(FindProblematicModuleRule(L"SkyrimCrashGuard.dll") != nullptr, "exact name");
  Require(FindProblematicModuleRule(L"SKYRIMCRASHGUARD.DLL") != nullptr, "any case");
  Require(FindProblematicModuleRule(L"SkyrimCrashGuard.dll.bak") == nullptr, "no partial match");
  Require(FindProblematicModuleRule(L"CrashLogger.dll") == nullptr, "Crash Logger is not one");

  const auto found = FindProblematicModules({
    Module(L"CrashLogger.dll", L"CrashLogger"),
    Module(L"SkyrimCrashGuard.dll", L"Skyrim Crash Guard"),
    Module(L"skyrimcrashguard.dll", L"Skyrim Crash Guard"),
  });
  Require(found.size() == 1u, "each problem module is listed once");
  Require(found[0].filename == L"SkyrimCrashGuard.dll" && found[0].name == L"SkyrimCrashGuard" &&
            found[0].mod_name == L"Skyrim Crash Guard",
    "file, name and MO2 mod");
}

AnalysisResult MakeCrash()
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261007_123742_440.dmp";
  r.exc_code = 0xC0000005u;
  r.fault_module_filename = L"SkyrimSE.exe";
  r.problematic_modules = FindProblematicModules({ Module(L"SkyrimCrashGuard.dll", L"Skyrim Crash Guard") });
  return r;
}

void TestEveryCaptureNamesIt()
{
  for (const auto lang : { i18n::Language::kEnglish, i18n::Language::kKorean }) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = MakeCrash();
    BuildEvidenceAndSummary(r, lang);
    bool hasEvidence = false;
    for (const auto& e : r.evidence) {
      hasEvidence = hasEvidence ||
        (Contains(e.title, L"SkyrimCrashGuard") && e.confidence_level == i18n::ConfidenceLevel::kMedium &&
         Contains(e.details, L"nexusmods.com/skyrimspecialedition/mods/172082"));
    }
    Require(hasEvidence, "evidence item with the reason and the mod page");
    Require(AnyStartsWith(r.recommendations, en ? L"[Problem module]" : L"[문제 모듈]",
              en ? L"SkyrimCrashGuard.dll (SkyrimCrashGuard, mod: Skyrim Crash Guard) is loaded"
                 : L"SkyrimCrashGuard.dll (SkyrimCrashGuard, 모드: Skyrim Crash Guard)이(가) 로드되어 있습니다. 이 모듈은 크래시를"),
      "recommendation naming the DLL and its mod");
  }

  // Without a candidate it is the next action, ahead of recapture advice: a
  // recapture with the module still loaded tells no more.
  {
    auto recaptured = MakeCrash();
    recaptured.incident_capture_profile_present = true;
    recaptured.incident_capture_kind = "crash_recapture";
    BuildEvidenceAndSummary(recaptured, i18n::Language::kEnglish);
    Require(AnyStartsWith(recaptured.recommendations, L"[Recapture]"), "the recapture line is there");
    const auto next = skydiag::dump_tool::SelectNextActionIndex(recaptured.recommendations);
    Require(next != std::wstring::npos && recaptured.recommendations[next].rfind(L"[Problem module]", 0) == 0,
      "the problem module is the next action");
  }

  // A freeze explained by a modal dialog still names it.
  AnalysisResult modal{};
  modal.dump_path = L"C:\\Logs\\SkyrimDiag_Hang_20261005_155303_207.dmp";
  modal.freeze_analysis.has_analysis = true;
  modal.modal_dialog_wait.detected = true;
  modal.modal_dialog_wait.window_evidence = true;
  modal.modal_dialog_wait.dialog_title = L"SmoothCam.dll";
  modal.problematic_modules = MakeCrash().problematic_modules;
  BuildEvidenceAndSummary(modal, i18n::Language::kEnglish);
  Require(AnyStartsWith(modal.recommendations, L"[Problem module]"), "named next to a modal dialog too");

  auto r = MakeCrash();
  BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
  const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", false);
  Require(summary["problematic_modules"].size() == 1u &&
            summary["problematic_modules"][0]["module"] == "SkyrimCrashGuard.dll" &&
            summary["problematic_modules"][0]["mod_name"] == "Skyrim Crash Guard",
    "summary JSON");
  const auto report = skydiag::dump_tool::BuildReportText(r, summary, false);
  Require(report.find("ProblematicModules: SkyrimCrashGuard.dll (SkyrimCrashGuard, mod: Skyrim Crash Guard)") !=
            std::string::npos,
    "report line");

  AnalysisResult clean{};
  clean.dump_path = MakeCrash().dump_path;
  clean.exc_code = 0xC0000005u;
  BuildEvidenceAndSummary(clean, i18n::Language::kEnglish);
  Require(!AnyStartsWith(clean.recommendations, L"[Problem module]"), "nothing without the DLL");
  const auto cleanSummary = skydiag::dump_tool::BuildSummaryJson(clean, std::filesystem::temp_directory_path(), L"stem", false);
  Require(!cleanSummary.contains("problematic_modules"), "no JSON block without the DLL");
}

}  // namespace

int main()
{
  try {
    TestMatching();
    TestEveryCaptureNamesIt();
    std::puts("problematic module tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
