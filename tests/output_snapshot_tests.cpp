// output_snapshot_tests.cpp — E2E JSON schema contract & OutputWriter guard tests
//
// Validates:
//  1. Golden JSON fixture conforms to SkyrimDiagSummary v2 schema
//  2. OutputWriter.cpp source contains all required JSON field writes
//  3. OutputWriter produces both summary JSON and report text

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "SourceGuardTestUtils.h"
#include "SummarySchemaV2Checks.h"

namespace {
using skydiag::tests::source_guard::ProjectRoot;
using skydiag::tests::source_guard::ReadAllText;
using skydiag::tests::source_guard::ReadProjectConcatenatedText;
using skydiag::tests::source_guard::ReadProjectText;
using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::summary_schema::ValidateSummarySchemaV2;

nlohmann::json LoadGoldenJson()
{
  const auto root = ProjectRoot();
  const auto goldenPath = root / "tests" / "data" / "golden_summary_v2.json";
  const std::string text = ReadAllText(goldenPath);
  return nlohmann::json::parse(text);
}

std::string ReadCrashLoggerFrameFixture(const char* filename)
{
  return ReadAllText(ProjectRoot() / "tests" / "data" / "crashlogger_frame_cases" / filename);
}

void TestGoldenJsonSchemaV2(const nlohmann::json& j)
{
  ValidateSummarySchemaV2(j);
  assert(!j["suspects"].empty());

  std::cout << "  [PASS] Golden JSON v2 schema validation\n";
}

// ── Value validation: check golden fixture data integrity ──

void TestGoldenJsonValues(const nlohmann::json& j)
{
  // Verify specific values to catch accidental golden file corruption
  assert(j["exception"]["code"].get<std::uint32_t>() == 0xC0000005u);
  assert(j["analysis"]["is_crash_like"].get<bool>() == true);
  assert(j["suspects"].size() == 2);
  assert(j["suspects"][0]["confidence"].get<std::string>() == "High");
  assert(j["suspects"][0]["module_filename"].get<std::string>() == "hdtSMP64.dll");
  assert(j["callstack"]["frames"].size() == 4);
  assert(j["evidence"].size() == 1);
  assert(j["recommendations"].size() == 2);
  assert(j["freeze_analysis"]["state_id"].is_string());
  assert(j["freeze_analysis"]["first_chance_context"].is_object());

  // CrashLogger object_refs with FormID
  assert(j["crash_logger"]["object_refs"].size() == 1);
  const auto& ref = j["crash_logger"]["object_refs"][0];
  assert(ref["esp_name"].get<std::string>() == "AE_StellarBlade_Doro.esp");
  assert(ref["form_id"].get<std::string>() == "0xFEAD081B");
  assert(ref["object_name"].get<std::string>() == "도로롱");

  std::cout << "  [PASS] Golden JSON value integrity\n";
}

// ── Source guard: OutputWriter.cpp must emit all schema fields ──

void TestOutputWriterEmitsAllFields()
{
  const std::string src = ReadProjectText("dump_tool/src/OutputWriter.cpp");

  // All top-level JSON keys that BuildSummaryJson must produce
  const std::vector<std::string> requiredKeys = {
    "\"schema\"",
    "\"dump_path\"",
    "\"pid\"",
    "\"state_flags\"",
    "\"summary_sentence\"",
    "\"crash_bucket_key\"",
    "\"crash_bucket_version\"",
    "\"analysis\"",
    "\"privacy\"",
    "\"triage\"",
    "\"exception\"",
    "\"game_version\"",
    "\"crash_logger\"",
    "\"signature_match\"",
    "\"graphics_environment\"",
    "\"suspects\"",
    "\"callstack\"",
    "\"symbolization\"",
    "\"resources\"",
    "\"actionable_candidates\"",
    "\"freeze_analysis\"",
    "\"first_chance_context\"",
    "\"evidence\"",
    "\"recommendations\"",
  };

  for (const auto& key : requiredKeys) {
    if (src.find(key) == std::string::npos) {
      std::cerr << "FAIL: OutputWriter.cpp missing JSON key " << key << "\n";
      assert(false);
    }
  }

  // Nested fields that must be emitted
  const std::vector<std::string> nestedKeys = {
    "\"is_crash_like\"",
    "\"is_hang_like\"",
    "\"is_snapshot_like\"",
    "\"is_manual_capture\"",
    "\"path_redaction_applied\"",
    "\"code\"",
    "\"thread_id\"",
    "\"address\"",
    "\"fault_module_offset\"",
    "\"module_plus_offset\"",
    "\"fault_module_unknown\"",
    "\"module_path\"",
    "\"inferred_mod_name\"",
    "\"log_path\"",
    "\"version\"",
    "\"top_modules\"",
    "\"object_refs\"",
    "\"esp_name\"",
    "\"form_id\"",
    "\"status_id\"",
    "\"display_name\"",
    "\"primary_identifier\"",
    "\"secondary_label\"",
    "\"plugin_name\"",
    "\"mod_name\"",
    "\"module_filename\"",
    "\"family_count\"",
    "\"supporting_families\"",
    "\"conflicting_families\"",
    "\"state_id\"",
    "\"support_quality\"",
    "\"primary_reasons\"",
    "\"related_candidates\"",
    "\"blackbox_context\"",
    "\"first_chance_context\"",
    "\"wct_consensus\"",
    "\"loading_window\"",
    "\"recent_module_loads\"",
    "\"recent_module_unloads\"",
    "\"recent_thread_creates\"",
    "\"recent_thread_exits\"",
    "\"module_churn_score\"",
    "\"thread_churn_score\"",
    "\"recent_non_system_modules\"",
    "\"has_context\"",
    "\"recent_count\"",
    "\"unique_signature_count\"",
    "\"loading_window_count\"",
    "\"repeated_signature_count\"",
    "\"enb_detected\"",
    "\"reshade_detected\"",
    "\"dxvk_detected\"",
    "\"injection_modules\"",
    "\"total_frames\"",
    "\"symbolized_frames\"",
    "\"source_line_frames\"",
    "\"dbghelp_path\"",
    "\"dbghelp_version\"",
    "\"msdia_available\"",
    "\"runtime_degraded\"",
    "\"recapture_evaluation\"",
    "\"triggered\"",
    "\"target_profile\"",
    "\"reasons\"",
    "\"escalation_level\"",
    "\"include_code_segments\"",
    "\"include_process_thread_data\"",
    "\"include_full_memory_info\"",
    "\"include_module_headers\"",
    "\"include_indirect_memory\"",
    "\"ignore_inaccessible_memory\"",
  };

  for (const auto& key : nestedKeys) {
    if (src.find(key) == std::string::npos) {
      std::cerr << "FAIL: OutputWriter.cpp missing nested key " << key << "\n";
      assert(false);
    }
  }

  std::cout << "  [PASS] OutputWriter emits all required JSON fields\n";
}

// ── Source guard: report text must have sections ──

void TestOutputWriterReportTextSections()
{
  const std::string src = ReadProjectText("dump_tool/src/OutputWriter.cpp");

  const std::vector<std::string> requiredSections = {
    "SkyrimDiag Report",
    "ExceptionCode:",
    "ExceptionAddress:",
    "ThreadId:",
    "Module+Offset:",
    "CaptureProfileBaseMode:",
    "CaptureProfileCodeSegments:",
    "CaptureProfileFullMemory:",
    "CaptureProfileProcessThreadData:",
    "CaptureProfileFullMemoryInfo:",
    "CaptureProfileModuleHeaders:",
    "CaptureProfileIndirectMemory:",
    "CaptureProfileIgnoreInaccessibleMemory:",
    "RecaptureTriggered:",
    "RecaptureTargetProfile:",
    "RecaptureReasons:",
    "RecaptureEscalationLevel:",
    "Evidence:",
    "Recommendations",
    "Callstack",
    "Suspects",
    "FreezeAnalysis:",
    "HasBlackbox:",
    "HasWCT:",
    "blackbox",
    "module churn",
    "first_chance_context",
    "wct capture_passes=",
    "cycle_consensus=",
    "consistent_loading_signal=",
    "repeated suspicious first-chance",
  };

  for (const auto& section : requiredSections) {
    if (src.find(section) == std::string::npos) {
      std::cerr << "FAIL: OutputWriter.cpp report text missing section \"" << section << "\"\n";
      assert(false);
    }
  }

  std::cout << "  [PASS] OutputWriter report text has all required sections\n";
}

void TestFirstChanceCandidateExplanationSourceGuards()
{
  const std::string evidenceSrc = ReadProjectText("dump_tool/src/EvidenceBuilderEvidence.cpp");
  const std::string recommendationSrc = ReadProjectText("dump_tool/src/EvidenceBuilderRecommendations.cpp");
  const std::string summarySrc = ReadProjectText("dump_tool/src/EvidenceBuilderSummary.cpp");

  assert(evidenceSrc.find("first_chance_context") != std::string::npos);
  assert(evidenceSrc.find("repeated suspicious first-chance") != std::string::npos);
  assert(recommendationSrc.find("first-chance") != std::string::npos);
  assert(summarySrc.find("first_chance_context") != std::string::npos);
  assert(summarySrc.find("Repeated suspicious first-chance context also matched this candidate.") != std::string::npos);
  assert(recommendationSrc.find("check the repeated first-chance path before broad EXE/system triage") != std::string::npos);
  assert(summarySrc.find("Repeated crash bucket history also matched this candidate.") != std::string::npos);
  assert(recommendationSrc.find("compare repeated same-bucket crashes before broad EXE/system triage") != std::string::npos);
  assert(summarySrc.find("Nearby resource provider activity also matched this candidate.") != std::string::npos);
  assert(recommendationSrc.find("compare nearby resource providers before broad EXE/system triage") != std::string::npos);

  std::cout << "  [PASS] CTD first-chance candidate explanation guards\n";
}

void TestStandaloneCallstackExplanationSourceGuards()
{
  const std::string recommendationSrc = ReadProjectText("dump_tool/src/EvidenceBuilderRecommendations.cpp");
  const std::string summarySrc = ReadProjectText("dump_tool/src/EvidenceBuilderSummary.cpp");

  AssertContains(
    summarySrc,
    "Tullius callstack first points to DLL candidate",
    "Standalone stackwalk-backed CTD summaries must expose Tullius callstack-first wording.");
  AssertContains(
    summarySrc,
    "This is stronger than stack scan only.",
    "Standalone stackwalk-backed CTD summaries must explain why callstack-backed clues outrank stack scan only.");
  AssertContains(
    recommendationSrc,
    "Tullius callstack first points to DLL candidate",
    "Standalone stackwalk-backed recommendations must expose Tullius callstack-first wording.");
  AssertContains(
    recommendationSrc,
    "Check it before broad EXE/system triage.",
    "Standalone stackwalk-backed recommendations must direct users before generic EXE/system triage.");

  std::cout << "  [PASS] Standalone callstack explanation guards\n";
}

void TestRecaptureEvaluationConsumptionSourceGuards()
{
  const std::string outputSrc = ReadProjectText("dump_tool/src/OutputWriter.cpp");
  const std::string evidenceSrc = ReadProjectText("dump_tool/src/EvidenceBuilderEvidence.cpp");
  const std::string recommendationSrc = ReadProjectText("dump_tool/src/EvidenceBuilderRecommendations.cpp");

  assert(outputSrc.find("RecaptureReasons:") != std::string::npos);
  assert(outputSrc.find("RecaptureEscalationLevel:") != std::string::npos);
  assert(outputSrc.find("\"recapture_evaluation\"") != std::string::npos);
  assert(evidenceSrc.find("Capture recapture context") != std::string::npos);
  assert(evidenceSrc.find("incident_recapture_target_profile") != std::string::npos);
  assert(recommendationSrc.find("[Recapture]") != std::string::npos);
  assert(recommendationSrc.find("freeze_snapshot_richer") != std::string::npos);

  std::cout << "  [PASS] Recapture evaluation consumption guards\n";
}

void TestCrashLoggerFrameCandidateFamilySourceGuards()
{
  const std::string candidateSrc = ReadProjectConcatenatedText({
    "dump_tool/src/CandidateConsensus.cpp",
    "dump_tool/src/EvidenceBuilderCandidates.cpp",
  });
  const std::string summarySrc = ReadProjectText("dump_tool/src/OutputWriter.Summary.cpp");

  AssertContains(candidateSrc, "crash_logger_frame", "Candidate consensus must recognize the crash_logger_frame family.");
  AssertContains(candidateSrc, "cross_validated", "Candidate consensus must still emit cross_validated actionable candidates.");
  AssertContains(candidateSrc, "related", "Candidate consensus must still emit related actionable candidates.");
  AssertContains(candidateSrc, "conflicting", "Candidate consensus must still emit conflicting actionable candidates.");
  AssertContains(summarySrc, "supporting_families", "Output summary must serialize supporting families for actionable candidates.");

  std::cout << "  [PASS] Crash Logger frame candidate family guards\n";
}

void TestCrashLoggerFrameFirstSummaryPrioritySourceGuards()
{
  const std::string summarySrc = ReadProjectConcatenatedText({
    "dump_tool/src/EvidenceBuilder.cpp",
    "dump_tool/src/EvidenceBuilderSummary.cpp",
    "dump_tool/src/EvidenceBuilderRecommendations.cpp",
    "dump_tool/src/OutputWriter.Summary.cpp",
  });

  AssertContains(
    summarySrc,
    "crash_logger_frame",
    "EXE/system victim summary must prefer Crash Logger frame-backed DLL guidance over isolated object refs.");
  AssertContains(
    summarySrc,
    "Crash Logger frame first",
    "Summary and recommendation wording must stay aligned with the frame-first CTD engine path.");
  AssertContains(
    summarySrc,
    "no second independent signal agrees yet",
    "Non-system DLL summaries must distinguish isolated frame clues from corroborated DLL blame.");
  AssertContains(
    summarySrc,
    "victim location",
    "Weak non-system DLL summaries must preserve victim-location caution.");
  AssertContains(
    summarySrc,
    "direct DLL fault",
    "Direct DLL fault cases should surface DLL guidance before broader EXE/system triage.");
  AssertContains(
    summarySrc,
    "paired CrashLogger log's",
    "Hook-framework victim summaries must prefer the paired CrashLogger frame-backed DLL candidate.");
  AssertContains(
    summarySrc,
    "fault-location evidence only",
    "Fallback non-system DLL summaries must avoid overclaiming when only the fault location is known.");
  AssertContains(
    summarySrc,
    "fault-location clue only; may be victim location",
    "Weak non-system DLL suspects must be downgraded before report serialization.");

  std::cout << "  [PASS] Crash Logger frame-first summary priority guards\n";
}

void TestCrashLoggerFrameFixtureSummaryPriorityGuards()
{
  const std::string directFaultFixture = ReadCrashLoggerFrameFixture("direct_fault_dll.log.txt");
  const std::string exeVictimFixture = ReadCrashLoggerFrameFixture("exe_victim_first_probable_dll.log.txt");
  const std::string conflictFixture = ReadCrashLoggerFrameFixture("frame_object_ref_conflict.log.txt");
  const std::string summarySrc = ReadProjectText("dump_tool/src/EvidenceBuilderSummary.cpp");
  const std::string recommendationSrc = ReadProjectText("dump_tool/src/EvidenceBuilderRecommendations.cpp");

  AssertContains(
    directFaultFixture,
    "Precision.dll+0x000FDDC7",
    "direct_fault_dll.log.txt must preserve the direct DLL fault token used by frame-first CTD summaries.");
  AssertContains(
    exeVictimFixture,
    "ExampleMod.dll+00000003",
    "exe_victim_first_probable_dll.log.txt must preserve the first actionable probable DLL frame.");
  AssertContains(
    conflictFixture,
    "\"OtherRef.esp\"",
    "frame_object_ref_conflict.log.txt must preserve the conflicting object-ref clue.");
  AssertContains(
    summarySrc,
    "stronger than an isolated object ref",
    "Fixture-backed EXE/system victim summaries must keep preferring frame-backed DLL guidance over isolated object refs.");
  AssertContains(
    recommendationSrc,
    "object ref/stack evidence disagree",
    "Fixture-backed conflict guidance must keep explaining frame vs object-ref disagreement.");

  std::cout << "  [PASS] Crash Logger frame fixture summary priority guards\n";
}

void TestCrashLoggerExpandedFixtureSummaryPriorityGuards()
{
  const std::string hookVictimFixture = ReadCrashLoggerFrameFixture("hook_framework_victim_first_probable_dll.log.txt");
  const std::string cppFixture = ReadCrashLoggerFrameFixture("cpp_exception_module_support.log.txt");
  const std::string systemVictimFixture = ReadCrashLoggerFrameFixture("system_dll_pathqualified_first_probable_dll.log.txt");
  const std::string summarySrc = ReadProjectText("dump_tool/src/EvidenceBuilderSummary.cpp");
  const std::string recommendationSrc = ReadProjectText("dump_tool/src/EvidenceBuilderRecommendations.cpp");
  const std::string summaryJsonSrc = ReadProjectText("dump_tool/src/OutputWriter.Summary.cpp");
  const std::string reportSrc = ReadProjectText("dump_tool/src/OutputWriter.Report.cpp");
  const std::string evidenceCrashSrc = ReadProjectText("dump_tool/src/EvidenceBuilderEvidence.Crash.cpp");

  AssertContains(
    hookVictimFixture,
    "CrashLoggerSSE.dll+0x00001234",
    "hook_framework_victim_first_probable_dll.log.txt must preserve the hook-framework fault token.");
  AssertContains(
    cppFixture,
    "C++ EXCEPTION:",
    "cpp_exception_module_support.log.txt must preserve the C++ exception block.");
  AssertContains(
    systemVictimFixture,
    "Faulting module path: C:\\Windows\\System32\\KERNELBASE.dll",
    "system_dll_pathqualified_first_probable_dll.log.txt must preserve the path-qualified system victim token.");
  AssertContains(
    summarySrc,
    "known hook framework",
    "Expanded fixtures must keep hook-framework victim wording in CTD summaries.");
  AssertContains(
    summarySrc,
    "Windows system DLL",
    "Expanded fixtures must keep Windows system DLL victim wording in CTD summaries.");
  AssertContains(
    recommendationSrc,
    "known hook framework DLL",
    "Expanded fixtures must keep hook-framework victim recommendations.");
  AssertContains(
    recommendationSrc,
    "before treating it as the root cause",
    "Weak non-system DLL recommendations must stay cautious before blaming the DLL.");
  AssertContains(
    recommendationSrc,
    "Compare nearby Crash Logger probable DLLs too",
    "Weak non-system DLL guidance should point users at neighboring probable DLLs, not just the fault-location DLL.");
  AssertContains(
    recommendationSrc,
    "when reporting to the mod author",
    "Cross-validated DLL candidates may still escalate to mod-author reporting.");
  AssertContains(
    summaryJsonSrc,
    "\"cpp_exception\"",
    "Expanded fixtures must keep serializing Crash Logger C++ exception details into summary JSON.");
  AssertContains(
    reportSrc,
    "CrashLoggerCppExceptionModule:",
    "Expanded fixtures must keep printing Crash Logger C++ exception module details in report text.");
  AssertContains(
    evidenceCrashSrc,
    "Crash Logger: C++ exception details",
    "Expanded fixtures must keep exposing Crash Logger C++ exception evidence details.");
  AssertContains(
    evidenceCrashSrc,
    "Top stack DLL clue (fault-location)",
    "Weak non-system DLL evidence should be labeled as a clue rather than a top suspect.");

  std::cout << "  [PASS] Expanded Crash Logger fixture summary priority guards\n";
}

void TestCrashLoggerExpandedFixtureReportTextGuards()
{
  const std::string reportSrc = ReadProjectText("dump_tool/src/OutputWriter.Report.cpp");

  AssertContains(
    reportSrc,
    "CrashLoggerDirectFaultModule:",
    "Report text must keep printing direct DLL fault modules for frame-first CTD cases.");
  AssertContains(
    reportSrc,
    "CrashLoggerFirstActionableProbableModule:",
    "Report text must keep printing first actionable probable DLL modules for EXE/system victim cases.");
  AssertContains(
    reportSrc,
    "CrashLoggerProbableStreakModule:",
    "Report text must keep printing probable frame streak modules for repeated DLL clues.");
  AssertContains(
    reportSrc,
    "CrashLoggerFrameSignalStrength:",
    "Report text must keep printing Crash Logger frame signal strength.");
  AssertContains(
    reportSrc,
    "CrashLoggerCppExceptionModule:",
    "Report text must keep printing Crash Logger C++ exception module details.");
  AssertContains(
    reportSrc,
    "CrashLoggerReadingPath:",
    "Report text must expose a CrashLogger-first reading path line.");
  AssertContains(
    reportSrc,
    "NextAction:",
    "Report text must expose a next-action line for checklist consumers.");

  std::cout << "  [PASS] Expanded Crash Logger fixture report text guards\n";
}

// ── Source guard: WriteOutputs writes both JSON and text files ──

void TestOutputWriterWritesBothFiles()
{
  const std::string src = ReadProjectText("dump_tool/src/OutputWriter.cpp");

  assert(src.find("_SkyrimDiagSummary.json") != std::string::npos);
  assert(src.find("_SkyrimDiagReport.txt") != std::string::npos);
  assert(src.find("BuildSummaryJson") != std::string::npos);
  assert(src.find("BuildReportText") != std::string::npos);

  std::cout << "  [PASS] OutputWriter produces summary JSON + report text\n";
}

}  // namespace

int main()
{
  std::cout << "output_snapshot_tests:\n";
  const auto golden = LoadGoldenJson();
  TestGoldenJsonSchemaV2(golden);
  TestGoldenJsonValues(golden);
  TestOutputWriterEmitsAllFields();
  TestOutputWriterReportTextSections();
  TestFirstChanceCandidateExplanationSourceGuards();
  TestStandaloneCallstackExplanationSourceGuards();
  TestRecaptureEvaluationConsumptionSourceGuards();
  TestCrashLoggerFrameCandidateFamilySourceGuards();
  TestCrashLoggerFrameFirstSummaryPrioritySourceGuards();
  TestCrashLoggerFrameFixtureSummaryPriorityGuards();
  TestCrashLoggerExpandedFixtureSummaryPriorityGuards();
  TestCrashLoggerExpandedFixtureReportTextGuards();
  TestOutputWriterWritesBothFiles();
  std::cout << "All output snapshot tests passed.\n";
  return 0;
}
