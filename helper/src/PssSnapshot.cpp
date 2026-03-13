#include "PssSnapshot.h"

#include <Windows.h>
#include <ProcessSnapshot.h>

#include <string>

namespace skydiag::helper::internal {
namespace {

using PssCaptureSnapshotFn = decltype(&::PssCaptureSnapshot);
using PssFreeSnapshotFn = decltype(&::PssFreeSnapshot);

struct PssApi
{
  PssCaptureSnapshotFn capture = nullptr;
  PssFreeSnapshotFn freeSnapshot = nullptr;
};

bool TryLoadPssApi(PssApi* api, std::wstring* status)
{
  if (!api) {
    if (status) {
      *status = L"invalid_pss_api_target";
    }
    return false;
  }

  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  if (!kernel32) {
    if (status) {
      *status = L"kernel32_unavailable";
    }
    return false;
  }

  api->capture = reinterpret_cast<PssCaptureSnapshotFn>(GetProcAddress(kernel32, "PssCaptureSnapshot"));
  api->freeSnapshot = reinterpret_cast<PssFreeSnapshotFn>(GetProcAddress(kernel32, "PssFreeSnapshot"));
  if (!api->capture || !api->freeSnapshot) {
    if (status) {
      *status = L"pss_api_unavailable";
    }
    return false;
  }

  if (status) {
    status->assign(L"pss_api_available");
  }
  return true;
}

constexpr PSS_CAPTURE_FLAGS kFreezeSnapshotFlags = static_cast<PSS_CAPTURE_FLAGS>(
  PSS_CAPTURE_VA_CLONE |
  PSS_CAPTURE_THREADS |
  PSS_CAPTURE_THREAD_CONTEXT);

}  // namespace

PssSnapshotAttempt TryCapturePssSnapshotForFreeze(bool enabled, HANDLE process)
{
  PssSnapshotAttempt result{};
  result.requested = enabled;
  if (!enabled) {
    result.status = L"disabled";
    return result;
  }

  if (!process) {
    result.status = L"invalid_process_handle";
    return result;
  }

  PssApi api{};
  if (!TryLoadPssApi(&api, &result.status)) {
    result.apiAvailable = false;
    return result;
  }

  result.apiAvailable = true;
  HPSS snapshot = nullptr;
  const ULONGLONG startTick = GetTickCount64();
  const DWORD status = api.capture(
    process,
    kFreezeSnapshotFlags,
    CONTEXT_ALL,
    &snapshot);
  result.captureDurationMs = static_cast<std::uint32_t>(GetTickCount64() - startTick);

  if (status != ERROR_SUCCESS || !snapshot) {
    result.status = L"PssCaptureSnapshot failed: " + std::to_wstring(status);
    return result;
  }

  result.snapshotHandle = snapshot;
  result.used = true;
  result.status = L"captured";
  return result;
}

void ReleasePssSnapshotForFreeze(HANDLE process, HANDLE snapshotHandle)
{
  if (!snapshotHandle) {
    return;
  }

  PssApi api{};
  std::wstring status;
  if (TryLoadPssApi(&api, &status) && api.freeSnapshot) {
    (void)api.freeSnapshot(process, reinterpret_cast<HPSS>(snapshotHandle));
    return;
  }

  CloseHandle(snapshotHandle);
}

}  // namespace skydiag::helper::internal
