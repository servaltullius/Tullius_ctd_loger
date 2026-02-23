#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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

}  // namespace

int main()
{
  TestSignatureDatabaseRuntime();
  TestSignatureCallstackContainsRuntime();
  TestSignatureDatabaseToleratesMalformedEntries();
  TestAddressResolverRuntime();
  TestCrashHistoryRuntime();
  TestCrashHistoryBucketCorrelation();
  return 0;
}
