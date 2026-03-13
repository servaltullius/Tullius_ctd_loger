#include <cassert>
#include <string>
#include <vector>

#include "CandidateConsensus.h"

using skydiag::dump_tool::ActionableCandidate;
using skydiag::dump_tool::BuildCandidateConsensus;
using skydiag::dump_tool::CandidateSignal;
using skydiag::dump_tool::i18n::Language;

namespace {

CandidateSignal MakeSignal(
  std::string familyId,
  std::wstring candidateKey,
  std::wstring displayName,
  std::uint32_t weight,
  std::wstring pluginName = L"",
  std::wstring modName = L"",
  std::wstring moduleFilename = L"")
{
  CandidateSignal signal{};
  signal.family_id = std::move(familyId);
  signal.candidate_key = std::move(candidateKey);
  signal.display_name = std::move(displayName);
  signal.plugin_name = std::move(pluginName);
  signal.mod_name = std::move(modName);
  signal.module_filename = std::move(moduleFilename);
  signal.weight = weight;
  return signal;
}

void AssertStatus(const ActionableCandidate& candidate, const char* expected)
{
  assert(candidate.status_id == expected);
}

void TestCrossValidatedObjectRefAndStackAgreement()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"examplemod", L"ExampleMod.esp", 6, L"ExampleMod.esp"),
    MakeSignal("actionable_stack", L"examplemod", L"Example Mod", 5, L"", L"Example Mod", L"ExampleMod.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "cross_validated");
  assert(candidates[0].cross_validated);
  assert(candidates[0].family_count == 2);
  assert(candidates[0].supporting_families.size() == 2);
}

void TestObjectRefOnlyStaysReferenceClue()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"refonly", L"RefOnly.esp", 6, L"RefOnly.esp"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "reference_clue");
  assert(!candidates[0].cross_validated);
  assert(candidates[0].family_count == 1);
}

void TestObjectRefAndStackConflictStayConflicting()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"pluginx", L"PluginX.esp", 6, L"PluginX.esp"),
    MakeSignal("actionable_stack", L"mody", L"Mod Y", 5, L"", L"Mod Y", L"mod_y.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() >= 2);
  AssertStatus(candidates[0], "conflicting");
  AssertStatus(candidates[1], "conflicting");
  assert(candidates[0].has_conflict);
  assert(candidates[1].has_conflict);
}

void TestSecondaryObjectRefCanStillCrossValidate()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"pluginx", L"PluginX.esp", 6, L"PluginX.esp"),
    MakeSignal("crash_logger_object_ref", L"pluginy", L"PluginY.esp", 5, L"PluginY.esp"),
    MakeSignal("actionable_stack", L"pluginy", L"Plugin Y", 5, L"", L"Plugin Y", L"plugin_y.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() >= 2);
  AssertStatus(candidates[0], "cross_validated");
  assert(candidates[0].cross_validated);
  assert(candidates[0].display_name == L"PluginY.esp");
}

void TestObjectRefAndResourceNeedMediumOnly()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"resourcex", L"ResourceX.esp", 6, L"ResourceX.esp"),
    MakeSignal("resource_provider", L"resourcex", L"Resource X", 3, L"", L"Resource X"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "related");
  assert(!candidates[0].cross_validated);
}

void TestHistoryOnlyDoesNotCreateStandaloneCandidate()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("history_repeat", L"historyonly", L"HistoryOnly.dll", 3, L"", L"", L"HistoryOnly.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.empty());
}

void TestObjectRefAndHistoryRepeatBecomeRelated()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"repeatmod", L"RepeatMod.esp", 6, L"RepeatMod.esp"),
    MakeSignal("history_repeat", L"repeatmod", L"RepeatMod.esp", 2, L"RepeatMod.esp"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "related");
  assert(candidates[0].family_count == 2);
}

void TestWeakStackAgreementStaysRelated()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"lowquality", L"LowQuality.esp", 6, L"LowQuality.esp"),
    MakeSignal("actionable_stack", L"lowquality", L"Low Quality", 3, L"", L"Low Quality", L"lowquality.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "related");
  assert(!candidates[0].cross_validated);
}

}  // namespace

int main()
{
  TestCrossValidatedObjectRefAndStackAgreement();
  TestObjectRefOnlyStaysReferenceClue();
  TestObjectRefAndStackConflictStayConflicting();
  TestSecondaryObjectRefCanStillCrossValidate();
  TestObjectRefAndResourceNeedMediumOnly();
  TestHistoryOnlyDoesNotCreateStandaloneCandidate();
  TestObjectRefAndHistoryRepeatBecomeRelated();
  TestWeakStackAgreementStaysRelated();
  return 0;
}
