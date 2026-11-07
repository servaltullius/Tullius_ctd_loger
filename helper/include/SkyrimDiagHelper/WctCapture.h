#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace skydiag::helper {

// The process's threads from one Toolhelp snapshot.
struct ProcessThreads
{
  // The thread ids walked, all of them when complete.
  std::vector<std::uint32_t> ids;
  // The walk ran to its end (ERROR_NO_MORE_FILES). False when the snapshot
  // failed (ids empty) or the walk stopped short (ids partial).
  bool complete = false;
};

ProcessThreads EnumerateProcessThreads(std::uint32_t pid);

bool CaptureWct(
  std::uint32_t pid,
  const volatile std::uint32_t* captureStateFlags,
  nlohmann::json& out,
  std::wstring* err);

// Whether a CaptureWct result has a thread whose wait chain is a cycle (a deadlock).
bool WctHasCycle(const nlohmann::json& wct);

}  // namespace skydiag::helper
