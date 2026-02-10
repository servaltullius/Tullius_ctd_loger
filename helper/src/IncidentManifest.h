#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace skydiag::helper {
struct HelperConfig;
}

namespace skydiag::helper::internal {

nlohmann::json MakeIncidentManifestV1(
  std::string_view captureKind,
  std::wstring_view ts,
  DWORD pid,
  const std::filesystem::path& dumpPath,
  const std::optional<std::filesystem::path>& wctPath,
  const std::optional<std::filesystem::path>& etwPath,
  std::string_view etwStatus,
  std::uint32_t stateFlags,
  const nlohmann::json& context,
  const skydiag::helper::HelperConfig& cfg,
  bool includeConfigSnapshot);

bool TryUpdateIncidentManifestEtw(
  const std::filesystem::path& manifestPath,
  const std::filesystem::path& etwPath,
  std::string_view etwStatus,
  std::wstring* err);

}  // namespace skydiag::helper::internal

