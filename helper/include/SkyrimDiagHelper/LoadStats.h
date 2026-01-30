#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace skydiag::helper {

struct HelperConfig;

class LoadStats
{
public:
  bool LoadFromFile(const std::filesystem::path& path);
  bool SaveToFile(const std::filesystem::path& path) const;

  void AddLoadingSampleSeconds(std::uint32_t seconds);
  bool HasSamples() const noexcept { return !loadingSeconds_.empty(); }

  // Returns a suggested "loading hang" threshold in seconds.
  // Falls back to config.hangThresholdLoadingSec when no samples exist.
  std::uint32_t SuggestedLoadingThresholdSec(const HelperConfig& config) const;

private:
  std::vector<std::uint32_t> loadingSeconds_;
};

}  // namespace skydiag::helper

