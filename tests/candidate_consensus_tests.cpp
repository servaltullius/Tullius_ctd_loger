#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "AnalyzerScoringPolicy.h"
#include "CandidateConsensus.h"

using skydiag::dump_tool::ActionableCandidate;
using skydiag::dump_tool::BuildCandidateConsensus;
using skydiag::dump_tool::CandidateSignal;
using skydiag::dump_tool::CanonicalCandidateKey;
using skydiag::dump_tool::SortActionableCandidates;
using skydiag::dump_tool::i18n::Language;
namespace scoring_policy = skydiag::dump_tool::internal::policy;

namespace {

std::filesystem::path ProjectRoot()
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  if (root && *root) {
    return std::filesystem::path(root);
  }
  auto p = std::filesystem::current_path();
  for (int i = 0; i < 5; ++i) {
    if (std::filesystem::exists(p / "vcpkg.json")) {
      return p;
    }
    p = p.parent_path();
  }
  assert(false && "Cannot find project root. Set SKYDIAG_PROJECT_ROOT.");
  return {};
}

std::string ReadAllText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::in | std::ios::binary);
  assert(in && "Failed to open file");
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void AssertContains(const std::string& haystack, const char* needle, const char* message)
{
  assert(haystack.find(needle) != std::string::npos && message);
}

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
  assert(!candidates[0].cross_validated);
  assert(!candidates[1].cross_validated);
  assert(candidates[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kMedium);
  assert(candidates[1].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kMedium);
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

void TestRepresentativeNamePrefersPluginFilenameOverFriendlyLabel()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"factionranks", L"Faction Ranks", 6, L"FactionRanks.esp"),
    MakeSignal("actionable_stack", L"factionranks", L"Faction Ranks", 5, L"", L"Faction Ranks", L"paragon-perks.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  assert(candidates[0].display_name == L"FactionRanks.esp");
  assert(candidates[0].primary_identifier == L"FactionRanks.esp");
  assert(candidates[0].secondary_label == L"Faction Ranks");
}

void TestRepresentativeNamePrefersDllFilenameOverModFolderName()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("actionable_stack", L"paragonperks", L"Faction Ranks", 5, L"", L"Faction Ranks", L"paragon-perks.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  assert(candidates[0].display_name == L"paragon-perks.dll");
  assert(candidates[0].primary_identifier == L"paragon-perks.dll");
  assert(candidates[0].secondary_label == L"Faction Ranks");
}

