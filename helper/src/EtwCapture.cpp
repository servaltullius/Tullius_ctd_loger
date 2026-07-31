#include "EtwCapture.h"

#include <Windows.h>

#include <algorithm>
#include <vector>

#include "HelperCommon.h"
#include "ProcessUtil.h"
#include "SkyrimDiagHelper/Config.h"

namespace skydiag::helper::internal {
namespace {

std::filesystem::path ResolveHelperExeDir()
{
  std::vector<wchar_t> buf(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  if (n == 0 || n >= buf.size()) {
    return {};
  }
  return std::filesystem::path(std::wstring_view(buf.data(), n)).parent_path();
}

std::filesystem::path ResolveEtwWprExecutablePath(const skydiag::helper::HelperConfig& cfg)
{
  std::filesystem::path configured = cfg.etwWprExe.empty() ? std::filesystem::path(L"wpr.exe") : std::filesystem::path(cfg.etwWprExe);
  if (configured.empty()) {
    configured = L"wpr.exe";
  }

  const bool defaultWprName =
    configured.parent_path().empty() &&
    EqualsIgnoreCase(configured.filename().wstring(), L"wpr.exe");

  // Use an explicit System32 path by default so CreateProcess never resolves wpr.exe
  // from an attacker-controlled output directory/current working directory.
  if (defaultWprName) {
    wchar_t systemDir[MAX_PATH]{};
    const DWORD n = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      return std::filesystem::path(std::wstring_view(systemDir, n)) / L"wpr.exe";
    }
  }

  if (configured.is_relative()) {
    const auto baseDir = ResolveHelperExeDir();
    if (!baseDir.empty()) {
      configured = baseDir / configured;
    }
  }

  std::error_code ec;
  const auto abs = std::filesystem::absolute(configured, ec);
  if (!ec) {
    return abs;
  }
  return configured;
}

DWORD EtwTimeoutMs(const skydiag::helper::HelperConfig& cfg)
{
  std::uint32_t sec = cfg.etwMaxDurationSec;
  if (sec < 10u) {
    sec = 10u;
  }
  if (sec > 120u) {
    sec = 120u;
  }
  return sec * 1000u;
}

struct EtwArtifactIdentity
{
  bool exists = false;
  DWORD volumeSerialNumber = 0;
  std::uint64_t fileIndex = 0;
  std::uint64_t size = 0;
  std::uint64_t lastWriteTimeUtc100ns = 0;
};

bool CaptureEtwArtifactIdentity(
  const std::filesystem::path& etlPath,
  EtwArtifactIdentity* out,
  std::wstring* err)
{
  if (out) {
    *out = EtwArtifactIdentity{};
  }
  if (!out || etlPath.empty()) {
    if (err) {
      *err = L"no ETL output path was provided";
    }
    return false;
  }

  HANDLE file = CreateFileW(
    etlPath.c_str(),
    FILE_READ_ATTRIBUTES,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD lastError = GetLastError();
    if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND) {
      return true;
    }
    if (err) {
      *err =
        L"could not read ETL artifact identity: "
        + etlPath.wstring()
        + L" (err="
        + std::to_wstring(lastError)
        + L")";
    }
    return false;
  }

  BY_HANDLE_FILE_INFORMATION info{};
  const BOOL infoOk = GetFileInformationByHandle(file, &info);
  const DWORD infoError = infoOk ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  if (!infoOk) {
    if (err) {
      *err =
        L"could not read ETL artifact identity: "
        + etlPath.wstring()
        + L" (err="
        + std::to_wstring(infoError)
        + L")";
    }
    return false;
  }

  ULARGE_INTEGER fileIndex{};
  fileIndex.HighPart = info.nFileIndexHigh;
  fileIndex.LowPart = info.nFileIndexLow;
  ULARGE_INTEGER size{};
  size.HighPart = info.nFileSizeHigh;
  size.LowPart = info.nFileSizeLow;
  ULARGE_INTEGER lastWrite{};
  lastWrite.HighPart = info.ftLastWriteTime.dwHighDateTime;
  lastWrite.LowPart = info.ftLastWriteTime.dwLowDateTime;

  out->exists = true;
  out->volumeSerialNumber = info.dwVolumeSerialNumber;
  out->fileIndex = fileIndex.QuadPart;
  out->size = size.QuadPart;
  out->lastWriteTimeUtc100ns = lastWrite.QuadPart;
  return true;
}

bool IsSameEtwArtifactIdentity(
  const EtwArtifactIdentity& before,
  const EtwArtifactIdentity& after) noexcept
{
  return
    before.exists &&
    after.exists &&
    before.volumeSerialNumber == after.volumeSerialNumber &&
    before.fileIndex == after.fileIndex &&
    before.size == after.size &&
    before.lastWriteTimeUtc100ns == after.lastWriteTimeUtc100ns;
}

bool ValidateWrittenEtwArtifact(
  const std::filesystem::path& etlPath,
  const EtwArtifactIdentity& before,
  std::wstring* err)
{
  if (etlPath.empty()) {
    if (err) {
      *err = L"wpr -stop exited successfully but no ETL output path was provided";
    }
    return false;
  }

  std::error_code ec;
  const auto status = std::filesystem::status(etlPath, ec);
  if (ec || !std::filesystem::exists(status)) {
    if (err) {
      *err =
        L"wpr -stop exited successfully but did not create the ETL file: "
        + etlPath.wstring();
      if (ec) {
        *err += L" (err=" + std::to_wstring(ec.value()) + L")";
      }
    }
    return false;
  }
  if (!std::filesystem::is_regular_file(status)) {
    if (err) {
      *err =
        L"wpr -stop exited successfully but the ETL output is not a regular file: "
        + etlPath.wstring();
    }
    return false;
  }

  const auto size = std::filesystem::file_size(etlPath, ec);
  if (ec || size == 0u) {
    if (err) {
      *err =
        L"wpr -stop exited successfully but the ETL file is empty or unreadable: "
        + etlPath.wstring();
      if (ec) {
        *err += L" (err=" + std::to_wstring(ec.value()) + L")";
      }
    }
    return false;
  }

  EtwArtifactIdentity after{};
  if (!CaptureEtwArtifactIdentity(etlPath, &after, err)) {
    return false;
  }
  if (IsSameEtwArtifactIdentity(before, after)) {
    if (err) {
      *err =
        L"wpr -stop exited successfully but the pre-existing ETL file was unchanged; "
        L"fresh artifact creation cannot be confirmed: "
        + etlPath.wstring();
    }
    return false;
  }

  if (err) {
    err->clear();
  }
  return true;
}

}  // namespace

