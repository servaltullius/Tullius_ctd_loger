#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "AddressResolver.h"
#include "CrashHistory.h"
#include "SignatureDatabase.h"

using skydiag::dump_tool::AddressResolver;
using skydiag::dump_tool::CrashHistory;
using skydiag::dump_tool::CrashHistoryEntry;
using skydiag::dump_tool::ModuleStats;
using skydiag::dump_tool::BucketStats;
using skydiag::dump_tool::SignatureDatabase;
using skydiag::dump_tool::SignatureMatchInput;

namespace {

std::filesystem::path ProjectRoot()
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  assert(root && "SKYDIAG_PROJECT_ROOT must be set");
  return std::filesystem::path(root);
}

std::string ReadAllText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::in | std::ios::binary);
  assert(in && "Failed to open file");
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string ReadJoinedText(std::initializer_list<std::filesystem::path> paths)
{
  std::ostringstream ss;
  for (const auto& path : paths) {
    ss << ReadAllText(path) << "\n";
  }
  return ss.str();
}

std::string ReadEvidenceBuilderEvidenceText(const std::filesystem::path& root)
{
  return ReadJoinedText({
    root / "dump_tool" / "src" / "EvidenceBuilderEvidence.cpp",
    root / "dump_tool" / "src" / "EvidenceBuilderEvidence.Context.cpp",
    root / "dump_tool" / "src" / "EvidenceBuilderEvidence.Crash.cpp",
    root / "dump_tool" / "src" / "EvidenceBuilderEvidence.Freeze.cpp",
  });
}

std::string ReadCrashLoggerFrameFixture(const char* filename)
{
  return ReadAllText(ProjectRoot() / "tests" / "data" / "crashlogger_frame_cases" / filename);
}

void AssertContains(const std::string& haystack, const char* needle, const char* message)
{
  assert(haystack.find(needle) != std::string::npos && message);
}

void AssertNotContains(const std::string& haystack, const char* needle, const char* message)
{
  assert(haystack.find(needle) == std::string::npos && message);
}

const ModuleStats* FindModule(const std::vector<ModuleStats>& stats, const std::string& name)
{
  for (const auto& s : stats) {
    if (s.module_name == name) {
      return &s;
    }
  }
  return nullptr;
}

void TestSignatureDatabaseRuntime()
{
  SignatureDatabase db;
  const auto jsonPath = ProjectRoot() / "dump_tool" / "data" / "crash_signatures.json";
  assert(db.LoadFromJson(jsonPath));
  assert(db.Size() > 0);

  SignatureMatchInput input{};
  input.exc_code = 0xC0000005u;
  input.fault_module = L"SkyrimSE.exe";
  input.fault_offset = 0xD6DDDAull;
  input.exc_address = 0x140000000ull + input.fault_offset;
  input.fault_module_is_system = false;
  input.callstack_modules = { L"SkyrimSE.exe!BSBatchRenderer::Draw+0x2F" };

  const auto matched = db.Match(input, /*useKorean=*/false);
  assert(matched.has_value());
  assert(matched->id == "D6DDDA_VRAM");
}

void TestSignatureCallstackContainsRuntime()
{
  const auto tempPath = std::filesystem::temp_directory_path() / "skydiag_signature_runtime_test.json";
  {
    std::ofstream out(tempPath, std::ios::binary);
    out <<
      "{\n"
      "  \"version\": 1,\n"
      "  \"signatures\": [\n"
      "    {\n"
      "      \"id\": \"CALLSTACK_STRINGS\",\n"
      "      \"match\": {\n"
      "        \"callstack_contains\": [\".STRINGS\"]\n"
      "      },\n"
      "      \"diagnosis\": {\n"
      "        \"cause_ko\": \"테스트\",\n"
      "        \"cause_en\": \"test\",\n"
      "        \"confidence\": \"low\",\n"
      "        \"recommendations_ko\": [],\n"
      "        \"recommendations_en\": []\n"
      "      }\n"
      "    }\n"
      "  ]\n"
      "}\n";
  }

  SignatureDatabase db;
  assert(db.LoadFromJson(tempPath));
  SignatureMatchInput input{};
  input.callstack_modules = { L"SkyrimSE.exe!SomePath\\.STRINGS::Read" };

  const auto matched = db.Match(input, /*useKorean=*/false);
  assert(matched.has_value());
  assert(matched->id == "CALLSTACK_STRINGS");

  std::error_code ec;
  std::filesystem::remove(tempPath, ec);
}

