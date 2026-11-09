#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "AddressLibrary.h"

namespace skydiag::dump_tool {

class AddressResolver
{
public:
  enum class LoadStatus
  {
    kOk,
    kFileOpenFailed,
    kInvalidJson,
    kMissingGameVersion,
    kEmptyEntries,
  };

  bool LoadFromJson(
    const std::filesystem::path& jsonPath,
    const std::string& gameVersion,
    LoadStatus* outStatus = nullptr);
  // Name of the SkyrimSE.exe function that starts at this offset (RVA), if
  // known. Exact match only.
  std::optional<std::string> Resolve(std::uint64_t functionStart) const;
  const std::unordered_map<std::uint64_t, std::string>& Functions() const { return m_functions; }
  std::size_t Size() const { return m_functions.size(); }

  // ADR-0027: the Address Library carried in the dump, for IDs of game
  // functions; names stay optional.
  void SetAddressLibrary(AddressLibrary library) { m_addressLibrary = std::move(library); }
  // The Address Library ID of the SkyrimSE.exe function that starts at this
  // offset (RVA). Exact match only.
  std::optional<std::uint64_t> ResolveId(std::uint64_t functionStart) const
  {
    return m_addressLibrary.IdAt(functionStart);
  }
  bool HasAddressLibrary() const { return m_addressLibrary.Size() != 0; }

private:
  std::unordered_map<std::uint64_t, std::string> m_functions;
  AddressLibrary m_addressLibrary;
};

}  // namespace skydiag::dump_tool
