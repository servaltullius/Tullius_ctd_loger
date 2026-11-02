#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace skydiag::helper {

bool CaptureWct(
  std::uint32_t pid,
  const volatile std::uint32_t* captureStateFlags,
  nlohmann::json& out,
  std::wstring* err);

// Whether a CaptureWct result has a thread whose wait chain is a cycle (a deadlock).
bool WctHasCycle(const nlohmann::json& wct);

}  // namespace skydiag::helper
