#include "FreezeCandidateConsensus.h"
#include "SourceGuardTestUtils.h"
#include "WctTypes.h"

#include <filesystem>

using skydiag::dump_tool::ActionableCandidate;
using skydiag::dump_tool::BuildFreezeCandidateConsensus;
using skydiag::dump_tool::FreezeSignalInput;
using skydiag::dump_tool::i18n::ConfidenceLevel;
using skydiag::dump_tool::i18n::ConfidenceText;
using skydiag::dump_tool::i18n::Language;
using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::ReadAllText;

namespace {

ActionableCandidate MakeCandidate(std::wstring name, ConfidenceLevel level = ConfidenceLevel::kMedium)
{
  ActionableCandidate candidate{};
  candidate.display_name = std::move(name);
  candidate.confidence_level = level;
  candidate.confidence = ConfidenceText(Language::kEnglish, level);
  return candidate;
}

void TestSourceContracts()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  const auto analyzerHeader = repoRoot / "dump_tool" / "src" / "Analyzer.h";
  const auto analyzerCpp = repoRoot / "dump_tool" / "src" / "Analyzer.cpp";
  const auto freezeConsensusHeader = repoRoot / "dump_tool" / "src" / "FreezeCandidateConsensus.h";
  const auto freezeConsensusCpp = repoRoot / "dump_tool" / "src" / "FreezeCandidateConsensus.cpp";

  assert(std::filesystem::exists(analyzerHeader) && "dump_tool/src/Analyzer.h not found");
  assert(std::filesystem::exists(analyzerCpp) && "dump_tool/src/Analyzer.cpp not found");
  assert(std::filesystem::exists(freezeConsensusHeader) && "dump_tool/src/FreezeCandidateConsensus.h not found");
  assert(std::filesystem::exists(freezeConsensusCpp) && "dump_tool/src/FreezeCandidateConsensus.cpp not found");

  const auto analyzerHeaderText = ReadAllText(analyzerHeader);
  const auto analyzerCppText = ReadAllText(analyzerCpp);
  const auto freezeConsensusHeaderText = ReadAllText(freezeConsensusHeader);
  const auto freezeConsensusCppText = ReadAllText(freezeConsensusCpp);

  AssertContains(analyzerHeaderText, "struct FreezeAnalysisResult", "AnalysisResult must expose a structured freeze analysis model.");
  AssertContains(analyzerHeaderText, "freeze_analysis", "AnalysisResult must store freeze analysis output.");
  AssertContains(analyzerHeaderText, "deadlock_likely", "Freeze analysis state ids must include deadlock_likely.");
  AssertContains(analyzerHeaderText, "loader_stall_likely", "Freeze analysis state ids must include loader_stall_likely.");
  AssertContains(analyzerHeaderText, "freeze_candidate", "Freeze analysis state ids must include freeze_candidate.");
  AssertContains(analyzerHeaderText, "freeze_ambiguous", "Freeze analysis state ids must include freeze_ambiguous.");

  AssertContains(freezeConsensusHeaderText, "BuildFreezeCandidateConsensus", "Freeze candidate consensus entry point must exist.");
  AssertContains(freezeConsensusCppText, "deadlock_likely", "Freeze candidate consensus must classify deadlock_likely.");
  AssertContains(freezeConsensusCppText, "loader_stall_likely", "Freeze candidate consensus must classify loader_stall_likely.");
  AssertContains(freezeConsensusCppText, "freeze_candidate", "Freeze candidate consensus must classify freeze_candidate.");
  AssertContains(freezeConsensusCppText, "freeze_ambiguous", "Freeze candidate consensus must classify freeze_ambiguous.");
  AssertContains(analyzerCppText, "BuildFreezeCandidateConsensus", "Analyzer must invoke freeze candidate consensus for freeze-like dumps.");
}

void TestWctFreezeSummaryParsing()
{
  const std::string json =
    R"({"capture":{"kind":"hang","secondsSinceHeartbeat":15.0,"thresholdSec":10,"isLoading":true,"pss_snapshot_requested":true,"pss_snapshot_used":true,"pss_snapshot_capture_ms":42,"pss_snapshot_status":"ok","dump_transport":"pss_snapshot"},"threads":[{"tid":1234,"isCycle":true},{"tid":5678,"isCycle":false,"nodes":[{"thread":{"waitTime":500}}]}]})";

  const auto parsed = skydiag::dump_tool::internal::TryParseWctFreezeSummary(json);
  assert(parsed.has_value());
  assert(parsed->has);
  assert(parsed->threads == 2);
  assert(parsed->cycles == 1);
  assert(parsed->cycle_thread_ids.size() == 1);
  assert(parsed->cycle_thread_ids[0] == 1234u);
  assert(parsed->longest_wait_tid == 5678u);
  assert(parsed->longest_wait_ms == 500u);
  assert(parsed->has_capture);
  assert(parsed->capture_kind == "hang");
  assert(parsed->isLoading);
  assert(parsed->suggestsHang);
  assert(parsed->pss_snapshot_requested);
  assert(parsed->pss_snapshot_used);
  assert(parsed->pss_snapshot_capture_ms == 42u);
  assert(parsed->pss_snapshot_status == "ok");
  assert(parsed->dump_transport == "pss_snapshot");
}

void TestConsensusDeadlockLikely()
{
  FreezeSignalInput input{};
  input.is_hang_like = true;
  input.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  input.wct->has = true;
  input.wct->cycles = 2;
  input.wct->pss_snapshot_used = true;

  const auto result = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(result.has_analysis);
  assert(result.state_id == "deadlock_likely");
  assert(result.support_quality == "snapshot_backed");
  assert(result.confidence_level == ConfidenceLevel::kHigh);
}

void TestConsensusLoaderStallLikely()
{
  FreezeSignalInput input{};
  input.is_hang_like = true;
  input.loading_context = true;
  input.wct = skydiag::dump_tool::internal::WctFreezeSummary{};
  input.wct->has = true;
  input.wct->has_capture = true;
  input.wct->capture_kind = "hang";
  input.wct->isLoading = true;

  const auto result = BuildFreezeCandidateConsensus(input, Language::kEnglish);
  assert(result.has_analysis);
  assert(result.state_id == "loader_stall_likely");
  assert(result.confidence_level == ConfidenceLevel::kMedium);
}

void TestConsensusFreezeCandidateAndAmbiguous()
{
  FreezeSignalInput candidateInput{};
  candidateInput.is_manual_capture = true;
  candidateInput.actionable_candidates.push_back(MakeCandidate(L"ExampleFreezeMod"));

  const auto candidateResult = BuildFreezeCandidateConsensus(candidateInput, Language::kEnglish);
  assert(candidateResult.has_analysis);
  assert(candidateResult.state_id == "freeze_candidate");
  assert(candidateResult.related_candidates.size() == 1);
  assert(candidateResult.related_candidates[0].display_name == L"ExampleFreezeMod");

  FreezeSignalInput ambiguousInput{};
  ambiguousInput.is_manual_capture = true;

  const auto ambiguousResult = BuildFreezeCandidateConsensus(ambiguousInput, Language::kEnglish);
  assert(ambiguousResult.has_analysis);
  assert(ambiguousResult.state_id == "freeze_ambiguous");
  assert(ambiguousResult.related_candidates.empty());
}

}  // namespace

int main()
{
  TestSourceContracts();
  TestWctFreezeSummaryParsing();
  TestConsensusDeadlockLikely();
  TestConsensusLoaderStallLikely();
  TestConsensusFreezeCandidateAndAmbiguous();
  return 0;
}
