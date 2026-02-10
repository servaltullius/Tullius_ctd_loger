#include "EtwCapture.h"

#include <Windows.h>

#include <algorithm>

#include "HelperCommon.h"
#include "ProcessUtil.h"
#include "SkyrimDiagHelper/Config.h"

namespace skydiag::helper::internal {
namespace {

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
  const std::wstring wprExe = cfg.etwWprExe.empty() ? L"wpr.exe" : cfg.etwWprExe;
  const std::wstring effectiveProfile = profile.empty() ? L"GeneralProfile" : std::wstring(profile);
  std::wstring cmd = QuoteArg(wprExe) + L" -start " + QuoteArg(effectiveProfile) + L" -filemode";
  return RunHiddenProcessAndWait(std::move(cmd), outBase, EtwTimeoutMs(cfg), err);
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
  const std::wstring wprExe = cfg.etwWprExe.empty() ? L"wpr.exe" : cfg.etwWprExe;
  std::wstring cmd = QuoteArg(wprExe) + L" -stop " + QuoteArg(etlPath.wstring());
  return RunHiddenProcessAndWait(std::move(cmd), outBase, EtwTimeoutMs(cfg), err);
}

}  // namespace skydiag::helper::internal