void TestStrongStackOnlyBecomesMediumRelated()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("actionable_stack", L"standalonestack", L"Standalone Stack", 5, L"", L"Standalone Stack", L"StandaloneStack.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "related");
  assert(!candidates[0].cross_validated);
  assert(candidates[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kMedium);
}

void TestHangThreadGroupCorroboratesWeakMainStack()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("actionable_stack", L"faster-hdt-smp", L"Faster HDT-SMP", 2, L"", L"Faster HDT-SMP", L"hdtsmp64.dll"),
    MakeSignal("hang_thread_group", L"faster-hdt-smp", L"Faster HDT-SMP", 5, L"", L"Faster HDT-SMP", L"hdtsmp64.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1u);
  AssertStatus(candidates[0], "related");
  assert(candidates[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kMedium);
  assert(candidates[0].supporting_families.size() == 2u);
}

void TestObjectRefAndResourceStayReferenceOnly()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"resourcex", L"ResourceX.esp", 6, L"ResourceX.esp"),
    MakeSignal("resource_provider", L"resourcex", L"Resource X", 3, L"", L"Resource X"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "reference_clue");
  assert(!candidates[0].cross_validated);
  assert(candidates[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kLow);
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

void TestObjectRefAndPointerScanCannotCrossValidateWithBoostOnlyScores()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"pointerscan", L"PointerScan.esp", 6, L"PointerScan.esp"),
    MakeSignal("actionable_stack", L"pointerscan", L"Pointer Scan", 2, L"", L"Pointer Scan", L"PointerScan.dll"),
    MakeSignal("history_repeat", L"pointerscan", L"Pointer Scan", 3, L"", L"Pointer Scan", L"PointerScan.dll"),
    MakeSignal("resource_provider", L"pointerscan", L"Pointer Scan", 5, L"", L"Pointer Scan", L"PointerScan.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1u);
  AssertStatus(candidates[0], "related");
  assert(!candidates[0].cross_validated);
  assert(candidates[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kMedium);
  assert(candidates[0].score == 16u);
}

void TestAuxiliaryCoreFamiliesCannotFillIndependentHighThreshold()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"auxiliary", L"Auxiliary.esp", 5, L"Auxiliary.esp"),
    MakeSignal("actionable_stack", L"auxiliary", L"Auxiliary", 4, L"", L"Auxiliary", L"Auxiliary.dll"),
    MakeSignal("first_chance_context", L"auxiliary", L"Auxiliary", 3, L"", L"Auxiliary", L"Auxiliary.dll"),
    MakeSignal("hang_thread_group", L"auxiliary", L"Auxiliary", 5, L"", L"Auxiliary", L"Auxiliary.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1u);
  AssertStatus(candidates[0], "related");
  assert(!candidates[0].cross_validated);
  assert(candidates[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kMedium);
  assert(candidates[0].score == 17u);
}

void TestObjectRefAndFormalStackCrossValidateAtIndependentThreshold()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"threshold", L"Threshold.esp", 6, L"Threshold.esp"),
    MakeSignal("actionable_stack", L"threshold", L"Threshold", 4, L"", L"Threshold", L"Threshold.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1u);
  AssertStatus(candidates[0], "cross_validated");
  assert(candidates[0].cross_validated);
  assert(candidates[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kHigh);
  assert(candidates[0].score == 10u);
}

void TestFirstChanceOnlyDoesNotCreateStandaloneCandidate()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("first_chance_context", L"firstchanceonly", L"FirstChanceOnly.dll", 3, L"", L"", L"FirstChanceOnly.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.empty());
}

void TestObjectRefAndFirstChanceBecomeRelated()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"firstchanceboost", L"FirstChanceBoost.esp", 6, L"FirstChanceBoost.esp"),
    MakeSignal("first_chance_context", L"firstchanceboost", L"FirstChanceBoost", 3, L"", L"FirstChanceBoost", L"FirstChanceBoost.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "related");
  assert(!candidates[0].cross_validated);
}

void TestCrossValidatedCandidateRetainsFirstChanceFamily()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"firstchancecross", L"FirstChanceCross.esp", 6, L"FirstChanceCross.esp"),
    MakeSignal("actionable_stack", L"firstchancecross", L"First Chance Cross", 5, L"", L"First Chance Cross", L"FirstChanceCross.dll"),
    MakeSignal("first_chance_context", L"firstchancecross", L"First Chance Cross", 3, L"", L"First Chance Cross", L"FirstChanceCross.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "cross_validated");
  assert(candidates[0].cross_validated);
  assert(candidates[0].supporting_families.size() == 3);
}

void TestFrameAndStackStayRelatedWhileOutrankingObjectRefHistory()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_frame", L"precisiondll", L"Precision.dll", 6, L"", L"Precision - Accurate Melee Collisions", L"Precision.dll"),
    MakeSignal("actionable_stack", L"precisiondll", L"Precision - Accurate Melee Collisions", 5, L"", L"Precision - Accurate Melee Collisions", L"Precision.dll"),
    MakeSignal("crash_logger_object_ref", L"otherref", L"OtherRef.esp", 6, L"OtherRef.esp"),
    MakeSignal("history_repeat", L"otherref", L"OtherRef.esp", 3, L"OtherRef.esp"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 2);
  assert(candidates[0].display_name == L"Precision.dll");
  AssertStatus(candidates[0], "related");
  assert(!candidates[0].cross_validated);
  assert(candidates[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kMedium);
  assert(!candidates[0].has_conflict);

  assert(candidates[1].display_name == L"OtherRef.esp");
  AssertStatus(candidates[1], "related");
  assert(!candidates[1].cross_validated);
  assert(!candidates[1].has_conflict);
}

