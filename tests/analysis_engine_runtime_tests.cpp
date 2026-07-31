#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

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
  const bool loaded = db.LoadFromJson(jsonPath);
  assert(loaded);
  assert(db.Size() > 0);

  SignatureMatchInput input{};
  input.exc_code = 0xC0000005u;
  input.game_version = "1.5.97.0";
  input.fault_module = L"SkyrimSE.exe";
  input.fault_offset = 0xD6DDDAull;
  input.fault_module_is_system = false;
  input.callstack_modules = { L"SkyrimSE.exe!BSBatchRenderer::Draw+0x2F" };

  const auto matched = db.Match(input, /*useKorean=*/false);
  assert(matched.has_value());
  assert(matched->id == "D6DDDA_1597_AV");
  assert(matched->scope == "mechanism");

  input.game_version = "1.6.640.0";
  assert(!db.Match(input, /*useKorean=*/false).has_value());

  input.game_version = "1.5.97.0";
  input.fault_offset = 0xD6DDD9ull;
  assert(!db.Match(input, /*useKorean=*/false).has_value());
  input.fault_offset = 0xD6DDDBull;
  assert(!db.Match(input, /*useKorean=*/false).has_value());

  input.game_version.clear();
  input.fault_offset = 0xD6DDDAull;
  assert(!db.Match(input, /*useKorean=*/false).has_value());
}

void TestNullAccessViolationSignatureRuntime()
{
  SignatureDatabase db;
  const auto jsonPath = ProjectRoot() / "dump_tool" / "data" / "crash_signatures.json";
  const bool loaded = db.LoadFromJson(jsonPath);
  assert(loaded);

  SignatureMatchInput input{};
  input.exc_code = 0xC0000005u;
  input.fault_module = L"ExampleMod.dll";
  input.fault_offset = 0x1234ull;

  assert(!input.access_violation_address.has_value());
  assert(!db.Match(input, /*useKorean=*/false).has_value());

  input.access_violation_address = 0x20ull;
  const auto nearNull = db.Match(input, /*useKorean=*/false);
  assert(nearNull.has_value());
  assert(nearNull->id == "ACCESS_VIOLATION_NULL");

  input.access_violation_address = 0x140001234ull;
  assert(!db.Match(input, /*useKorean=*/false).has_value());
}

void TestLegacyNullAddressFieldFailsClosedRuntime()
{
  const auto tempPath = std::filesystem::temp_directory_path() / "skydiag_signature_legacy_null_test.json";
  {
    std::ofstream out(tempPath, std::ios::binary);
    out <<
      "{\n"
      "  \"version\": 1,\n"
      "  \"signatures\": [{\n"
      "    \"id\": \"LEGACY_NULL\",\n"
      "    \"match\": {\"exc_code\": \"0xC0000005\", \"exc_address_near_zero\": true},\n"
      "    \"diagnosis\": {\"cause_ko\": \"test\", \"cause_en\": \"test\", "
      "\"confidence\": \"low\", \"recommendations_ko\": [], \"recommendations_en\": []}\n"
      "  }]\n"
      "}\n";
  }

  SignatureDatabase db;
  const bool loaded = db.LoadFromJson(tempPath);
  assert(loaded);

  SignatureMatchInput input{};
  input.exc_code = 0xC0000005u;
  assert(!db.Match(input, /*useKorean=*/false).has_value());
  input.access_violation_address = 0x10ull;
  const auto matched = db.Match(input, /*useKorean=*/false);
  assert(matched.has_value());
  assert(matched->id == "LEGACY_NULL");

  std::error_code ec;
  std::filesystem::remove(tempPath, ec);
}

