#include "AnalyzerInternals.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace skydiag::dump_tool::internal::stackwalk {
namespace {

using skydiag::dump_tool::minidump::FindModuleIndexForAddress;
using skydiag::dump_tool::minidump::IsSkseModule;
using skydiag::dump_tool::minidump::ModuleInfo;
using skydiag::dump_tool::minidump::WideLower;
using skydiag::dump_tool::i18n::ConfidenceText;

// Callstack frame weights: frames closer to the crash point are weighted higher
// to reflect their higher likelihood of being the actual crash cause.
constexpr std::uint32_t kWeightDepth0      = 16;  // crash frame itself
constexpr std::uint32_t kWeightDepth1      = 12;  // immediate caller
constexpr std::uint32_t kWeightDepth2      = 8;   // 2nd caller
constexpr std::uint32_t kWeightDepth3to5   = 4;   // near callers
constexpr std::uint32_t kWeightDepth6to10  = 2;   // mid-range callers
constexpr std::uint32_t kWeightDeepFrame   = 1;   // deep callers (depth > 10)

// Confidence thresholds: empirically tuned against real Skyrim crash dumps.
// "High" requires a near-top suspect with a clear margin over the runner-up.
constexpr std::size_t   kHighConfMaxDepth     = 2;
constexpr std::uint32_t kHighConfMinScore     = 24;
constexpr std::uint32_t kHighConfMinMargin    = 12;
constexpr std::size_t   kMedConfMaxDepth      = 6;
constexpr std::uint32_t kMedConfMinScore      = 12;
constexpr std::uint32_t kMedConfMinMargin     = 6;

// Score proximity threshold for hook-framework demotion heuristic.
constexpr std::uint32_t kHookFrameworkNearTieThreshold = 4;

std::uint32_t CallstackFrameWeight(std::size_t depth)
{
  if (depth == 0) return kWeightDepth0;
  if (depth == 1) return kWeightDepth1;
  if (depth == 2) return kWeightDepth2;
  if (depth <= 5) return kWeightDepth3to5;
  if (depth <= 10) return kWeightDepth6to10;
  return kWeightDeepFrame;
}

i18n::ConfidenceLevel ConfidenceForTopSuspectCallstackLevel(std::uint32_t topScore, std::uint32_t secondScore, std::size_t firstDepth)
{
  if (firstDepth <= kHighConfMaxDepth && (topScore >= kHighConfMinScore || topScore >= (secondScore + kHighConfMinMargin))) {
    return i18n::ConfidenceLevel::kHigh;
  }
  if (firstDepth <= kMedConfMaxDepth && (topScore >= kMedConfMinScore || topScore >= (secondScore + kMedConfMinMargin))) {
    return i18n::ConfidenceLevel::kMedium;
  }
  return i18n::ConfidenceLevel::kLow;
}

}  // namespace