void TestFrameAndStackStayRelatedWhileOutrankingIsolatedObjectRef()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_frame", L"precisiondll", L"Precision.dll", 6, L"", L"Precision - Accurate Melee Collisions", L"Precision.dll"),
    MakeSignal("actionable_stack", L"precisiondll", L"Precision - Accurate Melee Collisions", 5, L"", L"Precision - Accurate Melee Collisions", L"Precision.dll"),
    MakeSignal("crash_logger_object_ref", L"otherref", L"OtherRef.esp", 6, L"OtherRef.esp"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 2);
  assert(candidates[0].display_name == L"Precision.dll");
  AssertStatus(candidates[0], "related");
  assert(!candidates[0].cross_validated);
  assert(candidates[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kMedium);
  assert(!candidates[0].has_conflict);

  assert(candidates[1].display_name == L"OtherRef.esp");
  AssertStatus(candidates[1], "reference_clue");
  assert(!candidates[1].cross_validated);
  assert(!candidates[1].has_conflict);
}

void TestStrongFrameOnlyBecomesRelated()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_frame", L"paragonperks", L"ParagonPerks.dll", 6, L"", L"Paragon Perks", L"ParagonPerks.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "related");
  assert(!candidates[0].cross_validated);
  assert(!candidates[0].has_conflict);
  assert(candidates[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kMedium);
}

void TestWeakFrameOnlyStaysReferenceClue()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_frame", L"paragonperks", L"ParagonPerks.dll", 5, L"", L"Paragon Perks", L"ParagonPerks.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "reference_clue");
  assert(!candidates[0].cross_validated);
  assert(!candidates[0].has_conflict);
}

void TestFrameAndFirstChanceBecomeRelated()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_frame", L"earlywinner", L"EarlyWinner.dll", 6, L"", L"Early Winner", L"EarlyWinner.dll"),
    MakeSignal("first_chance_context", L"earlywinner", L"Early Winner", 3, L"", L"Early Winner", L"EarlyWinner.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "related");
  assert(!candidates[0].cross_validated);
  assert(!candidates[0].has_conflict);
  assert(candidates[0].supporting_families.size() == 2);
}

void TestFrameAndHistoryBecomeRelated()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_frame", L"repeatdll", L"RepeatDll.dll", 6, L"", L"Repeat DLL Mod", L"RepeatDll.dll"),
    MakeSignal("history_repeat", L"repeatdll", L"RepeatDll.dll", 3, L"", L"Repeat DLL Mod", L"RepeatDll.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "related");
  assert(!candidates[0].cross_validated);
  assert(!candidates[0].has_conflict);
  assert(candidates[0].supporting_families.size() == 2);
}