void TestSignatureDatabaseRejectsUnknownFutureSchemaRuntime()
{
  const auto tempPath = std::filesystem::temp_directory_path() / "skydiag_signature_future_schema_test.json";
  {
    std::ofstream out(tempPath, std::ios::binary);
    out <<
      "{\n"
      "  \"version\": 999,\n"
      "  \"signatures\": [{\n"
      "    \"id\": \"FUTURE_NARROW_RULE\",\n"
      "    \"match\": {\"future_constraint\": \"must-not-be-ignored\"},\n"
      "    \"diagnosis\": {\"cause_ko\": \"test\", \"cause_en\": \"test\", "
      "\"confidence\": \"high\", \"recommendations_ko\": [], \"recommendations_en\": []}\n"
      "  }]\n"
      "}\n";
  }

  SignatureDatabase db;
  const bool loaded = db.LoadFromJson(tempPath);
  assert(!loaded);

  std::error_code ec;
  std::filesystem::remove(tempPath, ec);
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
  const bool loaded = db.LoadFromJson(tempPath);
  assert(loaded);
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
      "      \"id\": \"OVERFLOW_HEX\",\n"
      "      \"match\": {\"exc_code\": \"0x100000000\"},\n"
      "      \"diagnosis\": {\n"
      "        \"cause_ko\": \"bad\",\n"
      "        \"cause_en\": \"bad\",\n"
      "        \"confidence\": \"high\",\n"
      "        \"recommendations_ko\": [],\n"
      "        \"recommendations_en\": []\n"
      "      }\n"
      "    },\n"
      "    {\n"
      "      \"id\": \"UNKNOWN_CONSTRAINT\",\n"
      "      \"match\": {\"future_constraint\": \"must-not-be-ignored\"},\n"
      "      \"diagnosis\": {\n"
      "        \"cause_ko\": \"bad\",\n"
      "        \"cause_en\": \"bad\",\n"
      "        \"confidence\": \"high\",\n"
      "        \"recommendations_ko\": [],\n"
      "        \"recommendations_en\": []\n"
      "      }\n"
      "    },\n"
      "    {\n"
      "      \"id\": \"EMPTY_MATCH\",\n"
      "      \"match\": {},\n"
      "      \"diagnosis\": {\n"
      "        \"cause_ko\": \"bad\",\n"
      "        \"cause_en\": \"bad\",\n"
      "        \"confidence\": \"high\",\n"
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
  const bool loaded = db.LoadFromJson(tempPath);
  assert(loaded);
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
  const bool loaded = resolver.LoadFromJson(jsonPath, "1.5.97.0");
  assert(loaded);
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
  const bool loaded = resolver.LoadFromJson(tempPath, "1.5.97.0");
  assert(loaded);
  assert(resolver.Size() == 1);

  const auto resolved = resolver.Resolve(0xD6DDDAull);
  assert(resolved.has_value());
  assert(resolved.value() == "BSBatchRenderer::Draw");

  std::error_code ec;
  std::filesystem::remove(tempPath, ec);
}

void TestStackwalkRuntimeCanUseBundledGameMsdia()
{
  const auto symbolsCpp = ProjectRoot() / "dump_tool" / "src" / "AnalyzerInternalsStackwalkSymbols.cpp";
  const auto text = ReadAllText(symbolsCpp);
  AssertContains(
    text,
    "Data\" / L\"SKSE\" / L\"Plugins\" / moduleName",
    "Stackwalk runtime must check the game's SKSE Plugins folder for bundled msdia140.dll.");
  AssertContains(
    text,
    "LoadLibraryW",
    "Stackwalk runtime must load a bundled msdia140.dll when only the game install provides it.");
}

