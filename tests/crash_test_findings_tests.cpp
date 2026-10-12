// What the 2026-10-12 crash test (tools/crash_tester) showed the reports got
// wrong, pinned with the shapes of those real captures:
// - a freeze whose main thread waited in the engine for a game job thread
//   that was stuck inside a plugin named only the main thread's call path;
// - an unhandled C++ exception that Crash Logger also logged was described as
//   a possibly caught throw, with advice to set the CrashHookMode already set;
// - the SKSE runtime, the path SKSE tasks take, came up as a candidate and as
//   a "nearby DLL" to compare.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "Analyzer.h"
#include "AnalyzerInternals.h"
#include "AnalyzerPipeline.h"
#include "EvidenceBuilder.h"
#include "EvidenceBuilderPrivate.h"
#include "NextAction.h"
#include "OutputWriterPipeline.h"

namespace skydiag::dump_tool::internal::stackwalk {
std::vector<SuspectItem> ComputeCallstackSuspectsFromAddrs(
  const std::vector<minidump::ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  i18n::Language lang);
}

namespace {

using skydiag::dump_tool::AnalysisResult;
using skydiag::dump_tool::BuildEvidenceAndSummary;
using skydiag::dump_tool::SelectNextActionIndex;
using skydiag::dump_tool::SuspectItem;
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

SuspectItem Suspect(std::wstring module, std::wstring modName, i18n::ConfidenceLevel level)
{
  SuspectItem s{};
  s.module_filename = std::move(module);
  s.inferred_mod_name = std::move(modName);
  s.confidence_level = level;
  s.score = 1;
  return s;
}

// SkyrimDiag_Hang_20261012_191024_970 after the fix: the lead the analyzer
// puts first, then the main thread's call path (demoted).
AnalysisResult MakeBlockedFreeze(std::wstring module, std::string state)
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Hang_20261012_191024_970.dmp";
  r.freeze_analysis.has_analysis = true;
  r.freeze_analysis.state_id = "freeze_ambiguous";
  auto& wait = r.main_thread_wait;
  wait.kind = "engine_wait";
  wait.wait_class = "sync";
  wait.wait_api = L"KERNELBASE.dll!WaitForSingleObjectEx";
  wait.waiting_module = L"SkyrimSE.exe";
  wait.path_modules = { L"hdtsmp64.dll" };
  wait.blocked_thread_tid = 40400;
  wait.blocked_module = module;
  wait.blocked_mod_name = L"CrashTester (dev test)";
  wait.blocked_state = std::move(state);
  wait.blocked_top = L"KERNELBASE.dll!SleepEx";
  wait.blocked_thread_count = 1;
  r.suspects = {
    Suspect(module, L"CrashTester (dev test)", i18n::ConfidenceLevel::kMedium),
    Suspect(L"hdtsmp64.dll", L"FSMP", i18n::ConfidenceLevel::kLow),
  };
  r.suspects_from_stackwalk = true;
  r.suspects_from_main_thread = true;
  return r;
}

void TestBlockedGameThreadLeadsTheFreezeReport()
{
  for (const auto lang : { i18n::Language::kEnglish, i18n::Language::kKorean }) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = MakeBlockedFreeze(L"CrashTester.dll", "waiting_in_plugin");
    BuildEvidenceAndSummary(r, lang);
    Require(Contains(r.summary_sentence, en ? L"that thread (tid 40400) was waiting inside CrashTester (dev test) (CrashTester.dll)"
                                            : L"그 스레드(tid 40400)는 CrashTester (dev test) (CrashTester.dll) 코드 안에서 대기 중이었습니다"),
      "the summary names the plugin holding the thread the main thread waited for");
    Require(!Contains(r.summary_sentence, en ? L"cannot tell what the engine was waiting for" : L"무엇을 기다렸는지는"),
      "no longer says the wait cannot be explained");
    const auto next = SelectNextActionIndex(r.recommendations);
    Require(next != std::wstring::npos &&
              r.recommendations[next].rfind(en ? L"[Main thread]" : L"[메인 스레드]", 0) == 0 &&
              Contains(r.recommendations[next], L"CrashTester.dll"),
      "the next action is the blocked plugin");
    Require(!AnyStartsWith(r.recommendations, en ? L"[Main thread]" : L"[메인 스레드]", L"hdtsmp64.dll"),
      "the main thread's call path is not the advice");
    bool hasEvidence = false;
    for (const auto& e : r.evidence) {
      hasEvidence = hasEvidence || (e.confidence_level == i18n::ConfidenceLevel::kMedium &&
        Contains(e.title, en ? L"was inside plugin code" : L"플러그인 코드 안에 있었음") && Contains(e.details, L"tid 40400"));
    }
    Require(hasEvidence, "evidence item for the blocked thread");
  }

  auto r = MakeBlockedFreeze(L"CrashTester.dll", "waiting_in_plugin");
  BuildEvidenceAndSummary(r, i18n::Language::kEnglish);
  const auto summary = skydiag::dump_tool::BuildSummaryJson(r, std::filesystem::temp_directory_path(), L"stem", false);
  const auto& blocked = summary["freeze_analysis"]["main_thread_wait"]["blocked_thread"];
  Require(blocked["tid"] == 40400 && blocked["module"] == "CrashTester.dll" && blocked["state"] == "waiting_in_plugin",
    "summary JSON blocked_thread");
  const auto report = skydiag::dump_tool::BuildReportText(r, summary, false);
  Require(report.find("blocked_tid=40400 blocked_module=CrashTester.dll blocked_state=waiting_in_plugin") != std::string::npos,
    "report main_thread_wait line");

  const auto keys = skydiag::dump_tool::CollectHistoryCandidateKeys(r);
  Require(keys.size() == 1u && keys[0].find("crashtester") != std::string::npos,
    "history records the blocked plugin, not the call path");
}

