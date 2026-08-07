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

void TestFrameAndStackOutrankObjectRefHistoryWithoutConflict()
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
  AssertStatus(candidates[0], "cross_validated");
  assert(candidates[0].cross_validated);
  assert(!candidates[0].has_conflict);

  assert(candidates[1].display_name == L"OtherRef.esp");
  AssertStatus(candidates[1], "related");
  assert(!candidates[1].cross_validated);
  assert(!candidates[1].has_conflict);
}

void TestFrameAndStackOutrankIsolatedObjectRefWithoutConflict()
{
  const std::vector<CandidateSignal> signals = {
    MakeSignal("crash_logger_frame", L"precisiondll", L"Precision.dll", 6, L"", L"Precision - Accurate Melee Collisions", L"Precision.dll"),
    MakeSignal("actionable_stack", L"precisiondll", L"Precision - Accurate Melee Collisions", 5, L"", L"Precision - Accurate Melee Collisions", L"Precision.dll"),
    MakeSignal("crash_logger_object_ref", L"otherref", L"OtherRef.esp", 6, L"OtherRef.esp"),
  };

  const auto candidates = BuildCandidateConsensus(signals, Language::kEnglish);
  assert(candidates.size() == 2);
  assert(candidates[0].display_name == L"Precision.dll");
  AssertStatus(candidates[0], "cross_validated");
  assert(candidates[0].cross_validated);
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
  assert(candidates[0].supporting_families.size() == 2);
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

void TestSecondaryConfidencePolicyRejectsOnePointCandidates()
{
  assert(!scoring_policy::SecondaryCallstackCanBeMedium(1u, 11u, 12u, 6u));
  assert(scoring_policy::SecondaryCallstackCanBeMedium(12u, 6u, 12u, 6u));
  assert(!scoring_policy::SecondaryStackScanCanBeMedium(1u, 40u));
  assert(scoring_policy::SecondaryStackScanCanBeMedium(40u, 40u));
}

void TestFirstChanceFamilySourceContract()
{
  const auto root = ProjectRoot();
  const auto source = ReadAllText(root / "dump_tool" / "src" / "CandidateConsensus.cpp");
  AssertContains(source, "first_chance_context", "Candidate consensus must recognize first_chance_context as a supporting family.");
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
  TestFirstChanceOnlyDoesNotCreateStandaloneCandidate();
  TestObjectRefAndFirstChanceBecomeRelated();
  TestCrossValidatedCandidateRetainsFirstChanceFamily();
  TestFrameAndStackOutrankObjectRefHistoryWithoutConflict();
  TestFrameAndStackOutrankIsolatedObjectRefWithoutConflict();
  TestStrongFrameOnlyBecomesRelated();
  TestWeakFrameOnlyStaysReferenceClue();
  TestFrameAndFirstChanceBecomeRelated();
  TestFrameAndHistoryBecomeRelated();
  TestFrameAndResourceBecomeRelated();
  TestResourceOnlyDoesNotCreateStandaloneCandidate();
  TestCaptureQualityDoesNotCrossValidateWeakStackAgreement();
  TestCaptureQualityDoesNotUpgradeStandaloneStack();
  TestCanonicalCandidateKeyPreservesUnicodeAndSeparators();
  TestConsensusCanonicalizationDoesNotMergeSeparatedNames();
  TestConflictingEvidenceOutranksResourceOnlyRelatedCandidate();
  TestExceptionThreadSelectionPolicyIsOrderIndependent();
  TestHookFallbackPromotionRequiresNearTieAndMinimumEvidence();
  TestSecondaryConfidencePolicyRejectsOnePointCandidates();
  TestFirstChanceFamilySourceContract();
  return 0;
}
