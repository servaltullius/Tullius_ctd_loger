#include "AnalyzerInternals.h"
#include "AnalyzerScoringPolicy.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <unordered_map>

namespace skydiag::dump_tool::internal {
namespace {

using skydiag::dump_tool::minidump::FindModuleIndexForAddress;
using skydiag::dump_tool::minidump::GetThreadStackBytes;
using skydiag::dump_tool::minidump::IsSkseModule;
using skydiag::dump_tool::minidump::LoadThreads;
using skydiag::dump_tool::minidump::ModuleInfo;
using skydiag::dump_tool::minidump::ReadThreadContextWin64;
using skydiag::dump_tool::minidump::ThreadRecord;
using skydiag::dump_tool::minidump::WideLower;
using skydiag::dump_tool::i18n::ConfidenceText;

constexpr std::uint32_t kHookFrameworkNearTieThreshold = 8u;
constexpr std::uint32_t kPassiveHookFallbackMinScore = 8u;
constexpr std::uint32_t kOtherHookFallbackMinScore = 16u;

std::uint32_t StackScanSlotWeight(std::size_t slotIndex)
{
  if (slotIndex < 4) {
    return 8;
  }
  if (slotIndex < 16) {
    return 4;
  }
  if (slotIndex < 64) {
    return 2;
  }
  return 1;
}

}  // namespace

std::vector<SuspectItem> ComputeStackScanSuspects(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint32_t>& targetTids,
  std::uint32_t exceptionTid,
  i18n::Language lang)
{
  std::vector<SuspectItem> out;
  if (!dumpBase || modules.empty() || targetTids.empty()) {
    return out;
  }

  const auto threads = LoadThreads(dumpBase, dumpSize);
  if (threads.empty()) {
    return out;
  }

  std::unordered_map<std::size_t, std::uint32_t> scoreByModule;
  std::unordered_map<std::size_t, std::uint32_t> exceptionScoreByModule;

  constexpr std::size_t kMaxScanBytes = std::size_t{96} * 1024u;
  for (const auto tid : targetTids) {
    const auto it = std::find_if(threads.begin(), threads.end(), [&](const ThreadRecord& tr) { return tr.tid == tid; });
    if (it == threads.end()) {
      continue;
    }

    CONTEXT ctx{};
    if (!ReadThreadContextWin64(dumpBase, dumpSize, *it, ctx)) {
      continue;
    }
    const std::uint64_t sp = ctx.Rsp;

    const std::uint8_t* stackBytes = nullptr;
    std::size_t stackSize = 0;
    std::uint64_t stackBase = 0;
    if (!GetThreadStackBytes(dumpBase, dumpSize, *it, stackBytes, stackSize, stackBase)) {
      continue;
    }

    std::size_t startOff = 0;
    if (sp >= stackBase && sp < stackBase + static_cast<std::uint64_t>(stackSize)) {
      startOff = static_cast<std::size_t>(sp - stackBase);
    }
    const std::size_t endOff = std::min<std::size_t>(stackSize, startOff + kMaxScanBytes);

    for (std::size_t off = startOff; off + sizeof(std::uint64_t) <= endOff; off += sizeof(std::uint64_t)) {
      std::uint64_t val = 0;
      std::memcpy(&val, stackBytes + off, sizeof(val));
      auto mi = FindModuleIndexForAddress(modules, val);
      if (!mi) {
        continue;
      }
      const std::size_t slotIndex = (off - startOff) / sizeof(std::uint64_t);
      const auto weight = StackScanSlotWeight(slotIndex);
      scoreByModule[*mi] += weight;
      if (exceptionTid != 0u && tid == exceptionTid) {
        exceptionScoreByModule[*mi] += weight;
      }
    }
  }

  struct Row
  {
    std::size_t modIndex = 0;
    std::uint32_t score = 0;
  };
  const auto buildActionableRows = [&](const auto& scores) {
    std::vector<Row> result;
    result.reserve(scores.size());
    for (const auto& [idx, score] : scores) {
      const auto& m = modules[idx];
      if (m.is_systemish || m.is_game_exe) {
        continue;
      }
      result.push_back(Row{ idx, score });
    }
    return result;
  };

  // As with the real stackwalk path, a usable exception-thread result is the
  // authoritative CTD stack. Auxiliary/main-thread pointer density must not
  // replace it merely because symbols were unavailable.
  std::vector<Row> rows = buildActionableRows(exceptionScoreByModule);
  const bool usedExceptionThreadScores = !rows.empty();
  if (rows.empty()) {
    rows = buildActionableRows(scoreByModule);
  }

  std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    const auto an = WideLower(modules[a.modIndex].filename);
    const auto bn = WideLower(modules[b.modIndex].filename);
    return an < bn;
  });

  if (rows.empty()) {
    return out;
  }

  // Same policy as callstack scoring: when the top hit is a hook framework
  // (especially CrashLoggerSSE), prefer a non-hook candidate if available.
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
      const bool topIsPassiveHook = topIsCrashLogger || topIsSkseRuntime || topIsMo2Vfs;
      const std::uint32_t minimumFallbackScore = topIsPassiveHook
        ? kPassiveHookFallbackMinScore
        : kOtherHookFallbackMinScore;
      const bool promotionEligible = policy::ShouldPromoteHookFallback(
        rows[0].score,
        fallbackIt->score,
        minimumFallbackScore,
        kHookFrameworkNearTieThreshold);
      if (promotionEligible) {
        std::iter_swap(rows.begin(), fallbackIt);
        promotedHookTop = true;
      }
    }
  }

  const bool en = (lang == i18n::Language::kEnglish);

  const std::size_t n = std::min<std::size_t>(rows.size(), 5);
  out.reserve(n);
  for (std::size_t i = 0; i < n; i++) {
    const auto& row = rows[i];
    const auto& m = modules[row.modIndex];

    SuspectItem si{};
    si.confidence_level = i18n::ConfidenceLevel::kLow;
    si.confidence = ConfidenceText(lang, si.confidence_level);
    si.module_filename = m.filename;
    si.module_path = m.path;
    si.inferred_mod_name = m.inferred_mod_name;
    si.score = row.score;
    si.reason = en
      ? (std::wstring(usedExceptionThreadScores
           ? L"Exception-thread raw stack scan weighted pointer score="
           : L"Raw stack scan weighted pointer score=") +
         std::to_wstring(row.score) +
         L"; raw stack slots may contain stale or non-return-address pointers such as vtable/callback addresses")
      : (std::wstring(usedExceptionThreadScores ? L"예외 스레드 원시 스택 스캔" : L"원시 스택 스캔") +
         L"의 가중 포인터 점수=" + std::to_wstring(row.score) +
         L"; 원시 스택 슬롯에는 오래되었거나 반환 주소가 아닌 포인터(vtable/callback 주소 등)가 포함될 수 있음");
    if (i == 0 && promotedHookTop) {
      si.reason += en
        ? L" (primary candidate promoted over hook framework pointer-score owner)"
        : L" (훅 프레임워크 포인터 점수 소유자보다 우선 후보로 승격)";
    }
    out.push_back(std::move(si));
  }

  return out;
}

