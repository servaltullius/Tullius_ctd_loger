#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/DumpProfile.h"
#include "SkyrimDiagShared.h"

namespace skydiag::helper {

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
  std::wstring* err);

// File writes performed by the dump callback under SKYDIAG_DUMP_IO_TRACE=1
// since the process started; lets tests prove the traced path ran.
std::uint64_t TracedDumpWriteCount() noexcept;

// Crash dumps whose exception context went through the XSTATE-sized copy
// (not the bare-CONTEXT fallback) since the process started.
std::uint64_t XStateSizedContextCount() noexcept;

// Bytes of crash-object memory (ADR-0010) added to crash dumps since the
// process started.
std::uint64_t CrashObjectMemoryBytes() noexcept;

}  // namespace skydiag::helper