void TestBlockedPluginSplitsTheHangBucket()
{
  const auto key = [](std::wstring module, std::string state) {
    auto r = MakeBlockedFreeze(std::move(module), std::move(state));
    skydiag::dump_tool::internal::ComputeCrashBucket(r);
    return r.crash_bucket_key;
  };
  const auto sleeping = key(L"CrashTester.dll", "waiting_in_plugin");
  Require(sleeping.rfind(L"HANG1-", 0) == 0, "a wait bucket");
  Require(sleeping == key(L"CRASHTESTER.DLL", "waiting_in_plugin"), "the same plugin and state share a bucket");
  Require(sleeping != key(L"CrashTester.dll", "engine_call_from_plugin"), "the state splits it");
  Require(sleeping != key(L"Other.dll", "waiting_in_plugin"), "another plugin splits it");
  auto plain = MakeBlockedFreeze(L"CrashTester.dll", "waiting_in_plugin");
  plain.main_thread_wait.blocked_thread_tid = 0;
  plain.main_thread_wait.blocked_module.clear();
  skydiag::dump_tool::internal::ComputeCrashBucket(plain);
  Require(plain.crash_bucket_key != sleeping, "a plain engine wait keeps its own bucket");
}

AnalysisResult MakeCppCrash(std::wstring crashLoggerType)
{
  AnalysisResult r{};
  r.dump_path = L"C:\\Logs\\SkyrimDiag_Crash_20261012_192248_896.dmp";
  r.exc_code = 0xE06D7363u;
  r.fault_module_filename = L"KERNELBASE.dll";
  r.crash_logger_exception_type = std::move(crashLoggerType);
  return r;
}

void TestConfirmedCppExceptionIsARealCrash()
{
  for (const auto lang : { i18n::Language::kEnglish, i18n::Language::kKorean }) {
    const bool en = lang == i18n::Language::kEnglish;
    auto r = MakeCppCrash(L"C++ Exception");
    Require(skydiag::dump_tool::IsCrashLoggerConfirmedCppException(r), "Crash Logger's C++ Exception line confirms it");
    BuildEvidenceAndSummary(r, lang);
    Require(AnyStartsWith(r.recommendations, en ? L"[Interpretation]" : L"[해석]", en ? L"real CTD" : L"실제 CTD"),
      "explained as a real crash");
    Require(!AnyStartsWith(r.recommendations, en ? L"[Config]" : L"[설정]"), "no CrashHookMode advice");
    Require(!AnyStartsWith(r.recommendations, en ? L"[Interpretation]" : L"[해석]", en ? L"false positive" : L"오탐일 수"),
      "not called a possible false positive");
  }

  auto unconfirmed = MakeCppCrash(L"");
  Require(!skydiag::dump_tool::IsCrashLoggerConfirmedCppException(unconfirmed), "no Crash Logger line, no confirmation");
  BuildEvidenceAndSummary(unconfirmed, i18n::Language::kEnglish);
  Require(AnyStartsWith(unconfirmed.recommendations, L"[Config]"), "without Crash Logger the old caution stays");

  auto other = MakeCppCrash(L"EXCEPTION_ACCESS_VIOLATION");
  Require(!skydiag::dump_tool::IsCrashLoggerConfirmedCppException(other), "only Crash Logger's C++ exception counts");
}

void TestSkseRuntimeIsThePathNotASuspect()
{
  using skydiag::dump_tool::minidump::ModuleInfo;
  const auto module = [](std::wstring name, std::uint64_t base) {
    ModuleInfo m{};
    m.filename = std::move(name);
    m.path = L"G:\\mods\\x\\SKSE\\Plugins\\" + m.filename;
    m.base = base;
    m.end = base + 0x1000;
    return m;
  };
  const std::vector<ModuleInfo> modules = {
    module(L"CrashTester.dll", 0x10000), module(L"skse64_1_6_1170.dll", 0x20000),
  };
  const auto below = skydiag::dump_tool::internal::stackwalk::ComputeCallstackSuspectsFromAddrs(
    modules, { 0x10100, 0x10200, 0x20100 }, i18n::Language::kEnglish);
  Require(below.size() == 1u && below[0].module_filename == L"CrashTester.dll", "SKSE below a plugin is the path");
  const auto faulting = skydiag::dump_tool::internal::stackwalk::ComputeCallstackSuspectsFromAddrs(
    modules, { 0x20100, 0x10100 }, i18n::Language::kEnglish);
  bool hasSkse = false;
  for (const auto& s : faulting) {
    hasSkse = hasSkse || s.module_filename == L"skse64_1_6_1170.dll";
  }
  Require(hasSkse, "SKSE that faulted is still a suspect");

  AnalysisResult r{};
  r.fault_module_filename = L"CrashTester.dll";
  r.crash_logger_top_modules = { L"CrashTester.dll", L"skse64_1_6_1170.dll" };
  Require(!skydiag::dump_tool::internal::HasNearbyProbableDll(r), "SKSE is not a nearby DLL to compare");
  r.crash_logger_top_modules.push_back(L"NPCWaterAIFix.dll");
  Require(skydiag::dump_tool::internal::HasNearbyProbableDll(r), "another plugin is");
}

}  // namespace

int main()
{
  try {
    TestBlockedGameThreadLeadsTheFreezeReport();
    TestBlockedPluginSplitsTheHangBucket();
    TestConfirmedCppExceptionIsARealCrash();
    TestSkseRuntimeIsThePathNotASuspect();
    std::puts("crash test findings tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