std::vector<SuspectItem> ComputeCallstackSuspectsFromAddrs(
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  i18n::Language lang)
{
  std::vector<SuspectItem> out;
  if (modules.empty() || pcs.empty()) {
    return out;
  }

  struct Row
  {
    std::size_t modIndex = 0;
    std::uint32_t score = 0;
    std::size_t firstDepth = 0;
  };
  std::unordered_map<std::size_t, Row> byModule;

  for (std::size_t i = 0; i < pcs.size(); i++) {
    auto mi = FindModuleIndexForAddress(modules, pcs[i]);
    if (!mi) {
      continue;
    }
    const auto& m = modules[*mi];
    if (m.is_systemish || m.is_game_exe) {
      continue;
    }
    const std::uint32_t w = CallstackFrameWeight(i);
    auto it = byModule.find(*mi);
    if (it == byModule.end()) {
      Row r{};
      r.modIndex = *mi;
      r.score = w;
      r.firstDepth = i;
      byModule.emplace(*mi, r);
    } else {
      it->second.score += w;
      it->second.firstDepth = std::min<std::size_t>(it->second.firstDepth, i);
    }
  }

  if (byModule.empty()) {
    return out;
  }

  std::vector<Row> rows;
  rows.reserve(byModule.size());
  for (const auto& [_, row] : byModule) {
    rows.push_back(row);
  }

  std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    if (a.firstDepth != b.firstDepth) {
      return a.firstDepth < b.firstDepth;
    }
    const auto an = WideLower(modules[a.modIndex].filename);
    const auto bn = WideLower(modules[b.modIndex].filename);
    return an < bn;
  });

  // If the top frame owner is a hook framework (especially CrashLoggerSSE), prefer a
  // non-hook candidate when one is available to reduce common victim-as-culprit false positives.
  bool promotedHookTop = false;
  if (rows.size() > 1 && modules[rows[0].modIndex].is_known_hook_framework) {
    const auto fallbackIt = std::find_if(rows.begin() + 1, rows.end(), [&](const Row& r) {
      return !modules[r.modIndex].is_known_hook_framework;
    });
    if (fallbackIt != rows.end()) {
      const std::wstring topLower = WideLower(modules[rows[0].modIndex].filename);
      const bool topIsCrashLogger = (topLower == L"crashloggersse.dll" || topLower == L"crashlogger.dll");
      const bool topIsSkseRuntime = IsSkseModule(topLower);
      const bool topIsMo2Vfs = (topLower == L"usvfs_x64.dll" || topLower == L"uvsfs64.dll");
      const bool nearTie = (fallbackIt->score + kHookFrameworkNearTieThreshold) >= rows[0].score;
      if (topIsCrashLogger || topIsSkseRuntime || topIsMo2Vfs || nearTie) {
        std::iter_swap(rows.begin(), fallbackIt);
        promotedHookTop = true;
      }
    }
  }

  const std::uint32_t topScore = rows[0].score;
  const std::uint32_t secondScore = (rows.size() > 1) ? rows[1].score : 0;
  auto confTop = ConfidenceForTopSuspectCallstackLevel(topScore, secondScore, rows[0].firstDepth);

  // Known hook framework modules are often victims of other mods' memory corruption.
  // Downgrade confidence by one level when the top suspect is a hook framework.
  if (modules[rows[0].modIndex].is_known_hook_framework) {
    if (confTop == i18n::ConfidenceLevel::kHigh) {
      confTop = i18n::ConfidenceLevel::kMedium;
    } else if (confTop == i18n::ConfidenceLevel::kMedium) {
      confTop = i18n::ConfidenceLevel::kLow;
    }
  }

  const bool en = (lang == i18n::Language::kEnglish);

  const std::size_t n = std::min<std::size_t>(rows.size(), 5);
  out.reserve(n);
  for (std::size_t i = 0; i < n; i++) {
    const auto& row = rows[i];
    const auto& m = modules[row.modIndex];

    SuspectItem si{};
    si.confidence_level = (i == 0) ? confTop : i18n::ConfidenceLevel::kMedium;
    si.confidence = ConfidenceText(lang, si.confidence_level);
    si.module_filename = m.filename;
    si.module_path = m.path;
    si.inferred_mod_name = m.inferred_mod_name;
    si.score = row.score;
    si.reason = en
      ? (L"Callstack weight=" + std::to_wstring(row.score) + L", first depth=" + std::to_wstring(row.firstDepth))
      : (L"콜스택 상위 프레임에서 가중치=" + std::to_wstring(row.score) + L", 최초 깊이=" + std::to_wstring(row.firstDepth));
    if (i == 0 && promotedHookTop) {
      si.reason += en
        ? L" (primary candidate promoted over hook framework frame owner)"
        : L" (훅 프레임워크 프레임 소유자보다 우선 후보로 승격)";
    }
    out.push_back(std::move(si));
  }

  return out;
}

}  // namespace skydiag::dump_tool::internal::stackwalk
