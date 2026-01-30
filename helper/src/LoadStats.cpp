#include "SkyrimDiagHelper/LoadStats.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

#include "SkyrimDiagHelper/Config.h"

namespace skydiag::helper {
namespace {

std::uint32_t Percentile(const std::vector<std::uint32_t>& v, double p)
{
  if (v.empty()) {
    return 0;
  }
  if (p <= 0.0) {
    return *std::min_element(v.begin(), v.end());
  }
  if (p >= 1.0) {
    return *std::max_element(v.begin(), v.end());
  }

  std::vector<std::uint32_t> sorted = v;
  std::sort(sorted.begin(), sorted.end());

  const double idx = (static_cast<double>(sorted.size() - 1) * p);
  const auto i = static_cast<std::size_t>(idx + 0.5);  // nearest-rank-ish for small N
  return sorted[std::min(i, sorted.size() - 1)];
}

}  // namespace

bool LoadStats::LoadFromFile(const std::filesystem::path& path)
{
  loadingSeconds_.clear();

  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) {
    return false;
  }

  nlohmann::json j;
  try {
    f >> j;
  } catch (...) {
    return false;
  }

  if (!j.is_object()) {
    return false;
  }

  const auto it = j.find("loadingSeconds");
  if (it == j.end() || !it->is_array()) {
    return false;
  }

  for (const auto& el : *it) {
    if (!el.is_number_unsigned()) {
      continue;
    }
    const auto s = el.get<std::uint32_t>();
    if (s == 0 || s > 60u * 60u) {  // cap at 1 hour to avoid junk
      continue;
    }
    loadingSeconds_.push_back(s);
  }

  if (loadingSeconds_.size() > 50) {
    loadingSeconds_.erase(loadingSeconds_.begin(), loadingSeconds_.end() - 50);
  }

  return !loadingSeconds_.empty();
}

bool LoadStats::SaveToFile(const std::filesystem::path& path) const
{
  nlohmann::json j;
  j["version"] = 1;
  j["loadingSeconds"] = loadingSeconds_;

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f.is_open()) {
    return false;
  }
  f << j.dump(2);
  return true;
}

void LoadStats::AddLoadingSampleSeconds(std::uint32_t seconds)
{
  if (seconds == 0) {
    return;
  }

  // Keep the most recent N samples.
  constexpr std::size_t kMaxSamples = 10;
  loadingSeconds_.push_back(seconds);
  if (loadingSeconds_.size() > kMaxSamples) {
    loadingSeconds_.erase(loadingSeconds_.begin(), loadingSeconds_.end() - kMaxSamples);
  }
}

std::uint32_t LoadStats::SuggestedLoadingThresholdSec(const HelperConfig& config) const
{
  if (loadingSeconds_.empty()) {
    return config.hangThresholdLoadingSec;
  }

  // Heuristic:
  // threshold = p90(loadSeconds) + max(minExtraSec, p90/2)
  // - Fast modpack (p90 ~ 30s) => ~150s (2.5m)
  // - Heavy modpack (p90 ~ 600s) => ~900s (15m)
  const std::uint32_t p90 = Percentile(loadingSeconds_, 0.90);
  const std::uint32_t extra = std::max(config.adaptiveLoadingMinExtraSec, p90 / 2u);
  std::uint32_t threshold = p90 + extra;

  threshold = std::max(threshold, config.adaptiveLoadingMinSec);

  if (config.adaptiveLoadingMaxSec != 0) {
    threshold = std::min(threshold, config.adaptiveLoadingMaxSec);
  }

  return threshold;
}

}  // namespace skydiag::helper
