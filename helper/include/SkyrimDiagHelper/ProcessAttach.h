#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "SkyrimDiagSecondFault.h"
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
  // GetLastError of the last failed try at each (ERROR_SUCCESS: none failed).
  // ERROR_FILE_NOT_FOUND for the plugin's event means the plugin has none.
  std::uint32_t unhandledEventOpenError = ERROR_SUCCESS;
  std::uint32_t dumpSettledEventCreateError = ERROR_SUCCESS;

  HANDLE secondFaultMapping = nullptr;
  skydiag::second_fault::Slot* secondFault = nullptr;
  HANDLE secondFaultPosted = nullptr;
  // Made by the capture loop (StartSecondFaultSignal); closed when the helper
  // stops (StopSecondFaultSignal), which the plugin reads as no helper.
  mutable HANDLE secondFaultSettled = nullptr;
  std::uint32_t secondFaultAttachError = ERROR_SUCCESS;
  std::filesystem::path secondFaultOutputBase;
  // The capture thread owns the record until the incident ends, when the
  // counters may have advanced after we released the faulting thread.
  mutable std::filesystem::path secondFaultRecord;
  mutable LONG secondFaultRecordedResumed = 0;
  mutable LONG secondFaultRecordedDropped = 0;
  mutable bool secondFaultRetentionPending = false;
};

// ADR-0032: opens the plugin's "unhandled" event and makes the "dump settled"
// event, whichever is missing. True when both are there. TryAttachCrashEvent
// calls it once the crash event is open; call it again to retry.
bool TryAttachUnhandledSignal(AttachedProcess& proc);
// Whether trying again can help: the crash event is open, one of the two is
// missing, and not because the plugin has no such event.
bool ShouldRetryUnhandledSignalAttach(const AttachedProcess& proc) noexcept;
// ADR-0036: makes the event whose presence tells the plugin that this helper
// handles second faults. False, with the feature off, when it cannot.
bool StartSecondFaultSignal(AttachedProcess& proc, const std::filesystem::path& outBase) noexcept;
void StopSecondFaultSignal(const AttachedProcess& proc) noexcept;

bool AttachByPid(std::uint32_t pid, AttachedProcess& out, std::wstring* err);
bool FindAndAttach(AttachedProcess& out, std::wstring* err);
bool TryAttachCrashEvent(AttachedProcess& proc, std::wstring* err);
void Detach(AttachedProcess& p);

}  // namespace skydiag::helper
