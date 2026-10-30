#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/DumpProfile.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper {

// ADR-0023: dumps that write only the game executable's code whole get the
// code their threads' RIPs and stack values point at instead, and the
// headers, exports and unwind data of the game and of the modules those
// point into. All zero for dumps that write every module's code.
struct DumpCodeMemoryStats
{
  bool collected = false;
  // The game executable was not found, so every module's code was written.
  bool all_modules_fallback = false;
  std::uint32_t threads = 0;  // threads examined
  std::uint32_t values = 0;   // RIPs and stack values that pointed into module code
  std::uint32_t code_ranges = 0;
  std::uint64_t code_bytes = 0;
  std::uint32_t modules = 0;  // modules given headers, exports and unwind data
  std::uint64_t module_bytes = 0;
  std::uint32_t elapsed_us = 0;
  bool budget_reached = false;  // the byte or time budget stopped it early
};

// Freeze and manual dumps: the game objects that the registers and stack top
// of the threads that matter pointed at, kept in the dump (ADR-0010, addendum
// 2026-10-22). All zero for crash dumps, PSS snapshots and full-memory dumps,
// except `code`.
struct DumpObjectMemoryStats
{
  std::uint32_t threads_selected = 0;
  std::uint32_t threads_read = 0;  // selected threads whose context the writer reported and that were examined
  std::uint32_t ranges = 0;
  std::uint64_t bytes = 0;
  std::uint32_t elapsed_us = 0;
  bool budget_reached = false;  // the byte or time budget stopped it early

  // ADR-0021: windows of the game executable's writable data around the
  // values of game threads waiting in a delay (where engine locks live).
  bool lock_enabled = false;        // the game's delay stub was found
  std::uint32_t lock_threads = 0;   // game threads found waiting in a delay
  std::uint32_t lock_pointers = 0;  // their values that pointed into writable data
  std::uint32_t lock_ranges = 0;    // windows kept (plus the image header)
  std::uint64_t lock_bytes = 0;
  std::uint32_t lock_elapsed_us = 0;
  bool lock_budget_reached = false;

  DumpCodeMemoryStats code{};
};

bool WriteDumpWithStreams(
  HANDLE process,
  std::uint32_t pid,
  const std::wstring& dumpPath,
  const skydiag::SharedLayout* shmSnapshot,
  std::size_t shmSnapshotBytes,
  const std::string& wctJsonUtf8,
  const std::string& pluginScanJson,
  bool isCrash,
  const DumpProfile& dumpProfile,
  bool isProcessSnapshot,
  std::wstring* err,
  DumpObjectMemoryStats* objectMemory = nullptr);

// The game's main thread, as the newest SessionStart or Heartbeat event in a
// blackbox snapshot names it; nullopt when none does.
std::optional<DWORD> InferMainThreadIdFromSnapshot(
  const skydiag::SharedLayout* snapshot,
  std::size_t snapshotBytes);

// File writes performed by the dump callback under SKYDIAG_DUMP_IO_TRACE=1
// since the process started; lets tests prove the traced path ran.
std::uint64_t TracedDumpWriteCount() noexcept;

// Crash dumps whose exception context went through the XSTATE-sized copy
// (not the bare-CONTEXT fallback) since the process started.
std::uint64_t XStateSizedContextCount() noexcept;

// Bytes of crash-object memory (ADR-0010) added to crash dumps since the
// process started.
std::uint64_t CrashObjectMemoryBytes() noexcept;

// Bytes of freeze and manual object memory (DumpObjectMemoryStats) added to
// dumps since the process started.
std::uint64_t HangObjectMemoryBytes() noexcept;

// Bytes of Windows-module headers, exports and unwind data (ADR-0011) added
// to dumps since the process started.
std::uint64_t ModuleUnwindMemoryBytes() noexcept;

}  // namespace skydiag::helper
