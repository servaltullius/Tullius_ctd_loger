#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace skydiag::helper {

bool EnableDebugPrivilege();
bool CaptureWct(std::uint32_t pid, nlohmann::json& out, std::wstring* err);

}  // namespace skydiag::helper