void TestSignatureDatabaseToleratesMalformedEntries()
{
  const auto tempPath = std::filesystem::temp_directory_path() / "skydiag_signature_malformed_runtime_test.json";
  {
    std::ofstream out(tempPath, std::ios::binary);
    out <<
      "{\n"
      "  \"version\": 1,\n"
      "  \"signatures\": [\n"
      "    {\n"
      "      \"id\": \"BAD_REGEX\",\n"
      "      \"match\": {\n"
      "        \"exc_code\": \"0xC0000005\",\n"
      "        \"fault_offset_regex\": \"[\"\n"
      "      },\n"
      "      \"diagnosis\": {\n"
      "        \"cause_ko\": \"bad\",\n"
      "        \"cause_en\": \"bad\",\n"
      "        \"confidence\": \"low\",\n"
      "        \"recommendations_ko\": [],\n"
      "        \"recommendations_en\": []\n"
      "      }\n"
      "    },\n"
      "    {\n"
      "      \"id\": \"BAD_HEX\",\n"
      "      \"match\": {\n"
      "        \"exc_code\": \"0xGG\"\n"
      "      },\n"
      "      \"diagnosis\": {\n"
      "        \"cause_ko\": \"bad\",\n"
      "        \"cause_en\": \"bad\",\n"
      "        \"confidence\": \"low\",\n"
      "        \"recommendations_ko\": [],\n"
      "        \"recommendations_en\": []\n"
      "      }\n"
      "    },\n"
      "    {\n"
      "      \"id\": \"GOOD\",\n"
      "      \"match\": {\n"
      "        \"exc_code\": \"0xC0000005\",\n"
      "        \"fault_module\": \"SkyrimSE.exe\"\n"
      "      },\n"
      "      \"diagnosis\": {\n"
      "        \"cause_ko\": \"jeongsang\",\n"
      "        \"cause_en\": \"good\",\n"
      "        \"confidence\": \"medium\",\n"
      "        \"recommendations_ko\": [],\n"
      "        \"recommendations_en\": []\n"
      "      }\n"
      "    }\n"
      "  ]\n"
      "}\n";
  }

  SignatureDatabase db;
  assert(db.LoadFromJson(tempPath));
  assert(db.Size() == 1);

  SignatureMatchInput input{};
  input.exc_code = 0xC0000005u;
  input.fault_module = L"SkyrimSE.exe";
  const auto matched = db.Match(input, /*useKorean=*/false);
  assert(matched.has_value());
  assert(matched->id == "GOOD");

  std::error_code ec;
  std::filesystem::remove(tempPath, ec);
}

void TestAddressResolverRuntime()
{
  AddressResolver resolver;
  const auto jsonPath = ProjectRoot() / "dump_tool" / "data" / "address_db" / "skyrimse_functions.json";
  assert(resolver.LoadFromJson(jsonPath, "1.5.97.0"));
  assert(resolver.Size() > 0);

  const auto exact = resolver.Resolve(0xD6DDDAull);
  assert(exact.has_value());
  assert(exact.value() == "BSBatchRenderer::Draw");

  const auto fuzzy = resolver.Resolve(0xD6DDDAull + 0x20ull);
  assert(fuzzy.has_value());
  assert(fuzzy.value() == "BSBatchRenderer::Draw");

  const auto miss = resolver.Resolve(0x1ull);
  assert(!miss.has_value());
}

void TestAddressResolverToleratesMalformedEntries()
{
  const auto tempPath = std::filesystem::temp_directory_path() / "skydiag_address_resolver_malformed_runtime_test.json";
  {
    std::ofstream out(tempPath, std::ios::binary);
    out <<
      "{\n"
      "  \"game_versions\": {\n"
      "    \"1.5.97.0\": {\n"
      "      \"functions\": {\n"
      "        \"D6DDDA\": \"BSBatchRenderer::Draw\",\n"
      "        \"NOT_HEX\": \"BrokenEntry\",\n"
      "        \"D6DD10\": 42\n"
      "      }\n"
      "    }\n"
      "  }\n"
      "}\n";
  }

  AddressResolver resolver;
  assert(resolver.LoadFromJson(tempPath, "1.5.97.0"));
  assert(resolver.Size() == 1);

  const auto resolved = resolver.Resolve(0xD6DDDAull);
  assert(resolved.has_value());
  assert(resolved.value() == "BSBatchRenderer::Draw");

  std::error_code ec;
  std::filesystem::remove(tempPath, ec);
}

