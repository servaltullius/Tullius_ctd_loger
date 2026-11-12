#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "SkyrimDiagShared.h"

namespace skydiag::helper {

struct AttachedProcess {
  std::uint32_t pid = 0;
  HANDLE process = nullptr;

  HANDLE shmMapping = nullptr;
  const skydiag::SharedLayout* shm = nullptr;
  skydiag::SharedLayout* shmWritable = nullptr;
  std::size_t shmSize = 0;  // mapped bytes (best-effort)

  HANDLE crashEvent = nullptr;
  std::uint32_t crashEventOpenError = ERROR_SUCCESS;

  // ADR-0032. unhandledEvent: the plugin sets it when an exception reached
  // the top-level filter (opened here; null with a plugin that does not have
  // it). dumpSettledEvent: created here and set when a crash dump write is
  // over, so the plugin stops holding the crashing thread; made only when the
  // plugin's event exists.
  HANDLE unhandledEvent = nullptr;
  HANDLE dumpSettledEvent = nullptr;
};

bool AttachByPid(std::uint32_t pid, AttachedProcess& out, std::wstring* err);
bool FindAndAttach(AttachedProcess& out, std::wstring* err);
bool TryAttachCrashEvent(AttachedProcess& proc, std::wstring* err);
void Detach(AttachedProcess& p);

}  // namespace skydiag::helper
