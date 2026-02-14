#include "AnalyzerInternals.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <unordered_map>

namespace skydiag::dump_tool::internal {
namespace {

using skydiag::dump_tool::minidump::FindModuleIndexForAddress;
using skydiag::dump_tool::minidump::GetThreadStackBytes;
using skydiag::dump_tool::minidump::LoadThreads;
using skydiag::dump_tool::minidump::ModuleInfo;
using skydiag::dump_tool::minidump::ReadThreadContextWin64;
using skydiag::dump_tool::minidump::ThreadRecord;
using skydiag::dump_tool::minidump::WideLower;

std::wstring ConfidenceText(i18n::Language lang, i18n::ConfidenceLevel level)
{
  return std::wstring(i18n::ConfidenceLabel(lang, level));
}

i18n::ConfidenceLevel ConfidenceForTopSuspectLevel(std::uint32_t topScore, std::uint32_t secondScore)
{
  if (topScore >= 256u || (topScore >= 96u && topScore >= (secondScore * 2u))) {
    return i18n::ConfidenceLevel::kHigh;
  }
  if (topScore >= 40u) {
    return i18n::ConfidenceLevel::kMedium;
  }
  return i18n::ConfidenceLevel::kLow;
}

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

  constexpr std::size_t kMaxScanBytes = 96 * 1024;
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
      scoreByModule[*mi] += StackScanSlotWeight(slotIndex);
    }
  }

  struct Row
  {
    std::size_t modIndex = 0;
    std::uint32_t score = 0;
  };
  std::vector<Row> rows;
  rows.reserve(scoreByModule.size());
  for (const auto& [idx, score] : scoreByModule) {
    const auto& m = modules[idx];
    if (m.is_systemish || m.is_game_exe) {
      continue;
    }
    rows.push_back(Row{ idx, score });
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
      const bool topIsCrashLogger = (topLower == L"crashloggersse.dll");
      const bool nearTie = (fallbackIt->score + 8u) >= rows[0].score;
      if (topIsCrashLogger || nearTie) {
        std::iter_swap(rows.begin(), fallbackIt);
        promotedHookTop = true;
      }
    }
  }

  const std::uint32_t topScore = rows[0].score;
  const std::uint32_t secondScore = (rows.size() > 1) ? rows[1].score : 0;
  auto confTop = ConfidenceForTopSuspectLevel(topScore, secondScore);
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
      ? (L"Observed " + std::to_wstring(row.score) + L" hit(s) in stack scan")
      : (L"스택 스캔에서 " + std::to_wstring(row.score) + L"회 관측");
    if (i == 0 && promotedHookTop) {
      si.reason += en
        ? L" (primary candidate promoted over hook framework hit owner)"
        : L" (훅 프레임워크 히트 소유자보다 우선 후보로 승격)";
    }
    out.push_back(std::move(si));
  }

  return out;
}

}  // namespace skydiag::dump_tool::internal