void TestFrameAndResourceBecomeRelated()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_frame", L"resourcedll", L"ResourceDll.dll", 6, L"", L"Resource DLL Mod", L"ResourceDll.dll"),
    MakeSignal("resource_provider", L"resourcedll", L"Resource DLL Mod", 4, L"", L"Resource DLL Mod", L"ResourceDll.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "related");
  assert(!candidates[0].cross_validated);
  assert(!candidates[0].has_conflict);
  assert(candidates[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kMedium);
  assert(candidates[0].supporting_families.size() == 2);
}

void TestBoostOnlyFamiliesDoNotDemoteBaseClassification()
{
  const auto strongFrame = BuildCandidateConsensus({
    MakeSignal("crash_logger_frame", L"strong-frame", L"StrongFrame.dll", 6, L"", L"Strong Frame", L"StrongFrame.dll"),
  }, Language::kEnglish);
  const auto boostedStrongFrame = BuildCandidateConsensus({
    MakeSignal("crash_logger_frame", L"strong-frame", L"StrongFrame.dll", 6, L"", L"Strong Frame", L"StrongFrame.dll"),
    MakeSignal("history_repeat", L"strong-frame", L"Strong Frame", 3, L"", L"Strong Frame", L"StrongFrame.dll"),
    MakeSignal("resource_provider", L"strong-frame", L"Strong Frame", 4, L"", L"Strong Frame", L"StrongFrame.dll"),
  }, Language::kEnglish);
  assert(strongFrame.size() == 1u);
  assert(boostedStrongFrame.size() == 1u);
  assert(boostedStrongFrame[0].status_id == strongFrame[0].status_id);
  assert(boostedStrongFrame[0].confidence_level == strongFrame[0].confidence_level);
  assert(strongFrame[0].status_id == "related");
  assert(strongFrame[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kMedium);
  assert(strongFrame[0].score == 6u);
  assert(boostedStrongFrame[0].score == 13u);

  const auto strongStack = BuildCandidateConsensus({
    MakeSignal("actionable_stack", L"strong-stack", L"Strong Stack", 5, L"", L"Strong Stack", L"StrongStack.dll"),
  }, Language::kEnglish);
  const auto boostedStrongStack = BuildCandidateConsensus({
    MakeSignal("actionable_stack", L"strong-stack", L"Strong Stack", 5, L"", L"Strong Stack", L"StrongStack.dll"),
    MakeSignal("history_repeat", L"strong-stack", L"Strong Stack", 3, L"", L"Strong Stack", L"StrongStack.dll"),
    MakeSignal("resource_provider", L"strong-stack", L"Strong Stack", 4, L"", L"Strong Stack", L"StrongStack.dll"),
  }, Language::kEnglish);
  assert(strongStack.size() == 1u);
  assert(boostedStrongStack.size() == 1u);
  assert(boostedStrongStack[0].status_id == strongStack[0].status_id);
  assert(boostedStrongStack[0].confidence_level == strongStack[0].confidence_level);
  assert(strongStack[0].status_id == "related");
  assert(strongStack[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kMedium);
  assert(strongStack[0].score == 5u);
  assert(boostedStrongStack[0].score == 12u);

  const auto objectRefWithHistory = BuildCandidateConsensus({
    MakeSignal("crash_logger_object_ref", L"object-history", L"ObjectHistory.esp", 6, L"ObjectHistory.esp"),
    MakeSignal("history_repeat", L"object-history", L"ObjectHistory.esp", 2, L"ObjectHistory.esp"),
  }, Language::kEnglish);
  const auto boostedObjectRefWithHistory = BuildCandidateConsensus({
    MakeSignal("crash_logger_object_ref", L"object-history", L"ObjectHistory.esp", 6, L"ObjectHistory.esp"),
    MakeSignal("history_repeat", L"object-history", L"ObjectHistory.esp", 2, L"ObjectHistory.esp"),
    MakeSignal("resource_provider", L"object-history", L"Object History", 4, L"ObjectHistory.esp"),
  }, Language::kEnglish);
  assert(objectRefWithHistory.size() == 1u);
  assert(boostedObjectRefWithHistory.size() == 1u);
  assert(boostedObjectRefWithHistory[0].status_id == objectRefWithHistory[0].status_id);
  assert(boostedObjectRefWithHistory[0].confidence_level == objectRefWithHistory[0].confidence_level);
  assert(objectRefWithHistory[0].status_id == "related");
  assert(objectRefWithHistory[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kLow);
  assert(objectRefWithHistory[0].score == 8u);
  assert(boostedObjectRefWithHistory[0].score == 12u);
}

void TestResourceBoostDoesNotCreateConflictForSecondaryObjectRefClue()
{
  const auto base = BuildCandidateConsensus({
    MakeSignal("crash_logger_frame", L"fault-owner", L"FaultOwner.dll", 6, L"", L"Fault Owner", L"FaultOwner.dll"),
    MakeSignal("actionable_stack", L"fault-owner", L"Fault Owner", 5, L"", L"Fault Owner", L"FaultOwner.dll"),
    MakeSignal("crash_logger_object_ref", L"object-owner", L"ObjectOwner.esp", 6, L"ObjectOwner.esp"),
  }, Language::kEnglish);
  const auto boosted = BuildCandidateConsensus({
    MakeSignal("crash_logger_frame", L"fault-owner", L"FaultOwner.dll", 6, L"", L"Fault Owner", L"FaultOwner.dll"),
    MakeSignal("actionable_stack", L"fault-owner", L"Fault Owner", 5, L"", L"Fault Owner", L"FaultOwner.dll"),
    MakeSignal("crash_logger_object_ref", L"object-owner", L"ObjectOwner.esp", 6, L"ObjectOwner.esp"),
    MakeSignal("resource_provider", L"object-owner", L"Object Owner", 5, L"ObjectOwner.esp"),
  }, Language::kEnglish);

  assert(base.size() == 2u);
  assert(boosted.size() == 2u);
  assert(base[0].module_filename == L"FaultOwner.dll");
  assert(boosted[0].module_filename == L"FaultOwner.dll");
  AssertStatus(base[0], "related");
  AssertStatus(boosted[0], "related");
  assert(!base[0].has_conflict);
  assert(!boosted[0].has_conflict);

  assert(base[1].plugin_name == L"ObjectOwner.esp");
  assert(boosted[1].plugin_name == L"ObjectOwner.esp");
  AssertStatus(base[1], "reference_clue");
  AssertStatus(boosted[1], "reference_clue");
  assert(!base[1].has_conflict);
  assert(!boosted[1].has_conflict);
  assert(base[1].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kLow);
  assert(boosted[1].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kLow);
  assert(base[1].score == 6u);
  assert(boosted[1].score == 11u);
}

void TestFrameAndStackRemainMediumWithAuxiliaryFamilies()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_frame", L"frame-stack", L"FrameStack.dll", 8, L"", L"Frame Stack", L"FrameStack.dll"),
    MakeSignal("actionable_stack", L"frame-stack", L"Frame Stack", 5, L"", L"Frame Stack", L"FrameStack.dll"),
    MakeSignal("first_chance_context", L"frame-stack", L"Frame Stack", 3, L"", L"Frame Stack", L"FrameStack.dll"),
    MakeSignal("hang_thread_group", L"frame-stack", L"Frame Stack", 5, L"", L"Frame Stack", L"FrameStack.dll"),
    MakeSignal("history_repeat", L"frame-stack", L"Frame Stack", 3, L"", L"Frame Stack", L"FrameStack.dll"),
    MakeSignal("resource_provider", L"frame-stack", L"Frame Stack", 5, L"", L"Frame Stack", L"FrameStack.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1u);
  AssertStatus(candidates[0], "related");
  assert(!candidates[0].cross_validated);
  assert(candidates[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kMedium);
  assert(candidates[0].score == 29u);
}

void TestBoostOnlyScoresDoNotFillGeneralRelatedThreshold()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_frame", L"weak-core", L"WeakCore.dll", 3, L"", L"Weak Core", L"WeakCore.dll"),
    MakeSignal("first_chance_context", L"weak-core", L"Weak Core", 3, L"", L"Weak Core", L"WeakCore.dll"),
    MakeSignal("history_repeat", L"weak-core", L"Weak Core", 3, L"", L"Weak Core", L"WeakCore.dll"),
    MakeSignal("resource_provider", L"weak-core", L"Weak Core", 5, L"", L"Weak Core", L"WeakCore.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1u);
  AssertStatus(candidates[0], "reference_clue");
  assert(!candidates[0].cross_validated);
  assert(candidates[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kMedium);
  assert(candidates[0].score == 14u);
}

void TestSortActionableCandidatesRestoresStatusPriorityAndKeepsScoreRanking()
{
  ActionableCandidate downgradedDirectFault{};
  downgradedDirectFault.display_name = L"DowngradedDirectFault.dll";
  downgradedDirectFault.status_id = "reference_clue";
  downgradedDirectFault.confidence_level = skydiag::dump_tool::i18n::ConfidenceLevel::kLow;
  downgradedDirectFault.score = 40u;
  downgradedDirectFault.family_count = 3u;

  ActionableCandidate relatedCandidate{};
  relatedCandidate.display_name = L"RelatedCandidate.dll";
  relatedCandidate.status_id = "related";
  relatedCandidate.confidence_level = skydiag::dump_tool::i18n::ConfidenceLevel::kMedium;
  relatedCandidate.score = 7u;
  relatedCandidate.family_count = 2u;

  std::vector<ActionableCandidate> postprocessed = { downgradedDirectFault, relatedCandidate };
  SortActionableCandidates(postprocessed);
  assert(postprocessed[0].display_name == L"RelatedCandidate.dll");
  assert(postprocessed[1].display_name == L"DowngradedDirectFault.dll");

  ActionableCandidate higherScore = relatedCandidate;
  higherScore.display_name = L"HigherScore.dll";
  higherScore.score = 15u;
  std::vector<ActionableCandidate> sameStatus = { relatedCandidate, higherScore };
  SortActionableCandidates(sameStatus);
  assert(sameStatus[0].display_name == L"HigherScore.dll");
  assert(sameStatus[0].score == 15u);
  assert(sameStatus[1].score == 7u);
}

void TestTotalScoreStillRanksCandidatesWithinSameStatus()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_frame", L"plain-core", L"PlainCore.dll", 4, L"", L"Plain Core", L"PlainCore.dll"),
    MakeSignal("first_chance_context", L"plain-core", L"Plain Core", 3, L"", L"Plain Core", L"PlainCore.dll"),
    MakeSignal("crash_logger_frame", L"boosted-core", L"BoostedCore.dll", 4, L"", L"Boosted Core", L"BoostedCore.dll"),
    MakeSignal("first_chance_context", L"boosted-core", L"Boosted Core", 3, L"", L"Boosted Core", L"BoostedCore.dll"),
    MakeSignal("history_repeat", L"boosted-core", L"Boosted Core", 3, L"", L"Boosted Core", L"BoostedCore.dll"),
    MakeSignal("resource_provider", L"boosted-core", L"Boosted Core", 5, L"", L"Boosted Core", L"BoostedCore.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 2u);
  AssertStatus(candidates[0], "related");
  AssertStatus(candidates[1], "related");
  assert(candidates[0].module_filename == L"BoostedCore.dll");
  assert(candidates[0].score == 15u);
  assert(candidates[1].module_filename == L"PlainCore.dll");
  assert(candidates[1].score == 7u);
}

void TestResourceOnlyDoesNotCreateStandaloneCandidate()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("resource_provider", L"sparkpatch", L"Spark Patch", 5, L"", L"Spark Patch"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.empty());
}

void TestCaptureQualityDoesNotCrossValidateWeakStackAgreement()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"captureboost", L"CaptureBoost.esp", 6, L"CaptureBoost.esp"),
    MakeSignal("actionable_stack", L"captureboost", L"Capture Boost", 3, L"", L"Capture Boost", L"captureboost.dll"),
    MakeSignal("capture_quality_stack", L"captureboost", L"Capture Boost", 1, L"", L"Capture Boost", L"captureboost.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "related");
  assert(!candidates[0].cross_validated);
  assert(candidates[0].score == 9u);
  assert(candidates[0].family_count == 2u);
  assert(candidates[0].supporting_families.size() == 2u);
}

void TestCaptureQualityDoesNotUpgradeStandaloneStack()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("actionable_stack", L"capturestack", L"Capture Stack", 4, L"", L"Capture Stack", L"capturestack.dll"),
    MakeSignal("capture_quality_stack", L"capturestack", L"Capture Stack", 1, L"", L"Capture Stack", L"capturestack.dll"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 1);
  AssertStatus(candidates[0], "related");
  assert(!candidates[0].cross_validated);
  assert(candidates[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kLow);
  assert(candidates[0].score == 4u);
  assert(candidates[0].family_count == 1u);
  assert(candidates[0].supporting_families.size() == 1u);
  assert(candidates[0].supporting_families[0] == "actionable_stack");
}

void TestCanonicalCandidateKeyPreservesUnicodeAndSeparators()
{
  assert(CanonicalCandidateKey(L"A-B.esp") == L"a-b");
  assert(CanonicalCandidateKey(L"AB.dll") == L"ab");
  assert(CanonicalCandidateKey(L"A-B.esp") != CanonicalCandidateKey(L"AB.dll"));
  assert(CanonicalCandidateKey(L"한글모드.esp") == L"한글모드");
  assert(CanonicalCandidateKey(L"다른모드.esp") == L"다른모드");
  assert(CanonicalCandidateKey(L"한글모드.esp") != CanonicalCandidateKey(L"다른모드.esp"));
  assert(CanonicalCandidateKey(L"My__Mod.esm") == L"my-mod");
  assert(CanonicalCandidateKey(L"MÓD.esp") == CanonicalCandidateKey(L"mód.ESP"));
  assert(CanonicalCandidateKey(L"МОД.esp") == CanonicalCandidateKey(L"мод.ESP"));
  assert(CanonicalCandidateKey(L"A！B.esl") == L"a-b");
}

void TestProductionNamesCanonicalizeIntoQualifiedCrossValidation()
{
  const auto objectRefKey = CanonicalCandidateKey(L"Quest Runtime Fix.esp");
  const auto stackKey = CanonicalCandidateKey(L"Quest Runtime Fix");
  assert(objectRefKey == stackKey);

  const auto candidates = BuildCandidateConsensus({
    MakeSignal("crash_logger_object_ref", objectRefKey, L"Quest Runtime Fix.esp", 6, L"Quest Runtime Fix.esp"),
    MakeSignal("actionable_stack", stackKey, L"Quest Runtime Fix", 5, L"", L"Quest Runtime Fix", L"QuestRuntimeFix.dll"),
  }, Language::kEnglish);
  assert(candidates.size() == 1u);
  AssertStatus(candidates[0], "cross_validated");
  assert(candidates[0].cross_validated);
  assert(candidates[0].confidence_level == skydiag::dump_tool::i18n::ConfidenceLevel::kHigh);
  assert(candidates[0].score == 11u);
}

void TestConsensusCanonicalizationDoesNotMergeSeparatedNames()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_object_ref", L"A-B.esp", L"A-B.esp", 3, L"A-B.esp"),
    MakeSignal("crash_logger_object_ref", L"AB.esp", L"AB.esp", 3, L"AB.esp"),
    MakeSignal("crash_logger_object_ref", L"한글모드.esp", L"한글모드.esp", 3, L"한글모드.esp"),
    MakeSignal("crash_logger_object_ref", L"다른모드.esp", L"다른모드.esp", 3, L"다른모드.esp"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 4u);
}

void TestConflictingEvidenceOutranksResourceOnlyRelatedCandidate()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_frame", L"frame-owner", L"FrameOwner.dll", 6, L"", L"Frame Owner", L"FrameOwner.dll"),
    MakeSignal("actionable_stack", L"stack-owner", L"StackOwner.dll", 5, L"", L"Stack Owner", L"StackOwner.dll"),
    MakeSignal("resource_provider", L"recent-resource", L"Recent Resource", 5, L"", L"Recent Resource"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 2u);
  AssertStatus(candidates[0], "conflicting");
  AssertStatus(candidates[1], "conflicting");
}

void TestExceptionThreadSelectionPolicyIsOrderIndependent()
{
  assert(scoring_policy::ShouldSelectStackwalkCandidate(false, 0u, 0u, true, 40u, 4u, 40u));
  assert(!scoring_policy::ShouldSelectStackwalkCandidate(true, 40u, 4u, true, 90u, 50u, 40u));
  assert(scoring_policy::ShouldSelectStackwalkCandidate(true, 90u, 50u, true, 40u, 4u, 40u));
  assert(scoring_policy::ShouldSelectStackwalkCandidate(true, 10u, 4u, true, 20u, 5u, 0u));
}

void TestHookFallbackPromotionRequiresNearTieAndMinimumEvidence()
{
  assert(!scoring_policy::ShouldPromoteHookFallback(28u, 1u, 4u, 4u));
  assert(!scoring_policy::ShouldPromoteHookFallback(28u, 3u, 4u, 30u));
  assert(scoring_policy::ShouldPromoteHookFallback(28u, 24u, 4u, 4u));
}

void TestSecondaryCallstackConfidencePolicyRejectsOnePointCandidates()
{
  assert(!scoring_policy::SecondaryCallstackCanBeMedium(1u, 11u, 12u, 6u));
  assert(scoring_policy::SecondaryCallstackCanBeMedium(12u, 6u, 12u, 6u));
}

void TestFirstChanceFamilySourceContract()
{
  const auto root = ProjectRoot();
  const auto source = ReadAllText(root / "dump_tool" / "src" / "CandidateConsensus.cpp");
  AssertContains(source, "first_chance_context", "Candidate consensus must recognize first_chance_context as a supporting family.");
}

void TestWeakFaultLocationRequiresQualifiedIndependentSupport()
{
  const auto root = ProjectRoot();
  const auto source = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderUtil.cpp");
  AssertContains(source, "candidate.has_conflict || candidate.status_id == \"conflicting\"",
                 "Fault-location post-processing must preserve explicit conflicts.");
  AssertContains(source, "candidate.cross_validated && hasCrashLoggerObjectRef && hasActionableStack",
                  "A direct-fault frame must remain weak without qualified object-ref and formal-stack support.");
}

void TestWeakFaultLocationPostprocessingResortsCandidates()
{
  const auto root = ProjectRoot();
  const auto source = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderCandidates.cpp");
  const auto consensusPos = source.find("r.actionable_candidates = BuildCandidateConsensus(signals, lang);");
  const auto loopPos = source.find("for (auto& candidate : r.actionable_candidates)", consensusPos);
  const auto downgradePos = source.find("candidate.status_id = hasStackSupport ? \"related\" : \"reference_clue\";", loopPos);
  const auto resortPos = source.find("SortActionableCandidates(r.actionable_candidates);", downgradePos);
  assert(consensusPos != std::string::npos);
  assert(loopPos != std::string::npos && loopPos > consensusPos);
  assert(downgradePos != std::string::npos && downgradePos > loopPos);
  assert(resortPos != std::string::npos && resortPos > downgradePos);
}

}  // namespace

int main()
{
  TestCrossValidatedObjectRefAndStackAgreement();
  TestObjectRefOnlyStaysReferenceClue();
  TestObjectRefAndStackConflictStayConflicting();
  TestSecondaryObjectRefCanStillCrossValidate();
  TestRepresentativeNamePrefersPluginFilenameOverFriendlyLabel();
  TestRepresentativeNamePrefersDllFilenameOverModFolderName();
  TestStrongStackOnlyBecomesMediumRelated();
  TestHangThreadGroupCorroboratesWeakMainStack();
  TestObjectRefAndResourceStayReferenceOnly();
  TestHistoryOnlyDoesNotCreateStandaloneCandidate();
  TestObjectRefAndHistoryRepeatBecomeRelated();
  TestWeakStackAgreementStaysRelated();
  TestObjectRefAndPointerScanCannotCrossValidateWithBoostOnlyScores();
  TestAuxiliaryCoreFamiliesCannotFillIndependentHighThreshold();
  TestObjectRefAndFormalStackCrossValidateAtIndependentThreshold();
  TestFirstChanceOnlyDoesNotCreateStandaloneCandidate();
  TestObjectRefAndFirstChanceBecomeRelated();
  TestCrossValidatedCandidateRetainsFirstChanceFamily();
  TestFrameAndStackStayRelatedWhileOutrankingObjectRefHistory();
  TestFrameAndStackStayRelatedWhileOutrankingIsolatedObjectRef();
  TestStrongFrameOnlyBecomesRelated();
  TestWeakFrameOnlyStaysReferenceClue();
  TestFrameAndFirstChanceBecomeRelated();
  TestFrameAndHistoryBecomeRelated();
  TestFrameAndResourceBecomeRelated();
  TestBoostOnlyFamiliesDoNotDemoteBaseClassification();
  TestResourceBoostDoesNotCreateConflictForSecondaryObjectRefClue();
  TestFrameAndStackRemainMediumWithAuxiliaryFamilies();
  TestBoostOnlyScoresDoNotFillGeneralRelatedThreshold();
  TestSortActionableCandidatesRestoresStatusPriorityAndKeepsScoreRanking();
  TestTotalScoreStillRanksCandidatesWithinSameStatus();
  TestResourceOnlyDoesNotCreateStandaloneCandidate();
  TestCaptureQualityDoesNotCrossValidateWeakStackAgreement();
  TestCaptureQualityDoesNotUpgradeStandaloneStack();
  TestCanonicalCandidateKeyPreservesUnicodeAndSeparators();
  TestProductionNamesCanonicalizeIntoQualifiedCrossValidation();
  TestConsensusCanonicalizationDoesNotMergeSeparatedNames();
  TestConflictingEvidenceOutranksResourceOnlyRelatedCandidate();
  TestExceptionThreadSelectionPolicyIsOrderIndependent();
  TestHookFallbackPromotionRequiresNearTieAndMinimumEvidence();
  TestSecondaryCallstackConfidencePolicyRejectsOnePointCandidates();
  TestFirstChanceFamilySourceContract();
  TestWeakFaultLocationRequiresQualifiedIndependentSupport();
  TestWeakFaultLocationPostprocessingResortsCandidates();
  return 0;
}
