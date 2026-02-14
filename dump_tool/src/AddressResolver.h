#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace skydiag::dump_tool {

class AddressResolver
{
public:
  bool LoadFromJson(const std::filesystem::path& jsonPath, const std::string& gameVersion);
  std::optional<std::string> Resolve(std::uint64_t offset) const;
  const std::unordered_map<std::uint64_t, std::string>& Functions() const { return m_functions; }
  std::size_t Size() const { return m_functions.size(); }

private:
  std::unordered_map<std::uint64_t, std::string> m_functions;
};

}  // namespace skydiag::dump_tool