void TestCrashHistoryRuntime()
{
  CrashHistory history;
  for (int i = 0; i < 105; ++i) {
    CrashHistoryEntry e{};
    e.timestamp_utc = "2026-02-15T00:00:00Z";
    e.dump_file = "case_" + std::to_string(i) + ".dmp";
    e.bucket_key = "bucket-a";
    e.top_suspect = (i % 2 == 0) ? "modA.dll" : "modB.dll";
    e.confidence = "High";
    e.signature_id = (i % 3 == 0) ? "SIG" : "";
    e.all_suspects = { e.top_suspect, "shared.dll" };
    history.AddEntry(std::move(e));
  }
  assert(history.Size() == CrashHistory::kMaxEntries);

  const auto stats = history.GetModuleStats(20);
  const ModuleStats* shared = FindModule(stats, "shared.dll");
  assert(shared);
  assert(shared->total_appearances == 20u);
  assert(shared->total_crashes == 20u);

  const auto historyPath = std::filesystem::temp_directory_path() / "skydiag_crash_history_runtime_test.json";
  assert(history.SaveToFile(historyPath));

  CrashHistory loaded;
  assert(loaded.LoadFromFile(historyPath));
  assert(loaded.Size() == CrashHistory::kMaxEntries);
  const auto statsLoaded = loaded.GetModuleStats(20);
  const ModuleStats* sharedLoaded = FindModule(statsLoaded, "shared.dll");
  assert(sharedLoaded);
  assert(sharedLoaded->total_appearances == 20u);

  std::error_code ec;
  std::filesystem::remove(historyPath, ec);
}

void TestCrashHistoryBucketCorrelation()
{
  CrashHistory history;

  // Add 3 entries with bucket-a
  for (int i = 0; i < 3; ++i) {
    CrashHistoryEntry e{};
    e.timestamp_utc = "2026-02-23T0" + std::to_string(i) + ":00:00Z";
    e.dump_file = "dump_" + std::to_string(i) + ".dmp";
    e.bucket_key = "bucket-a";
    e.top_suspect = "modA.dll";
    e.all_suspects = { "modA.dll" };
    history.AddEntry(std::move(e));
  }

  // Add 1 entry with bucket-b
  {
    CrashHistoryEntry e{};
    e.timestamp_utc = "2026-02-23T03:00:00Z";
    e.dump_file = "dump_3.dmp";
    e.bucket_key = "bucket-b";
    e.top_suspect = "modB.dll";
    e.all_suspects = { "modB.dll" };
    history.AddEntry(std::move(e));
  }

  // bucket-a should have 3 occurrences
  const auto corrA = history.GetBucketStats("bucket-a");
  assert(corrA.count == 3);
  assert(corrA.first_seen == "2026-02-23T00:00:00Z");
  assert(corrA.last_seen == "2026-02-23T02:00:00Z");

  // bucket-b should have 1 occurrence
  const auto corrB = history.GetBucketStats("bucket-b");
  assert(corrB.count == 1);

  // unknown bucket should have 0 occurrences
  const auto corrC = history.GetBucketStats("bucket-c");
  assert(corrC.count == 0);

  // empty bucket key should have 0 occurrences
  const auto corrEmpty = history.GetBucketStats("");
  assert(corrEmpty.count == 0);
}

void TestCrashHistoryBucketCandidateStats()
{
  CrashHistory history;

  {
    CrashHistoryEntry e{};
    e.timestamp_utc = "2026-02-23T00:00:00Z";
    e.dump_file = "dump_0.dmp";
    e.bucket_key = "bucket-a";
    e.top_suspect = "modA.dll";
    e.candidate_keys = { "repeatmod", "sharedcandidate" };
    history.AddEntry(std::move(e));
  }
  {
    CrashHistoryEntry e{};
    e.timestamp_utc = "2026-02-23T01:00:00Z";
    e.dump_file = "dump_1.dmp";
    e.bucket_key = "bucket-a";
    e.top_suspect = "modB.dll";
    e.candidate_keys = { "repeatmod", "sharedcandidate", "repeatmod" };
    history.AddEntry(std::move(e));
  }
  {
    CrashHistoryEntry e{};
    e.timestamp_utc = "2026-02-23T02:00:00Z";
    e.dump_file = "dump_2.dmp";
    e.bucket_key = "bucket-b";
    e.top_suspect = "modC.dll";
    e.candidate_keys = { "othercandidate" };
    history.AddEntry(std::move(e));
  }

  const auto stats = history.GetBucketCandidateStats("bucket-a");
  assert(!stats.empty());
  assert(stats[0].candidate_key == "repeatmod");
  assert(stats[0].count == 2u);
  assert(stats[1].candidate_key == "sharedcandidate");
  assert(stats[1].count == 2u);

  const auto noStats = history.GetBucketCandidateStats("bucket-c");
  assert(noStats.empty());
}

