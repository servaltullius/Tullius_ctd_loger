#pragma once

#include <Windows.h>

#include <cstddef>
#include <string>

#include "SkyrimDiagHelper/Config.h"
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
  DumpMode dumpMode,
  std::wstring* err);

}  // namespace skydiag::helper
