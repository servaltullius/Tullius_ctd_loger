#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace skydiag::helper {

// The ids of the process's threads from one Toolhelp snapshot; nullopt when
// the snapshot fails or its walk stops short.
std::optional<std::vector<std::uint32_t>> EnumerateProcessThreads(std::uint32_t pid);

bool CaptureWct(
  std::uint32_t pid,
  const volatile std::uint32_t* captureStateFlags,
  nlohmann::json& out,
  std::wstring* err);

// Whether a CaptureWct result has a thread whose wait chain is a cycle (a deadlock).
bool WctHasCycle(const nlohmann::json& wct);

}  // namespace skydiag::helper