void TestAddressResolverLoadStatusRuntime()
{
  const auto tempPath = std::filesystem::temp_directory_path() / "skydiag_address_resolver_status_runtime_test.json";
  {
    std::ofstream out(tempPath, std::ios::binary);
    out <<
      "{\n"
      "  \"game_versions\": {\n"
      "    \"1.5.97.0\": {\n"
      "      \"functions\": {\n"
      "        \"D6DDDA\": \"BSBatchRenderer::Draw\"\n"
      "      }\n"
      "    }\n"
      "  }\n"
      "}\n";
  }

  AddressResolver resolver;
  AddressResolver::LoadStatus status = AddressResolver::LoadStatus::kOk;

  const bool missingFileLoaded =
    resolver.LoadFromJson(tempPath.parent_path() / "missing.json", "1.5.97.0", &status);
  assert(!missingFileLoaded);
  assert(status == AddressResolver::LoadStatus::kFileOpenFailed);

  const bool unsupportedVersionLoaded = resolver.LoadFromJson(tempPath, "1.6.1170.0", &status);
  assert(!unsupportedVersionLoaded);
  assert(status == AddressResolver::LoadStatus::kMissingGameVersion);

  const bool supportedVersionLoaded = resolver.LoadFromJson(tempPath, "1.5.97.0", &status);
  assert(supportedVersionLoaded);
  assert(status == AddressResolver::LoadStatus::kOk);

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
  const bool saved = history.SaveToFile(historyPath);
  assert(saved);

  CrashHistory loaded;
  const bool historyLoaded = loaded.LoadFromFile(historyPath);
  assert(historyLoaded);
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

void TestCrashHistoryCandidateKeyVersionMigration()
{
  const auto historyPath =
    std::filesystem::temp_directory_path() / "skydiag_crash_history_candidate_key_v1.json";
  {
    std::ofstream out(historyPath, std::ios::binary);
    out <<
      "{\n"
      "  \"version\": 1,\n"
      "  \"entries\": [{\n"
      "    \"timestamp_utc\": \"2026-01-01T00:00:00Z\",\n"
      "    \"dump_file\": \"legacy.dmp\",\n"
      "    \"bucket_key\": \"bucket-key-version\",\n"
      "    \"top_suspect\": \"A-B.dll\",\n"
      "    \"candidate_keys\": [\"ab\"]\n"
      "  }]\n"
      "}\n";
  }

  CrashHistory history;
  const bool historyLoaded = history.LoadFromFile(historyPath);
  assert(historyLoaded);
  assert(
    history.GetBucketCandidateStats("bucket-key-version").empty() &&
    "Ambiguous v1 keys must not boost v2 candidates");

  CrashHistoryEntry dashed{};
  dashed.timestamp_utc = "2026-01-02T00:00:00Z";
  dashed.bucket_key = "bucket-key-version";
  dashed.candidate_keys = { "a-b" };
  history.AddEntry(std::move(dashed));

  CrashHistoryEntry compact{};
  compact.timestamp_utc = "2026-01-03T00:00:00Z";
  compact.bucket_key = "bucket-key-version";
  compact.candidate_keys = { "ab" };
  history.AddEntry(std::move(compact));

  const auto stats = history.GetBucketCandidateStats("bucket-key-version");
  assert(stats.size() == 2);
  assert(stats[0].count == 1u);
  assert(stats[1].count == 1u);
  assert(stats[0].candidate_key != stats[1].candidate_key);

  const bool saved = history.SaveToFile(historyPath);
  assert(saved);
  CrashHistory reloaded;
  const bool reloadedFromFile = reloaded.LoadFromFile(historyPath);
  assert(reloadedFromFile);
  const auto reloadedStats = reloaded.GetBucketCandidateStats("bucket-key-version");
  assert(reloadedStats.size() == 2);

  std::error_code ec;
  std::filesystem::remove(historyPath, ec);
}

void TestCrashHistorySameDumpIsIdempotent()
{
  CrashHistory history;

  CrashHistoryEntry first{};
  first.timestamp_utc = "2026-07-20T01:00:00Z";
  first.dump_file = "SkyrimDiag_Crash_20260720_100000_001.dmp";
  first.bucket_key = "bucket-old";
  first.top_suspect = "Old.dll";
  first.candidate_keys = { "old" };
  history.AddEntry(std::move(first));

  CrashHistoryEntry reanalyzed{};
  reanalyzed.timestamp_utc = "2026-07-20T02:00:00Z";
  reanalyzed.dump_file = "skyrimdiag_crash_20260720_100000_001.DMP";
  reanalyzed.bucket_key = "bucket-new";
  reanalyzed.top_suspect = "New.dll";
  reanalyzed.candidate_keys = { "new" };
  history.AddEntry(std::move(reanalyzed));

  assert(history.Size() == 1u);
  assert(history.GetBucketStats("bucket-old").count == 0u);
  const auto replacement = history.GetBucketStats("bucket-new");
  assert(replacement.count == 1u);
  assert(replacement.first_seen == "2026-07-20T01:00:00Z");
  const auto candidates = history.GetBucketCandidateStats("bucket-new");
  assert(candidates.size() == 1u);
  assert(candidates[0].candidate_key == "new");

  assert(history.RemoveEntriesForDumpFile("SKYRIMDIAG_CRASH_20260720_100000_001.DMP") == 1u);
  assert(history.Size() == 0u);

  const auto analyzerHistory = ReadAllText(ProjectRoot() / "dump_tool" / "src" / "Analyzer.History.cpp");
  AssertContains(
    analyzerHistory,
    "history.RemoveEntriesForDumpFile",
    "Current dump must be excluded before history_repeat evidence is calculated.");
}

void TestCrashHistorySameStemIdentityIsolation()
{
  CrashHistory history;

  CrashHistoryEntry first{};
  first.timestamp_utc = "2026-07-20T01:00:00Z";
  first.dump_file = "SameStem.dmp";
  first.dump_identity_key = "sha-a.0000000000001000.0000000000002000";
  first.bucket_key = "bucket-a";
  history.AddEntry(first);

  CrashHistoryEntry second{};
  second.timestamp_utc = "2026-07-20T02:00:00Z";
  second.dump_file = "samestem.DMP";
  second.dump_identity_key = "sha-b.0000000000001000.0000000000003000";
  second.bucket_key = "bucket-b";
  history.AddEntry(second);

  assert(history.Size() == 2u);
  assert(history.GetBucketStats("bucket-a").count == 1u);
  assert(history.GetBucketStats("bucket-b").count == 1u);

  CrashHistoryEntry reanalyzed = second;
  reanalyzed.timestamp_utc = "2026-07-20T03:00:00Z";
  reanalyzed.bucket_key = "bucket-b-new";
  history.AddEntry(std::move(reanalyzed));
  assert(history.Size() == 2u);
  assert(history.GetBucketStats("bucket-b").count == 0u);
  assert(history.GetBucketStats("bucket-b-new").count == 1u);

  assert(history.RemoveEntriesForDumpFile("SameStem.dmp", second.dump_identity_key) == 1u);
  assert(history.Size() == 1u);
  assert(history.GetBucketStats("bucket-a").count == 1u);
  assert(history.RemoveEntriesForDumpFile("SameStem.dmp", first.dump_identity_key) == 1u);
  assert(history.Size() == 0u);
}

#ifdef _WIN32
void TestCrashHistoryFailedReplacementPreservesExistingFile()
{
  const auto root = std::filesystem::temp_directory_path() /
    ("skydiag_history_atomic_" + std::to_string(GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  assert(!ec);

  const auto path = root / "crash_history.json";
  {
    std::ofstream out(path, std::ios::binary);
    out << "authoritative-old-history";
  }

  const HANDLE locked = CreateFileW(
    path.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  assert(locked != INVALID_HANDLE_VALUE);

  CrashHistory replacement;
  CrashHistoryEntry entry{};
  entry.timestamp_utc = "2026-07-20T04:00:00Z";
  entry.dump_file = "new.dmp";
  entry.dump_identity_key = "new-identity";
  replacement.AddEntry(std::move(entry));
  assert(!replacement.SaveToFile(path));
  CloseHandle(locked);

  assert(ReadAllText(path) == "authoritative-old-history");
  for (const auto& child : std::filesystem::directory_iterator(root)) {
    assert(child.path() == path && "Failed atomic replacement left a temporary file behind");
  }
  std::filesystem::remove_all(root, ec);
}
#endif

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
  AssertContains(analyzerHeader, "incident_capture_profile_process_thread_data", "AnalysisResult must expose process/thread-data capture quality.");
  AssertContains(analyzerHeader, "incident_capture_profile_full_memory_info", "AnalysisResult must expose full-memory-info capture quality.");
  AssertContains(analyzerHeader, "incident_capture_profile_module_headers", "AnalysisResult must expose module-header capture quality.");
  AssertContains(analyzerHeader, "incident_capture_profile_indirect_memory", "AnalysisResult must expose indirect-memory capture quality.");
  AssertContains(analyzerHeader, "incident_capture_profile_ignore_inaccessible_memory", "AnalysisResult must expose inaccessible-memory tolerance.");
  AssertContains(analyzerCpp, "capture_profile", "Analyzer must consume incident capture profile metadata.");
  AssertContains(analyzerCpp, "include_process_thread_data", "Analyzer must consume process/thread-data capture metadata.");
  AssertContains(analyzerCpp, "include_full_memory_info", "Analyzer must consume full-memory-info capture metadata.");
  AssertContains(analyzerCpp, "include_module_headers", "Analyzer must consume module-header capture metadata.");
  AssertContains(analyzerCpp, "include_indirect_memory", "Analyzer must consume indirect-memory capture metadata.");
  AssertContains(analyzerCpp, "ignore_inaccessible_memory", "Analyzer must consume inaccessible-memory tolerance metadata.");
  AssertContains(evidenceCpp, "Capture profile metadata", "Evidence must explain which capture profile produced the dump.");
  AssertContains(evidenceCpp, "process_thread_data=", "Capture evidence must expose richer capture capabilities.");
  AssertContains(evidenceCpp, "indirect_memory=", "Capture evidence must expose indirect-memory support.");
  AssertContains(evidenceCpp, "Symbol/runtime environment limited stackwalk quality", "Evidence must describe symbol/runtime degradation.");
  AssertContains(recommendationCpp, "richer crash recapture profile", "Recommendations must prefer richer recapture profiles before generic full-memory advice.");
  AssertContains(recommendationCpp, "indirect memory", "Recommendations must acknowledge richer indirect-memory capture when present.");
  AssertContains(recommendationCpp, "Fix dbghelp/msdia or symbol cache/path health first", "Recommendations must call out symbol/runtime remediation.");
}

void TestCrashLoggerSystemPathPromotionGuardSourceContracts()
{
  const auto analyzerCpp = ReadAllText(ProjectRoot() / "dump_tool" / "src" / "Analyzer.cpp");
  AssertContains(
    analyzerCpp,
    "IsCrashLoggerFrameModuleLoadedFromSystemPath",
    "Crash Logger promotion must check the loaded module path, not only its filename.");
  AssertContains(
    analyzerCpp,
    "IsLikelyWindowsSystemModulePath(loaded.path)",
    "Crash Logger promotion must reject modules loaded from a Windows system path.");
  AssertContains(
    analyzerCpp,
    "ApplyCrashLoggerCorroborationToSuspects(&out, allModules)",
    "Crash Logger suspect corroboration must receive loaded module paths.");
  AssertContains(
    analyzerCpp,
    "out->crash_logger_direct_fault_eligible =",
    "Direct-fault observations must keep a separate actionable eligibility flag.");
  AssertContains(
    analyzerCpp,
    "out->crash_logger_first_actionable_probable_eligible =",
    "Probable-frame observations must keep a separate actionable eligibility flag.");
  AssertContains(
    analyzerCpp,
    "out->crash_logger_probable_streak_eligible =",
    "Probable-streak observations must keep a separate actionable eligibility flag.");
  AssertNotContains(
    analyzerCpp,
    "out->crash_logger_direct_fault_module.clear()",
    "Ineligible Crash Logger observations must remain available as raw evidence.");
}

void TestAddressDbDiagnosticSourceContracts()
{
  const auto root = ProjectRoot();
  const auto analyzerCpp = ReadAllText(root / "dump_tool" / "src" / "Analyzer.cpp");
  const auto resolverHeader = ReadAllText(root / "dump_tool" / "src" / "AddressResolver.h");
  const auto resolverCpp = ReadAllText(root / "dump_tool" / "src" / "AddressResolver.cpp");

  AssertContains(resolverHeader, "enum class LoadStatus", "AddressResolver must expose a load-status enum for diagnostics.");
  AssertContains(resolverCpp, "LoadStatus::kMissingGameVersion", "AddressResolver must distinguish missing game-version entries.");
  AssertContains(analyzerCpp, "address_db/skyrimse_functions.json not found", "Analyzer must tell users when the address DB file is missing.");
  AssertContains(analyzerCpp, "has no entry for game_version", "Analyzer must tell users when the address DB lacks the current game version.");
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
  AssertContains(
    consensusSrc,
    "PairingAdjustedCrashLoggerWeight",
    "Ambiguous CrashLogger pairing must downgrade candidate signal weights.");

  const auto analyzerSrc = ReadAllText(root / "dump_tool" / "src" / "Analyzer.cpp");
  AssertContains(
    analyzerSrc,
    "if (out->crash_logger_pairing_ambiguous)",
    "Ambiguous CrashLogger pairing must not reorder independent stack suspects.");
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
  AssertContains(analyzerHeader, "support_quality", "Freeze analysis output must carry support_quality.");
  const auto freezeConsensusCpp = ReadAllText(root / "dump_tool" / "src" / "FreezeCandidateConsensus.cpp");
  AssertContains(freezeConsensusCpp, "snapshot_consensus_backed", "Freeze consensus must distinguish snapshot_consensus_backed support quality.");
  AssertContains(freezeConsensusCpp, "snapshot_backed", "Freeze consensus must distinguish snapshot_backed support quality.");
  AssertContains(freezeConsensusCpp, "snapshot_fallback", "Freeze consensus must distinguish snapshot_fallback support quality.");
  AssertContains(freezeConsensusCpp, "live_process", "Freeze consensus must distinguish live_process support quality.");
  AssertContains(freezeConsensusCpp, "cycle_consensus", "Freeze consensus must consume cycle_consensus metadata.");
  AssertContains(freezeConsensusCpp, "repeated_cycle_tids", "Freeze consensus must consume repeated cycle tids metadata.");
  AssertContains(freezeConsensusCpp, "consistent_loading_signal", "Freeze consensus must consume consistent loading signal metadata.");
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
  AssertContains(summaryCpp, "Repeated suspicious first-chance context also matched this candidate.",
                 "EXE/system victim frame-backed summaries must mention repeated first-chance context when it reinforces the DLL candidate.");
  AssertContains(recommendationCpp, "check the repeated first-chance path before broad EXE/system triage",
                 "Related frame-backed CTD recommendations must prioritize the repeated first-chance path before broad EXE/system triage.");
  AssertContains(summaryCpp, "Repeated crash bucket history also matched this candidate.",
                 "EXE/system victim frame-backed summaries must mention repeated crash bucket history when it reinforces the DLL candidate.");
  AssertContains(recommendationCpp, "compare repeated same-bucket crashes before broad EXE/system triage",
                 "Related frame-backed CTD recommendations must prioritize repeated same-bucket crash history before broad EXE/system triage.");
  AssertContains(summaryCpp, "Nearby resource provider activity also matched this candidate.",
                 "EXE/system victim frame-backed summaries must mention nearby resource provider activity when it reinforces the DLL candidate.");
  AssertContains(recommendationCpp, "compare nearby resource providers before broad EXE/system triage",
                 "Related frame-backed CTD recommendations must prioritize nearby resource providers before broad EXE/system triage.");
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
  const auto summaryCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderSummary.cpp");
  const auto builderCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilder.cpp");
  const auto recommendationCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderRecommendations.cpp");

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
  AssertContains(summaryCpp, "no second independent signal agrees yet",
                 "direct_fault_dll.log.txt: weak non-system DLL summaries must preserve isolated-frame caution.");
  AssertContains(summaryCpp, "fault-location evidence only",
                 "direct_fault_dll.log.txt: fallback non-system DLL summaries must avoid overclaiming from fault location alone.");
  AssertContains(builderCpp, "fault-location clue only; may be victim location",
                 "direct_fault_dll.log.txt: weak non-system DLL suspects must be downgraded before serialization.");
  AssertContains(recommendationCpp, "before treating it as the root cause",
                 "direct_fault_dll.log.txt: weak non-system DLL guidance must stay cautious before blaming the DLL.");
  AssertContains(recommendationCpp, "Compare nearby Crash Logger probable DLLs too",
                 "direct_fault_dll.log.txt: weak non-system DLL guidance should compare neighboring probable DLLs.");
  AssertContains(recommendationCpp, "when reporting to the mod author",
                 "direct_fault_dll.log.txt: corroborated DLL suspects may still escalate to mod-author reporting.");
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

void TestCrashLoggerFrameFixture_HookFrameworkVictimFirstProbableDllRuntimeContracts()
{
  const auto root = ProjectRoot();
  const auto log = ReadCrashLoggerFrameFixture("hook_framework_victim_first_probable_dll.log.txt");
  const auto summaryCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderSummary.cpp");
  const auto recommendationCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderRecommendations.cpp");

  AssertContains(log, "CrashLoggerSSE.dll+0x00001234",
                 "hook_framework_victim_first_probable_dll.log.txt: fixture must keep the hook-framework fault token.");
  AssertContains(log, "RealCause.dll+00000003",
                 "hook_framework_victim_first_probable_dll.log.txt: fixture must keep the promoted non-hook DLL frame.");
  AssertContains(summaryCpp, "known hook framework",
                 "hook_framework_victim_first_probable_dll.log.txt: summary must keep hook-framework victim wording.");
  AssertContains(summaryCpp, "victim location",
                 "hook_framework_victim_first_probable_dll.log.txt: summary must keep victim-location guidance.");
  AssertContains(recommendationCpp, "known hook framework DLL",
                 "hook_framework_victim_first_probable_dll.log.txt: recommendations must keep hook-framework fallback guidance.");
}

void TestCrashLoggerFrameFixture_CppExceptionModuleSupportRuntimeContracts()
{
  const auto root = ProjectRoot();
  const auto log = ReadCrashLoggerFrameFixture("cpp_exception_module_support.log.txt");
  const auto analyzerCpp = ReadAllText(root / "dump_tool" / "src" / "Analyzer.cpp");
  const auto captureCpp = ReadAllText(root / "dump_tool" / "src" / "Analyzer.CaptureInputs.cpp");
  const auto evidenceCrashCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderEvidence.Crash.cpp");

  AssertContains(log, "C++ EXCEPTION:",
                 "cpp_exception_module_support.log.txt: fixture must keep the C++ exception block.");
  AssertContains(log, "Module: CppOwner.dll",
                 "cpp_exception_module_support.log.txt: fixture must keep the exception module line.");
  AssertContains(captureCpp, "crash_logger_cpp_exception_module",
                 "cpp_exception_module_support.log.txt: analyzer capture must keep storing the C++ exception module.");
  AssertContains(analyzerCpp, "Crash Logger C++ exception module support",
                 "cpp_exception_module_support.log.txt: analyzer promotion must keep additive C++ exception support.");
  AssertContains(evidenceCrashCpp, "Crash Logger: C++ exception details",
                 "cpp_exception_module_support.log.txt: evidence output must keep exposing C++ exception details.");
}

void TestCrashLoggerFrameFixture_SystemDllPathqualifiedFirstProbableDllRuntimeContracts()
{
  const auto root = ProjectRoot();
  const auto log = ReadCrashLoggerFrameFixture("system_dll_pathqualified_first_probable_dll.log.txt");
  const auto summaryCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderSummary.cpp");
  const auto recommendationCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderRecommendations.cpp");

  AssertContains(log, "Faulting module path: C:\\Windows\\System32\\KERNELBASE.dll",
                 "system_dll_pathqualified_first_probable_dll.log.txt: fixture must keep the path-qualified system victim token.");
  AssertContains(log, "D:\\Mods\\SystemVictim.dll+00000003",
                 "system_dll_pathqualified_first_probable_dll.log.txt: fixture must keep the promoted non-system DLL frame.");
  AssertContains(summaryCpp, "Windows system DLL",
                 "system_dll_pathqualified_first_probable_dll.log.txt: summary must keep system-DLL victim wording.");
  AssertContains(summaryCpp, "stronger than an isolated object ref",
                 "system_dll_pathqualified_first_probable_dll.log.txt: system-DLL victim summaries must prefer frame-backed DLL guidance.");
  AssertContains(recommendationCpp, "DLL guidance",
                 "system_dll_pathqualified_first_probable_dll.log.txt: recommendations must keep DLL guidance wording for system victims.");
}

void TestStandaloneStackwalkRuntimeContracts()
{
  const auto root = ProjectRoot();
  const auto consensusCpp = ReadAllText(root / "dump_tool" / "src" / "CandidateConsensus.cpp");
  const auto summaryCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderSummary.cpp");
  const auto recommendationCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderRecommendations.cpp");
  const auto winUiCandidates = ReadAllText(root / "dump_tool_winui" / "MainWindowViewModel.Candidates.cs");
  const auto winUiRecommendations = ReadAllText(root / "dump_tool_winui" / "MainWindowViewModel.Recommendations.cs");

  AssertContains(consensusCpp, "strongStackOnly",
                 "Standalone stackwalk CTD handling must distinguish strong stackwalk-only candidates from weak stack-scan-only clues.");
  AssertContains(summaryCpp, "Tullius callstack first points to DLL candidate",
                 "Standalone stackwalk-backed summaries must expose Tullius callstack-first wording.");
  AssertContains(summaryCpp, "stronger than stack scan only",
                 "Standalone stackwalk-backed summaries must explain why callstack-backed clues outrank stack scan only.");
  AssertContains(recommendationCpp, "Tullius callstack first points to DLL candidate",
                 "Standalone stackwalk-backed recommendations must expose Tullius callstack-first wording.");
  AssertContains(winUiCandidates, "Tullius callstack first",
                 "WinUI evidence agreement must expose standalone Tullius callstack-first wording.");
  AssertContains(winUiRecommendations, "Tullius callstack: check",
                 "WinUI next-action summary must expose standalone Tullius callstack guidance.");
}

void TestResourceProviderScoreOnlyRuntimeContracts()
{
  const auto root = ProjectRoot();
  const auto candidateCpp = ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderCandidates.cpp");
  const auto consensusCpp = ReadAllText(root / "dump_tool" / "src" / "CandidateConsensus.cpp");

  AssertContains(candidateCpp, "signal.weight = (row.hitCount >= 2u && row.bestDistanceMs <= 150.0) ? 5u",
                 "resource-provider tuning must use hitCount plus distance for the top score-only bump.");
  AssertContains(candidateCpp, "((row.bestDistanceMs <= 300.0) || row.hitCount >= 2u) ? 4u : 3u",
                 "resource-provider tuning must keep lower-tier boosts score-only without changing status rules.");
  AssertContains(consensusCpp, "strongFrameOnly",
                 "resource-provider score-only tuning must not require new status-promotion rules.");
}

}  // namespace

int main()
{
  TestSignatureDatabaseRuntime();
  TestNullAccessViolationSignatureRuntime();
  TestLegacyNullAddressFieldFailsClosedRuntime();
  TestSignatureDatabaseRejectsUnknownFutureSchemaRuntime();
  TestSignatureCallstackContainsRuntime();
  TestSignatureDatabaseToleratesMalformedEntries();
  TestAddressResolverRuntime();
  TestAddressResolverToleratesMalformedEntries();
  TestStackwalkRuntimeCanUseBundledGameMsdia();
  TestAddressResolverLoadStatusRuntime();
  TestCrashHistoryRuntime();
  TestCrashHistoryBucketCorrelation();
  TestCrashHistoryBucketCandidateStats();
  TestCrashHistoryCandidateKeyVersionMigration();
  TestCrashHistorySameDumpIsIdempotent();
  TestCrashHistorySameStemIdentityIsolation();
#ifdef _WIN32
  TestCrashHistoryFailedReplacementPreservesExistingFile();
#endif
  TestCaptureQualitySourceContracts();
  TestCrashLoggerSystemPathPromotionGuardSourceContracts();
  TestAddressDbDiagnosticSourceContracts();
  TestCrashLoggerFrameConsensusContracts();
  TestFreezeAnalysisSourceContracts();
  TestFirstChanceCtdCandidateSourceContracts();
  TestRecaptureEvaluationConsumptionSourceContracts();
  TestCrashLoggerFrameFixture_DirectFaultDllRuntimeContracts();
  TestCrashLoggerFrameFixture_ExeVictimFirstProbableDllRuntimeContracts();
  TestCrashLoggerFrameFixture_FrameObjectRefConflictRuntimeContracts();
  TestCrashLoggerFrameFixture_HookFrameworkVictimFirstProbableDllRuntimeContracts();
  TestCrashLoggerFrameFixture_CppExceptionModuleSupportRuntimeContracts();
  TestCrashLoggerFrameFixture_SystemDllPathqualifiedFirstProbableDllRuntimeContracts();
  TestStandaloneStackwalkRuntimeContracts();
  TestResourceProviderScoreOnlyRuntimeContracts();
  return 0;
}
