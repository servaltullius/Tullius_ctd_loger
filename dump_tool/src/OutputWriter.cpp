#include "OutputWriter.h"
#include "OutputWriterPipeline.h"
#include "OutputWriterInternals.h"
#include "Utf.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>
namespace skydiag::dump_tool {

using skydiag::dump_tool::internal::output_writer::DefaultOutDirForDump;
using skydiag::dump_tool::internal::output_writer::IdentityArtifactDirectory;
using skydiag::dump_tool::internal::output_writer::OutputFamilyLockPath;
using skydiag::dump_tool::internal::output_writer::TriageHasReviewContent;
using skydiag::dump_tool::internal::output_writer::WriteTextUtf8;
using skydiag::dump_tool::internal::output_writer::WriteTriageState;

namespace {

class OutputFamilyFileLock final
{
public:
  OutputFamilyFileLock() = default;
  OutputFamilyFileLock(const OutputFamilyFileLock&) = delete;
  OutputFamilyFileLock& operator=(const OutputFamilyFileLock&) = delete;

  ~OutputFamilyFileLock()
  {
    if (handle_) {
      CloseHandle(handle_);
    }
  }

  bool Acquire(
    const std::filesystem::path& outBase,
    std::wstring_view dumpStem,
    std::wstring* err)
  {
    const auto lockPath = OutputFamilyLockPath(outBase, dumpStem);
    std::error_code directoryError;
    std::filesystem::create_directories(lockPath.parent_path(), directoryError);
    if (directoryError) {
      if (err) {
        *err = L"Failed to create output-family lock directory: " +
          lockPath.parent_path().wstring();
      }
      return false;
    }

    constexpr ULONGLONG kOutputFamilyLockTimeoutMs = 30000u;
    constexpr DWORD kRetryDelayMs = 25u;
    const ULONGLONG startedAt = GetTickCount64();
    DWORD openError = ERROR_SUCCESS;
    do {
      handle_ = CreateFileW(
        lockPath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
      if (handle_ != INVALID_HANDLE_VALUE) {
        return true;
      }
      handle_ = nullptr;
      openError = GetLastError();
      if (openError != ERROR_SHARING_VIOLATION &&
          openError != ERROR_LOCK_VIOLATION) {
        break;
      }
      Sleep(kRetryDelayMs);
    } while (GetTickCount64() - startedAt < kOutputFamilyLockTimeoutMs);

    if (err) {
      if (openError == ERROR_SHARING_VIOLATION ||
          openError == ERROR_LOCK_VIOLATION) {
        *err = L"Timed out waiting for another analysis to publish this dump output family";
      } else {
        *err = L"Failed to acquire output-family file lock: " +
          std::to_wstring(openError);
      }
    }
    return false;
  }

private:
  HANDLE handle_ = nullptr;
};

}  // namespace

bool WriteOutputs(const AnalysisResult& r, std::wstring* err)
{
  const std::filesystem::path dumpFs(r.dump_path);
  std::filesystem::path outBase = r.out_dir.empty() ? DefaultOutDirForDump(dumpFs) : std::filesystem::path(r.out_dir);
  std::error_code ec;
  std::filesystem::create_directories(outBase, ec);

  const std::wstring stem = dumpFs.stem().wstring();
  const bool redactPaths = r.path_redaction_applied;

  const auto summaryPath = outBase / (stem + L"_SkyrimDiagSummary.json");
  const auto reportPath = outBase / (stem + L"_SkyrimDiagReport.txt");
  const auto blackboxPath = outBase / (stem + L"_SkyrimDiagBlackbox.jsonl");
  const auto wctPath = outBase / (stem + L"_SkyrimDiagWct.json");
  const auto identityBase = IdentityArtifactDirectory(outBase, r.dump_identity);
  if (identityBase.empty()) {
    if (err) {
      *err = L"Cannot publish analysis outputs without a valid dump identity";
    }
    return false;
  }

  OutputFamilyFileLock outputFamilyLock;
  if (!outputFamilyLock.Acquire(outBase, stem, err)) {
    return false;
  }

  nlohmann::json summary = BuildSummaryJson(r, outBase, stem, redactPaths);
  const std::string summaryText = summary.dump(2) + "\n";
  const std::string reportText = BuildReportText(r, summary, redactPaths);

  std::string blackboxText;
  if (r.has_blackbox) {
    std::ostringstream bb;
    for (const auto& ev : r.events) {
      nlohmann::json j = nlohmann::json::object();
      j["i"] = ev.i;
      j["t_ms"] = ev.t_ms;
      j["tid"] = ev.tid;
      j["type"] = static_cast<std::uint32_t>(ev.type);
      j["type_name"] = WideToUtf8(ev.type_name);
      j["a"] = ev.a;
      j["b"] = ev.b;
      j["c"] = ev.c;
      j["d"] = ev.d;
      if (!ev.detail.empty()) {
        j["detail"] = WideToUtf8(ev.detail);
      }
      bb << j.dump() << "\n";
    }
    blackboxText = bb.str();
  }

  std::wstring writeErr;
  const auto removeArtifactIfPresent = [&](const std::filesystem::path& path, std::wstring_view artifactName) {
    std::error_code removeEc;
    std::filesystem::remove(path, removeEc);
    if (!removeEc) {
      return true;
    }
    writeErr = L"Failed to remove stale ";
    writeErr.append(artifactName);
    writeErr.append(L": ");
    writeErr.append(path.wstring());
    writeErr.append(L" (");
    writeErr.append(Utf8ToWide(removeEc.message()));
    writeErr.push_back(L')');
    return false;
  };
  const auto writeFamily = [&](const std::filesystem::path& familySummaryPath,
                               const std::filesystem::path& familyReportPath,
                               const std::filesystem::path& familyBlackboxPath,
                               const std::filesystem::path& familyWctPath) {
    // Invalidate the prior generation before mutating companions, then publish
    // the new summary last. A visible summary therefore authorizes only
    // companions reconciled for the same complete generation.
    if (!removeArtifactIfPresent(familySummaryPath, L"summary")) {
      return false;
    }
    if (!WriteTextUtf8(familyReportPath, reportText, &writeErr)) {
      return false;
    }
    if (r.has_blackbox) {
      if (!WriteTextUtf8(familyBlackboxPath, blackboxText, &writeErr)) {
        return false;
      }
    } else if (!removeArtifactIfPresent(familyBlackboxPath, L"blackbox companion")) {
      return false;
    }
    if (r.has_wct) {
      if (!WriteTextUtf8(familyWctPath, r.wct_json_utf8, &writeErr)) {
        return false;
      }
    } else if (!removeArtifactIfPresent(familyWctPath, L"WCT companion")) {
      return false;
    }
    return WriteTextUtf8(familySummaryPath, summaryText, &writeErr);
  };

  if (summary.contains("triage") &&
      TriageHasReviewContent(summary["triage"]) &&
      !WriteTriageState(outBase, r.dump_identity, summary["triage"], &writeErr)) {
    if (err) *err = writeErr;
    return false;
  }

  // The identity-bound family is authoritative and lets multiple same-stem
  // dumps coexist in one output directory without sharing reports or review
  // state. Keep the legacy stem paths as a compatibility alias.
  if (!writeFamily(
        identityBase / L"Summary.json",
        identityBase / L"Report.txt",
        identityBase / L"Blackbox.jsonl",
        identityBase / L"Wct.json")) {
    if (err) *err = writeErr;
    return false;
  }
  if (!writeFamily(summaryPath, reportPath, blackboxPath, wctPath)) {
      if (err) *err = writeErr;
      return false;
  }

  if (err) err->clear();
  return true;
}

}  // namespace skydiag::dump_tool
