#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "CandidateConsensus.h"

using skydiag::dump_tool::ActionableCandidate;
using skydiag::dump_tool::BuildCandidateConsensus;
using skydiag::dump_tool::CandidateSignal;
using skydiag::dump_tool::i18n::Language;

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
  TestObjectRefAndResourceNeedMediumOnly();
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
  TestFirstChanceFamilySourceContract();
  return 0;
}