void TestCaptureQualitySourceContracts()
{
  const auto root = ProjectRoot();
  const auto analyzerHeader = ReadAllText(root / "dump_tool" / "src" / "Analyzer.h");
  const auto analyzerCpp = ReadJoinedText({
    root / "dump_tool" / "src" / "Analyzer.cpp",
    root / "dump_tool" / "src" / "Analyzer.History.cpp",
  });
  const auto evidenceCpp = ReadEvidenceBuilderEvidenceText(root);
  const auto recommendationCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderRecommendations.cpp");

  AssertContains(analyzerHeader, "symbol_runtime_degraded", "AnalysisResult must track degraded symbol/runtime state.");
  AssertContains(analyzerHeader, "incident_capture_kind", "AnalysisResult must expose effective capture kind.");
  AssertContains(analyzerHeader, "incident_capture_profile_base_mode", "AnalysisResult must expose capture profile base mode.");
  AssertContains(analyzerCpp, "capture_profile", "Analyzer must consume incident capture profile metadata.");
  AssertContains(evidenceCpp, "Capture profile metadata", "Evidence must explain which capture profile produced the dump.");
  AssertContains(evidenceCpp, "Symbol/runtime environment limited stackwalk quality", "Evidence must describe symbol/runtime degradation.");
  AssertContains(recommendationCpp, "richer crash recapture profile", "Recommendations must prefer richer recapture profiles before generic full-memory advice.");
  AssertContains(recommendationCpp, "Fix dbghelp/msdia or symbol cache/path health first", "Recommendations must call out symbol/runtime remediation.");
}

void TestCrashLoggerFrameConsensusContracts()
{
  const auto root = ProjectRoot();
  const auto consensusSrc = ReadJoinedText({
    root / "dump_tool" / "src" / "CandidateConsensus.cpp",
    root / "dump_tool" / "src" / "EvidenceBuilderCandidates.cpp",
  });

  AssertContains(consensusSrc, "crash_logger_frame", "Candidate consensus must recognize the crash_logger_frame family.");
  AssertContains(consensusSrc, "actionable_stack", "Candidate consensus must still admit actionable_stack agreement.");
  AssertContains(consensusSrc, "crash_logger_object_ref", "Candidate consensus must still admit crash_logger_object_ref agreement.");
  AssertContains(consensusSrc, "cross_validated", "Candidate consensus must support cross_validated outcomes.");
  AssertContains(consensusSrc, "related", "Candidate consensus must support related outcomes.");
  AssertContains(consensusSrc, "conflicting", "Candidate consensus must support conflicting outcomes.");
}

void TestFreezeAnalysisSourceContracts()
{
  const auto root = ProjectRoot();
  const auto analyzerHeader = ReadAllText(root / "dump_tool" / "src" / "Analyzer.h");
  const auto analyzerCpp = ReadJoinedText({
    root / "dump_tool" / "src" / "Analyzer.cpp",
    root / "dump_tool" / "src" / "Analyzer.CaptureInputs.cpp",
  });
  const auto analyzerInternalsHeader = ReadAllText(root / "dump_tool" / "src" / "AnalyzerInternals.h");

  AssertContains(analyzerHeader, "FreezeAnalysisResult", "AnalysisResult must define a freeze analysis model.");
  AssertContains(analyzerHeader, "freeze_analysis", "AnalysisResult must store freeze analysis.");
  AssertContains(analyzerHeader, "BlackboxFreezeSummary", "AnalysisResult contracts must define a blackbox freeze aggregate.");
  AssertContains(analyzerHeader, "deadlock_likely", "Freeze analysis state ids must include deadlock_likely.");
  AssertContains(analyzerHeader, "loader_stall_likely", "Freeze analysis state ids must include loader_stall_likely.");
  AssertContains(analyzerHeader, "freeze_candidate", "Freeze analysis state ids must include freeze_candidate.");
  AssertContains(analyzerHeader, "freeze_ambiguous", "Freeze analysis state ids must include freeze_ambiguous.");
  AssertContains(analyzerCpp, "BuildFreezeCandidateConsensus", "Analyzer must call freeze candidate consensus.");
  AssertContains(analyzerCpp, "BlackboxFreezeSummary", "Analyzer must build a blackbox freeze summary for loader-stall analysis.");
  AssertContains(analyzerHeader, "FirstChanceSummary", "AnalysisResult contracts must define a first-chance aggregate.");
  AssertContains(analyzerCpp, "first_chance_context", "Analyzer must attach first-chance context to analysis results.");
  AssertContains(analyzerInternalsHeader, "BuildFirstChanceSummary", "Analyzer internals must expose a first-chance aggregate builder.");
  AssertContains(analyzerCpp, "BuildFirstChanceSummary", "Analyzer must build a first-chance aggregate from blackbox events.");
}

