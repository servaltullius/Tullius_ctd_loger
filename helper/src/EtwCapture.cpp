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
  return static_cast<DWORD>(sec * 1000u);
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
  const std::wstring wprExe = ResolveEtwWprExecutablePath(cfg).wstring();
  std::wstring cmd = QuoteArg(wprExe) + L" -stop " + QuoteArg(etlPath.wstring());
  return RunHiddenProcessAndWait(wprExe, std::move(cmd), outBase, EtwTimeoutMs(cfg), err);
}

}  // namespace skydiag::helper::internal