bool StartEtwCaptureWithProfile(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  std::wstring_view profile,
  std::wstring* err)
{
  const std::wstring wprExe = ResolveEtwWprExecutablePath(cfg).wstring();
  const std::wstring effectiveProfile = profile.empty() ? L"GeneralProfile" : std::wstring(profile);
  std::wstring cmd = QuoteArg(wprExe) + L" -start " + QuoteArg(effectiveProfile) + L" -filemode";
  return RunHiddenProcessAndWait(wprExe, std::move(cmd), outBase, EtwTimeoutMs(cfg), err);
}

bool StartEtwCaptureForHang(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  std::wstring* outUsedProfile,
  std::wstring* err)
{
  if (outUsedProfile) {
    outUsedProfile->clear();
  }

  const std::wstring primaryProfile = cfg.etwHangProfile.empty() ? L"GeneralProfile" : cfg.etwHangProfile;
  std::wstring primaryErr;
  if (StartEtwCaptureWithProfile(cfg, outBase, primaryProfile, &primaryErr)) {
    if (outUsedProfile) {
      *outUsedProfile = primaryProfile;
    }
    if (err) {
      err->clear();
    }
    return true;
  }

  const std::wstring fallbackProfile = cfg.etwHangFallbackProfile;
  if (!fallbackProfile.empty() && !EqualsIgnoreCase(primaryProfile, fallbackProfile)) {
    std::wstring fallbackErr;
    if (StartEtwCaptureWithProfile(cfg, outBase, fallbackProfile, &fallbackErr)) {
      if (outUsedProfile) {
        *outUsedProfile = fallbackProfile;
      }
      if (err) {
        err->clear();
      }
      return true;
    }
    if (err) {
      *err = L"primary(" + primaryProfile + L") failed: " + primaryErr + L"; fallback(" + fallbackProfile +
        L") failed: " + fallbackErr;
    }
    return false;
  }

  if (err) {
    *err = L"primary(" + primaryProfile + L") failed: " + primaryErr;
  }
  return false;
}

bool StopEtwCaptureToPath(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  const std::filesystem::path& etlPath,
  std::wstring* err)
{
  EtwArtifactIdentity before{};
  if (!CaptureEtwArtifactIdentity(etlPath, &before, err)) {
    return false;
  }

  const std::wstring wprExe = ResolveEtwWprExecutablePath(cfg).wstring();
  std::wstring cmd = QuoteArg(wprExe) + L" -stop " + QuoteArg(etlPath.wstring());
  if (!RunHiddenProcessAndWait(
        wprExe,
        std::move(cmd),
        outBase,
        EtwTimeoutMs(cfg),
        err)) {
    return false;
  }
  return ValidateWrittenEtwArtifact(etlPath, before, err);
}

bool CancelEtwCapture(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  std::wstring* err)
{
  const std::wstring wprExe = ResolveEtwWprExecutablePath(cfg).wstring();
  std::wstring cmd = QuoteArg(wprExe) + L" -cancel";
  return RunHiddenProcessAndWait(wprExe, std::move(cmd), outBase, EtwTimeoutMs(cfg), err);
}

EtwFinalizeStatus FinalizeEtwCaptureWithCleanup(
  const skydiag::helper::HelperConfig& cfg,
  const std::filesystem::path& outBase,
  const std::filesystem::path& etlPath,
  std::uint32_t maxStopAttempts,
  std::wstring* err)
{
  maxStopAttempts = std::clamp<std::uint32_t>(maxStopAttempts, 1u, 5u);
  std::wstring stopErrors;
  for (std::uint32_t attempt = 0; attempt < maxStopAttempts; ++attempt) {
    std::wstring stopErr;
    if (StopEtwCaptureToPath(cfg, outBase, etlPath, &stopErr)) {
      if (err) {
        err->clear();
      }
      return EtwFinalizeStatus::kWritten;
    }
    if (!stopErrors.empty()) {
      stopErrors += L"; ";
    }
    stopErrors +=
      L"stop attempt "
      + std::to_wstring(attempt + 1u)
      + L": "
      + stopErr;
  }

  std::wstring cancelErr;
  if (CancelEtwCapture(cfg, outBase, &cancelErr)) {
    if (err) {
      *err = stopErrors;
    }
    return EtwFinalizeStatus::kCancelledAfterStopFailure;
  }
  if (err) {
    *err = stopErrors + L"; cancel: " + cancelErr;
  }
  return EtwFinalizeStatus::kCleanupUnconfirmed;
}

}  // namespace skydiag::helper::internal