void TestFirstChanceCtdCandidateSourceContracts()
{
  const auto root = ProjectRoot();
  const auto candidateBuilder = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderCandidates.cpp");
  const auto summaryCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderSummary.cpp");
  const auto recommendationCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderRecommendations.cpp");

  AssertContains(candidateBuilder, "first_chance_context",
                 "CTD candidate aggregation must add a first_chance_context family.");
  AssertContains(candidateBuilder, "repeated_signature_count > 0u",
                 "CTD first-chance candidate boosts must require repeated suspicious signatures.");
  AssertContains(candidateBuilder, "loading_window_count",
                 "CTD first-chance candidate boosts must consider dense loading-window activity.");
  AssertContains(candidateBuilder, "recent_non_system_modules",
                 "CTD first-chance candidate boosts must link via repeated non-system modules.");
  AssertContains(candidateBuilder, "ctx.isGameExe || ctx.isSystem",
                 "CTD first-chance candidate boosts must be limited to EXE/system victims.");
  AssertNotContains(candidateBuilder, "existing object-ref/stack/resource candidate already present",
                    "CTD first-chance linkage must come from DLL/mod/plugin matches, not generic candidate presence.");
  AssertContains(summaryCpp, "first_chance_context",
                 "Summary family labels must include first_chance_context when it supports a CTD candidate.");
  AssertContains(recommendationCpp, "first-chance",
                 "Recommendations must explain repeated first-chance context for boosted CTD candidates.");
}

void TestRecaptureEvaluationConsumptionSourceContracts()
{
  const auto root = ProjectRoot();
  const auto analyzerHeader = ReadAllText(root / "dump_tool" / "src" / "Analyzer.h");
  const auto analyzerCpp = ReadJoinedText({
    root / "dump_tool" / "src" / "Analyzer.cpp",
    root / "dump_tool" / "src" / "Analyzer.History.cpp",
  });
  const auto outputWriter = ReadJoinedText({
    root / "dump_tool" / "src" / "OutputWriter.cpp",
    root / "dump_tool" / "src" / "OutputWriter.Summary.cpp",
    root / "dump_tool" / "src" / "OutputWriter.Report.cpp",
  });
  const auto evidenceCpp = ReadEvidenceBuilderEvidenceText(root);
  const auto recommendationCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderRecommendations.cpp");

  AssertContains(analyzerHeader, "incident_recapture_target_profile", "AnalysisResult must expose recapture target profile metadata.");
  AssertContains(analyzerHeader, "incident_recapture_reasons", "AnalysisResult must expose recapture reason metadata.");
  AssertContains(analyzerCpp, "recapture_evaluation", "Analyzer must load incident recapture metadata from the manifest.");
  AssertContains(outputWriter, "recapture_evaluation", "OutputWriter must consume incident recapture metadata.");
  AssertContains(outputWriter, "RecaptureReasons:", "Report text must print recapture reasons.");
  AssertContains(outputWriter, "RecaptureEscalationLevel:", "Report text must print recapture escalation level.");
  AssertContains(evidenceCpp, "Capture recapture context", "Evidence must explain why a recapture profile was chosen.");
  AssertContains(evidenceCpp, "incident_recapture_target_profile", "Evidence recapture explanations must read the chosen target profile.");
  AssertContains(recommendationCpp, "freeze_snapshot_richer", "Recommendations must explain freeze snapshot richer recapture intent.");
  AssertContains(recommendationCpp, "crash_full", "Recommendations must explain crash_full recapture intent.");
}