std::vector<std::uint32_t> FindThreadsWithNearStackModule(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<ModuleInfo>& modules,
  std::wstring_view moduleFilename,
  std::size_t maxSlots)
{
  std::vector<std::uint32_t> matchingTids;
  if (!dumpBase || moduleFilename.empty() || maxSlots == 0u) {
    return matchingTids;
  }

  const auto moduleIt = std::find_if(modules.begin(), modules.end(), [&](const ModuleInfo& module) {
    return WideLower(module.filename) == WideLower(moduleFilename);
  });
  if (moduleIt == modules.end()) {
    return matchingTids;
  }

  const auto threads = LoadThreads(dumpBase, dumpSize);
  for (const auto& thread : threads) {
    CONTEXT context{};
    if (!ReadThreadContextWin64(dumpBase, dumpSize, thread, context)) {
      continue;
    }

    const std::uint8_t* stackBytes = nullptr;
    std::size_t stackSize = 0;
    std::uint64_t stackBase = 0;
    if (!GetThreadStackBytes(dumpBase, dumpSize, thread, stackBytes, stackSize, stackBase)) {
      continue;
    }

    std::size_t startOffset = 0;
    if (context.Rsp >= stackBase && context.Rsp < stackBase + static_cast<std::uint64_t>(stackSize)) {
      startOffset = static_cast<std::size_t>(context.Rsp - stackBase);
    }
    const std::size_t scanBytes = std::min<std::size_t>(
      stackSize - startOffset,
      maxSlots * sizeof(std::uint64_t));
    bool matched = false;
    for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= scanBytes; offset += sizeof(std::uint64_t)) {
      std::uint64_t value = 0;
      std::memcpy(&value, stackBytes + startOffset + offset, sizeof(value));
      if (value >= moduleIt->base && value < moduleIt->end) {
        matched = true;
        break;
      }
    }
    if (matched) {
      matchingTids.push_back(thread.tid);
    }
  }
  return matchingTids;
}

}  // namespace skydiag::dump_tool::internal
