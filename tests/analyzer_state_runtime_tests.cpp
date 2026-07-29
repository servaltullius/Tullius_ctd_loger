#include <Windows.h>

#include <atomic>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "Analyzer.h"
#include "CleanExitEvidence.h"
#include "DumpIdentity.h"
#include "MinidumpUtil.h"
#include "OutputWriter.h"
#include "OutputWriterInternals.h"

namespace {

using skydiag::dump_tool::AnalysisResult;
using skydiag::dump_tool::ComputeDumpIdentity;
using skydiag::dump_tool::DumpIdentity;
using skydiag::dump_tool::TryConsumeCleanExitEvidence;
using skydiag::dump_tool::WriteOutputs;
using skydiag::dump_tool::internal::output_writer::DumpIdentityJson;
using skydiag::dump_tool::internal::output_writer::IdentityArtifactDirectory;
using skydiag::dump_tool::internal::output_writer::OutputFamilyLockPath;
using skydiag::dump_tool::internal::output_writer::WriteTextUtf8;
using skydiag::dump_tool::internal::output_writer::WriteTriageState;
using skydiag::dump_tool::minidump::MappedFile;

std::filesystem::path MakeTempDir(std::wstring_view suffix)
{
  const auto root = std::filesystem::temp_directory_path() /
    (L"skydiag_analyzer_state_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
     std::wstring(suffix));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  assert(!ec);
  return root;
}

std::string ReadAllText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  assert(in);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

void WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  assert(!ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  assert(out);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();
  assert(out);
}

void SetLastWriteTimeUtc100ns(
  const std::filesystem::path& path,
  std::uint64_t value)
{
  const HANDLE file = CreateFileW(
    path.c_str(),
    FILE_WRITE_ATTRIBUTES,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  assert(file != INVALID_HANDLE_VALUE);
  ULARGE_INTEGER ticks{};
  ticks.QuadPart = value;
  FILETIME modified{};
  modified.dwHighDateTime = ticks.HighPart;
  modified.dwLowDateTime = ticks.LowPart;
  assert(SetFileTime(file, nullptr, nullptr, &modified));
  CloseHandle(file);
}

DumpIdentity ComputeIdentity(
  const std::filesystem::path& path,
  const std::vector<std::uint8_t>& bytes)
{
  MappedFile mapped{};
  std::wstring err;
  assert(mapped.Open(path.wstring(), &err));
  assert(err.empty());
  assert(mapped.size == bytes.size());

  DumpIdentity identity{};
  assert(ComputeDumpIdentity(mapped.file.get(), mapped.view, mapped.size, &identity, &err));
  assert(err.empty());
  assert(identity.IsValid());
  return identity;
}

AnalysisResult MakeIdentityBoundResult(
  const std::filesystem::path& dumpPath,
  const std::filesystem::path& outDir,
  const DumpIdentity& identity,
  std::wstring label)
{
  AnalysisResult result{};
  result.dump_path = dumpPath.wstring();
  result.out_dir = outDir.wstring();
  result.dump_identity = identity;
  result.path_redaction_applied = true;
  result.summary_sentence = std::move(label);
  result.crash_bucket_key = L"SNAPSHOT";
  result.is_snapshot_like = true;
  return result;
}

nlohmann::json MakeCleanExitEvidence(
  const std::filesystem::path& dumpPath,
  const AnalysisResult& result,
  std::string state = "preserved",
  bool dumpPreserved = true)
{
  return {
    { "schema", "skydiag.clean_exit_evidence.v1" },
    { "dump_state", std::move(state) },
    { "dump_preserved", dumpPreserved },
    { "dump_filename", dumpPath.filename().string() },
    { "dump_size_bytes", result.dump_identity.size_bytes },
    { "dump_last_write_time_utc_100ns", result.dump_identity.last_write_time_utc_100ns },
    { "exception_code", result.exc_code },
    { "exception_addr", result.exc_addr },
    { "faulting_tid", result.exc_tid },
    { "state_flags", result.state_flags },
    { "crash_seq", result.blackbox_crash_seq },
  };
}

AnalysisResult MakeCleanExitResult(const DumpIdentity& identity)
{
  AnalysisResult result{};
  result.dump_identity = identity;
  result.exc_code = 0xC0000005u;
  result.exc_addr = 0x12345678u;
  result.exc_tid = 4242u;
  result.has_blackbox = true;
  result.state_flags = 0x00000001u;
  result.blackbox_crash_seq = 2u;
  result.blackbox_exception_code = result.exc_code;
  result.blackbox_exception_addr = result.exc_addr;
  result.blackbox_faulting_tid = result.exc_tid;
  return result;
}

void WriteEvidence(
  const std::filesystem::path& directory,
  std::wstring_view filename,
  const nlohmann::json& evidence)
{
  std::wstring err;
  assert(WriteTextUtf8(directory / filename, evidence.dump(2) + "\n", &err));
  assert(err.empty());
}

void TestAtomicWriterSurvivesLegacyStaleTempCollision()
{
  const auto root = MakeTempDir(L"atomic_stale_temp");
  const auto destination = root / L"state.json";
  auto staleTemp = destination;
  staleTemp += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L".0";
  WriteBytes(staleTemp, { 's', 't', 'a', 'l', 'e' });

  std::wstring err;
  assert(WriteTextUtf8(destination, "fresh\n", &err));
  assert(err.empty());
  assert(ReadAllText(destination) == "fresh\n");
  assert(ReadAllText(staleTemp) == "stale");

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

void TestDumpIdentityHashAndStorageKey()
{
  const auto root = MakeTempDir(L"identity");
  const auto dump = root / L"known.dmp";
  const std::vector<std::uint8_t> bytes{ 'a', 'b', 'c' };
  WriteBytes(dump, bytes);
  const auto identity = ComputeIdentity(dump, bytes);
  assert(identity.sha256 == "ba7816bf8f01cfea414140de5dae2223"
                            "b00361a396177a9cb410ff61f20015ad");
  assert(identity.StorageMetadataKey().size() == 33u);
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

void TestDumpIdentityStaysBoundToMappedHandleAcrossPathReplacement()
{
  const auto root = MakeTempDir(L"identity_path_replacement");
  const auto dump = root / L"race.dmp";
  const auto retained = root / L"mapped-original.dmp";
  const auto replacement = root / L"replacement.dmp";
  const std::vector<std::uint8_t> originalBytes{ 'a', 'b', 'c' };
  const std::vector<std::uint8_t> replacementBytes{ 'x', 'y', 'z' };
  WriteBytes(dump, originalBytes);
  WriteBytes(replacement, replacementBytes);
  SetLastWriteTimeUtc100ns(dump, 132537600000000000ull);
  SetLastWriteTimeUtc100ns(replacement, 133801632000000000ull);

  MappedFile mapped{};
  std::wstring err;
  assert(mapped.Open(dump.wstring(), &err));
  assert(err.empty());

  std::uint64_t originalSize = 0;
  std::uint64_t originalLastWrite = 0;
  assert(skydiag::dump_tool::ReadDumpFileMetadata(
    mapped.file.get(),
    &originalSize,
    &originalLastWrite,
    &err));
  assert(err.empty());

  assert(MoveFileExW(
    dump.c_str(),
    retained.c_str(),
    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
  assert(MoveFileExW(
    replacement.c_str(),
    dump.c_str(),
    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));

  DumpIdentity mappedIdentity{};
  assert(ComputeDumpIdentity(
    mapped.file.get(),
    mapped.view,
    mapped.size,
    &mappedIdentity,
    &err));
  assert(err.empty());
  assert(mappedIdentity.sha256 == "ba7816bf8f01cfea414140de5dae2223"
                                  "b00361a396177a9cb410ff61f20015ad");
  assert(mappedIdentity.size_bytes == originalSize);
  assert(mappedIdentity.last_write_time_utc_100ns == originalLastWrite);

  const auto replacementIdentity = ComputeIdentity(dump, replacementBytes);
  assert(replacementIdentity.sha256 == "3608bca1e44ea6c4d268eb6db0226026"
                                       "9892c0b42b86bbf1e77a6fa16c3c9282");
  assert(replacementIdentity.sha256 != mappedIdentity.sha256);
  assert(replacementIdentity.last_write_time_utc_100ns !=
         mappedIdentity.last_write_time_utc_100ns);

  mapped.Close();
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

void TestCleanExitEvidenceFailsClosedAndRequiresExactMatch()
{
  const auto root = MakeTempDir(L"clean_exit");
  const auto dump = root / L"SameStem.dmp";
  const std::vector<std::uint8_t> bytes(4096u, 0x41u);
  WriteBytes(dump, bytes);
  const auto identity = ComputeIdentity(dump, bytes);

  {
    auto result = MakeCleanExitResult(identity);
    WriteEvidence(
      root,
      L"SkyrimDiag_CleanExitEvidence_valid.json",
      MakeCleanExitEvidence(dump, result));
    assert(TryConsumeCleanExitEvidence(dump.wstring(), result));
    assert(result.is_filtered_clean_exit);
    assert(result.clean_exit_dump_state == "preserved");
  }

  std::error_code ec;
  std::filesystem::remove(root / L"SkyrimDiag_CleanExitEvidence_valid.json", ec);
  {
    auto result = MakeCleanExitResult(identity);
    auto mismatch = MakeCleanExitEvidence(dump, result);
    mismatch["exception_addr"] = result.exc_addr + 1u;
    WriteEvidence(root, L"SkyrimDiag_CleanExitEvidence_mismatch.json", mismatch);
    assert(!TryConsumeCleanExitEvidence(dump.wstring(), result));
  }

  std::filesystem::remove(root / L"SkyrimDiag_CleanExitEvidence_mismatch.json", ec);
  {
    auto result = MakeCleanExitResult(identity);
    WriteEvidence(
      root,
      L"SkyrimDiag_CleanExitEvidence_pending.json",
      MakeCleanExitEvidence(dump, result, "pending_delete", true));
    assert(!TryConsumeCleanExitEvidence(dump.wstring(), result));
  }

  std::filesystem::remove(root / L"SkyrimDiag_CleanExitEvidence_pending.json", ec);
  {
    auto result = MakeCleanExitResult(identity);
    WriteEvidence(
      root,
      L"SkyrimDiag_CleanExitEvidence_delete_failed.json",
      MakeCleanExitEvidence(dump, result, "delete_failed", true));
    assert(TryConsumeCleanExitEvidence(dump.wstring(), result));
    assert(result.clean_exit_dump_state == "delete_failed");
  }

  std::filesystem::remove(root / L"SkyrimDiag_CleanExitEvidence_delete_failed.json", ec);
  {
    auto result = MakeCleanExitResult(identity);
    WriteEvidence(
      root,
      L"SkyrimDiag_CleanExitEvidence_one.json",
      MakeCleanExitEvidence(dump, result));
    WriteEvidence(
      root,
      L"SkyrimDiag_CleanExitEvidence_two.json",
      MakeCleanExitEvidence(dump, result));
    assert(!TryConsumeCleanExitEvidence(dump.wstring(), result));
  }

  std::filesystem::remove(root / L"SkyrimDiag_CleanExitEvidence_one.json", ec);
  std::filesystem::remove(root / L"SkyrimDiag_CleanExitEvidence_two.json", ec);
  {
    auto result = MakeCleanExitResult(identity);
    auto invalidSchemaType = MakeCleanExitEvidence(dump, result);
    invalidSchemaType["schema"] = 1;
    WriteEvidence(
      root,
      L"SkyrimDiag_CleanExitEvidence_invalid_schema_type.json",
      invalidSchemaType);
    assert(!TryConsumeCleanExitEvidence(dump.wstring(), result));
    assert(!result.is_filtered_clean_exit);
  }

  std::filesystem::remove(root / L"SkyrimDiag_CleanExitEvidence_invalid_schema_type.json", ec);
  {
    auto result = MakeCleanExitResult(identity);
    auto invalidStateType = MakeCleanExitEvidence(dump, result);
    invalidStateType["dump_state"] = nlohmann::json::array({ "preserved" });
    WriteEvidence(
      root,
      L"SkyrimDiag_CleanExitEvidence_invalid_state_type.json",
      invalidStateType);
    assert(!TryConsumeCleanExitEvidence(dump.wstring(), result));
    assert(!result.is_filtered_clean_exit);
  }

  std::filesystem::remove_all(root, ec);
}

void TestSameStemArtifactFamiliesAndTriageAreIsolated()
{
  const auto root = MakeTempDir(L"same_stem");
  const auto outDir = root / L"out";
  const auto dumpA = root / L"a" / L"SameStem.dmp";
  const auto dumpB = root / L"b" / L"SameStem.dmp";
  const std::vector<std::uint8_t> bytesA(4096u, 0x41u);
  const std::vector<std::uint8_t> bytesB(4096u, 0x42u);
  WriteBytes(dumpA, bytesA);
  WriteBytes(dumpB, bytesB);
  const auto identityA = ComputeIdentity(dumpA, bytesA);
  const auto identityB = ComputeIdentity(dumpB, bytesB);
  assert(identityA.sha256 != identityB.sha256);

  auto resultA = MakeIdentityBoundResult(dumpA, outDir, identityA, L"summary-a");
  auto resultB = MakeIdentityBoundResult(dumpB, outDir, identityB, L"summary-b");
  std::wstring err;
  assert(WriteOutputs(resultA, &err));
  assert(err.empty());

  const nlohmann::json triageA = {
    { "review_status", "confirmed" },
    { "reviewed", true },
    { "verdict", "a-verdict" },
    { "ground_truth_mod", "A.esp" },
  };
  assert(WriteTriageState(outDir, identityA, triageA, &err));
  assert(WriteOutputs(resultA, &err));
  assert(WriteOutputs(resultB, &err));

  const auto familyA = IdentityArtifactDirectory(outDir, identityA);
  const auto familyB = IdentityArtifactDirectory(outDir, identityB);
  assert(familyA != familyB);
  const auto summaryA = nlohmann::json::parse(ReadAllText(familyA / L"Summary.json"));
  const auto summaryB = nlohmann::json::parse(ReadAllText(familyB / L"Summary.json"));
  assert(summaryA["summary_sentence"] == "summary-a");
  assert(summaryB["summary_sentence"] == "summary-b");
  assert(summaryA["triage"]["ground_truth_mod"] == "A.esp");
  assert(summaryB["triage"]["ground_truth_mod"] == "");
  assert(summaryA["dump_identity"] == DumpIdentityJson(identityA));
  assert(summaryB["dump_identity"] == DumpIdentityJson(identityB));
  assert(ReadAllText(familyA / L"Report.txt").find("summary-a") != std::string::npos);
  assert(ReadAllText(familyB / L"Report.txt").find("summary-b") != std::string::npos);

  const auto compatibilitySummary =
    nlohmann::json::parse(ReadAllText(outDir / L"SameStem_SkyrimDiagSummary.json"));
  assert(compatibilitySummary["dump_identity"] == DumpIdentityJson(identityB));

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

void TestOutputFamilyLockSerializesConcurrentPublish()
{
  const auto root = MakeTempDir(L"output_family_lock");
  const auto outDir = root / L"out";
  const auto dump = root / L"Concurrent.dmp";
  const std::vector<std::uint8_t> bytes(4096u, 0x63u);
  WriteBytes(dump, bytes);
  const auto identity = ComputeIdentity(dump, bytes);
  auto result = MakeIdentityBoundResult(
    dump,
    outDir,
    identity,
    L"serialized-output");

  const auto lockPath = OutputFamilyLockPath(outDir, dump.stem().wstring());
  std::error_code ec;
  std::filesystem::create_directories(lockPath.parent_path(), ec);
  assert(!ec);
  const HANDLE heldLock = CreateFileW(
    lockPath.c_str(),
    GENERIC_READ | GENERIC_WRITE,
    0,
    nullptr,
    OPEN_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  assert(heldLock != INVALID_HANDLE_VALUE);

  std::atomic_bool started{ false };
  std::atomic_bool finished{ false };
  bool writeSucceeded = false;
  std::wstring writeError;
  std::thread writer([&]() {
    started.store(true, std::memory_order_release);
    writeSucceeded = WriteOutputs(result, &writeError);
    finished.store(true, std::memory_order_release);
  });
  while (!started.load(std::memory_order_acquire)) {
    Sleep(1);
  }
  Sleep(100);
  assert(!finished.load(std::memory_order_acquire));
  assert(!std::filesystem::exists(
    IdentityArtifactDirectory(outDir, identity) / L"Summary.json"));

  CloseHandle(heldLock);
  writer.join();
  assert(writeSucceeded);
  assert(writeError.empty());
  assert(finished.load(std::memory_order_acquire));
  assert(std::filesystem::exists(
    IdentityArtifactDirectory(outDir, identity) / L"Summary.json"));

  std::filesystem::remove_all(root, ec);
}

void TestOptionalArtifactCleanupIsFailSafe()
{
  const auto root = MakeTempDir(L"optional_cleanup");
  const auto outDir = root / L"out";
  const auto dump = root / L"Optional.dmp";
  const std::vector<std::uint8_t> bytes(4096u, 0x51u);
  WriteBytes(dump, bytes);
  const auto identity = ComputeIdentity(dump, bytes);
  auto result = MakeIdentityBoundResult(dump, outDir, identity, L"with-optional");
  result.has_blackbox = true;
  result.has_wct = true;
  result.wct_json_utf8 = R"({"schema":"test"})";

  std::wstring err;
  assert(WriteOutputs(result, &err));
  const auto identityFamily = IdentityArtifactDirectory(outDir, identity);
  const auto identityBlackbox = identityFamily / L"Blackbox.jsonl";
  const auto identityWct = identityFamily / L"Wct.json";
  const auto legacySummary = outDir / L"Optional_SkyrimDiagSummary.json";
  const auto legacyBlackbox = outDir / L"Optional_SkyrimDiagBlackbox.jsonl";
  const auto legacyWct = outDir / L"Optional_SkyrimDiagWct.json";
  assert(std::filesystem::is_regular_file(identityBlackbox));
  assert(std::filesystem::is_regular_file(identityWct));
  assert(std::filesystem::is_regular_file(legacyBlackbox));
  assert(std::filesystem::is_regular_file(legacyWct));

  result.has_blackbox = false;
  result.has_wct = false;
  result.wct_json_utf8.clear();
  result.summary_sentence = L"without-optional";
  assert(WriteOutputs(result, &err));
  assert(!std::filesystem::exists(identityBlackbox));
  assert(!std::filesystem::exists(identityWct));
  assert(!std::filesystem::exists(legacyBlackbox));
  assert(!std::filesystem::exists(legacyWct));
  assert(nlohmann::json::parse(ReadAllText(identityFamily / L"Summary.json"))["summary_sentence"] ==
         "without-optional");
  assert(nlohmann::json::parse(ReadAllText(legacySummary))["summary_sentence"] ==
         "without-optional");

  result.has_blackbox = true;
  result.has_wct = true;
  result.wct_json_utf8 = R"({"schema":"test-locked"})";
  result.summary_sentence = L"before-delete-failure";
  assert(WriteOutputs(result, &err));

  const HANDLE lockedBlackbox = CreateFileW(
    identityBlackbox.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  assert(lockedBlackbox != INVALID_HANDLE_VALUE);

  result.has_blackbox = false;
  result.has_wct = false;
  result.wct_json_utf8.clear();
  result.summary_sentence = L"must-not-publish";
  err.clear();
  assert(!WriteOutputs(result, &err));
  assert(!err.empty());
  assert(!std::filesystem::exists(identityFamily / L"Summary.json"));
  assert(nlohmann::json::parse(ReadAllText(legacySummary))["summary_sentence"] ==
         "before-delete-failure");
  CloseHandle(lockedBlackbox);

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

}  // namespace

int main()
{
  TestAtomicWriterSurvivesLegacyStaleTempCollision();
  TestDumpIdentityHashAndStorageKey();
  TestDumpIdentityStaysBoundToMappedHandleAcrossPathReplacement();
  TestCleanExitEvidenceFailsClosedAndRequiresExactMatch();
  TestSameStemArtifactFamiliesAndTriageAreIsolated();
  TestOutputFamilyLockSerializesConcurrentPublish();
  TestOptionalArtifactCleanupIsFailSafe();
  return 0;
}