void TestCrashLoggerFrameFixture_DirectFaultDllRuntimeContracts()
{
  const auto root = ProjectRoot();
  const auto log = ReadCrashLoggerFrameFixture("direct_fault_dll.log.txt");
  const auto analyzerCpp = ReadAllText(root / "dump_tool" / "src" / "Analyzer.cpp");
  const auto candidateCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderCandidates.cpp");

  AssertContains(log, "Precision.dll+0x000FDDC7",
                 "direct_fault_dll.log.txt: fixture must keep the direct DLL fault token.");
  AssertContains(log, "Precision.dll+00000003",
                 "direct_fault_dll.log.txt: fixture must keep the first actionable probable Precision frame.");
  AssertContains(analyzerCpp, "CanPromoteCrashLoggerFrameModule",
                 "direct_fault_dll.log.txt: direct-fault promotion must keep an explicit eligibility gate.");
  AssertContains(analyzerCpp, "crash_logger_direct_fault_module",
                 "direct_fault_dll.log.txt: analyzer must keep storing Crash Logger direct-fault modules.");
  AssertContains(candidateCpp, "CrashLogger direct-fault frame",
                 "direct_fault_dll.log.txt: candidate builder must keep surfacing direct-fault frame clues.");
}

void TestCrashLoggerFrameFixture_ExeVictimFirstProbableDllRuntimeContracts()
{
  const auto root = ProjectRoot();
  const auto log = ReadCrashLoggerFrameFixture("exe_victim_first_probable_dll.log.txt");
  const auto summaryCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderSummary.cpp");
  const auto recommendationCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderRecommendations.cpp");

  AssertContains(log, "SkyrimSE.exe+0x00ABCDEF",
                 "exe_victim_first_probable_dll.log.txt: fixture must keep the EXE victim direct-fault token.");
  AssertContains(log, "ExampleMod.dll+00000003",
                 "exe_victim_first_probable_dll.log.txt: fixture must keep the first actionable probable DLL frame.");
  AssertContains(summaryCpp, "Crash Logger frame first",
                 "exe_victim_first_probable_dll.log.txt: EXE victim summaries must keep frame-first wording.");
  AssertContains(summaryCpp, "stronger than an isolated object ref",
                 "exe_victim_first_probable_dll.log.txt: frame-backed DLL guidance must outrank isolated object refs.");
  AssertContains(recommendationCpp, "DLL guidance",
                 "exe_victim_first_probable_dll.log.txt: recommendations must preserve DLL guidance wording.");
}

void TestCrashLoggerFrameFixture_FrameObjectRefConflictRuntimeContracts()
{
  const auto root = ProjectRoot();
  const auto log = ReadCrashLoggerFrameFixture("frame_object_ref_conflict.log.txt");
  const auto consensusCpp = ReadJoinedText({
    root / "dump_tool" / "src" / "CandidateConsensus.cpp",
    root / "dump_tool" / "src" / "EvidenceBuilderCandidates.cpp",
  });
  const auto recommendationCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderRecommendations.cpp");

  AssertContains(log, "FrameBacked.dll+00000003",
                 "frame_object_ref_conflict.log.txt: fixture must keep the frame-backed DLL clue.");
  AssertContains(log, "\"OtherRef.esp\"",
                 "frame_object_ref_conflict.log.txt: fixture must keep the object-ref clue.");
  AssertContains(consensusCpp, "crash_logger_object_ref",
                 "frame_object_ref_conflict.log.txt: consensus must keep object-ref family support alongside frame clues.");
  AssertContains(consensusCpp, "conflicting",
                 "frame_object_ref_conflict.log.txt: consensus must keep conflicting candidate handling.");
  AssertContains(recommendationCpp, "object ref/stack evidence disagree",
                 "frame_object_ref_conflict.log.txt: recommendations must explain frame vs object-ref disagreement.");
}

}  // namespace

int main()
{
  TestSignatureDatabaseRuntime();
  TestSignatureCallstackContainsRuntime();
  TestSignatureDatabaseToleratesMalformedEntries();
  TestAddressResolverRuntime();
  TestAddressResolverToleratesMalformedEntries();
  TestCrashHistoryRuntime();
  TestCrashHistoryBucketCorrelation();
  TestCrashHistoryBucketCandidateStats();
  TestCaptureQualitySourceContracts();
  TestCrashLoggerFrameConsensusContracts();
  TestFreezeAnalysisSourceContracts();
  TestFirstChanceCtdCandidateSourceContracts();
  TestRecaptureEvaluationConsumptionSourceContracts();
  TestCrashLoggerFrameFixture_DirectFaultDllRuntimeContracts();
  TestCrashLoggerFrameFixture_ExeVictimFirstProbableDllRuntimeContracts();
  TestCrashLoggerFrameFixture_FrameObjectRefConflictRuntimeContracts();
  return 0;
}
