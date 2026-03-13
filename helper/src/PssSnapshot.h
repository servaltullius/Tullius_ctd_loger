#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

namespace skydiag::helper::internal {

struct PssSnapshotAttempt
{
  HANDLE snapshotHandle = nullptr;
  bool requested = false;
  bool used = false;
  bool apiAvailable = false;
  std::uint32_t captureDurationMs = 0;
  std::wstring status = L"disabled";
};

PssSnapshotAttempt TryCapturePssSnapshotForFreeze(bool enabled, HANDLE process);
void ReleasePssSnapshotForFreeze(HANDLE process, HANDLE snapshotHandle);

}  // namespace skydiag::helper::internal
