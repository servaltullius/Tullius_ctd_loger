#include "AnalyzerInternals.h"

#include "AnalyzerInternalsStackwalkPriv.h"

#include <algorithm>
#include <cstring>

namespace skydiag::dump_tool::internal {
namespace {

using skydiag::dump_tool::internal::stackwalk_internal::MinidumpMemoryView;
using skydiag::dump_tool::internal::stackwalk_internal::StackWalkAddrsForContext;
using skydiag::dump_tool::internal::stackwalk_internal::SymSession;

using skydiag::dump_tool::minidump::ModuleInfo;
using skydiag::dump_tool::minidump::ReadThreadContextWin64;
using skydiag::dump_tool::minidump::ThreadRecord;
using skydiag::dump_tool::minidump::WideLower;

std::wstring ConfidenceText(i18n::Language lang, i18n::ConfidenceLevel level)
{
  return std::wstring(i18n::ConfidenceLabel(lang, level));
}

}  // namespace

namespace stackwalk {

std::vector<SuspectItem> ComputeCallstackSuspectsFromAddrs(
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  i18n::Language lang);

std::vector<std::wstring> FormatCallstackForDisplay(
  HANDLE process,
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  std::size_t maxFrames,
  std::uint32_t* outTotalFrames,
  std::uint32_t* outSymbolizedFrames,
  std::uint32_t* outSourceLineFrames);

}  // namespace stackwalk

bool TryReadContextFromLocation(void* dumpBase, std::uint64_t dumpSize, const MINIDUMP_LOCATION_DESCRIPTOR& loc, CONTEXT& out)
{
  if (!dumpBase) {
    return false;
  }
  const std::uint64_t rva = static_cast<std::uint64_t>(loc.Rva);
  const std::uint64_t sz = static_cast<std::uint64_t>(loc.DataSize);
  if (rva == 0 || sz == 0 || rva > dumpSize || sz > (dumpSize - rva)) {
    return false;
  }

  const auto* base = static_cast<const std::uint8_t*>(dumpBase);
  const std::size_t copyN = static_cast<std::size_t>(std::min<std::uint64_t>(sz, sizeof(CONTEXT)));
  std::memset(&out, 0, sizeof(out));
  std::memcpy(&out, base + rva, copyN);
  return true;
}

bool TryComputeStackwalkSuspects(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint32_t>& targetTids,
  std::uint32_t excTid,
  const std::optional<CONTEXT>& excCtx,
  const std::vector<ThreadRecord>& threads,
  i18n::Language lang,
  AnalysisResult& out)
{
  if (!dumpBase || modules.empty() || targetTids.empty() || threads.empty()) {
    return false;
  }

  MinidumpMemoryView mem;
  if (!mem.Init(dumpBase, dumpSize, &threads)) {
    return false;
  }

  SymSession sym(modules, out.online_symbol_source_allowed);
  out.symbol_search_path = sym.searchPath;
  out.symbol_cache_path = sym.cachePath;
  out.online_symbol_source_used = sym.usedOnlineSymbolSource;
  if (!sym.ok) {
    return false;
  }

  struct Candidate
  {
    std::uint32_t tid = 0;
    std::vector<std::uint64_t> pcs;
    std::vector<SuspectItem> suspects;
    std::uint32_t topScore = 0;
  };

  Candidate best{};
  Candidate bestAny{};
  const bool en = (lang == i18n::Language::kEnglish);
  for (const auto tid : targetTids) {
    CONTEXT ctx{};
    bool haveCtx = false;
    if (tid != 0 && tid == excTid && excCtx) {
      ctx = *excCtx;
      haveCtx = true;
    } else {
      const auto it = std::find_if(threads.begin(), threads.end(), [&](const ThreadRecord& tr) { return tr.tid == tid; });
      if (it != threads.end() && ReadThreadContextWin64(dumpBase, dumpSize, *it, ctx)) {
        haveCtx = true;
      }
    }
    if (!haveCtx) {
      continue;
    }
    if (ctx.Rip == 0 || ctx.Rsp == 0) {
      continue;
    }

    auto pcs = StackWalkAddrsForContext(sym.process, mem, ctx, /*maxFrames=*/64);
    if (pcs.empty()) {
      continue;
    }

    if (bestAny.pcs.size() < pcs.size()) {
      bestAny.tid = tid;
      bestAny.pcs = pcs;
    }

    auto suspects = stackwalk::ComputeCallstackSuspectsFromAddrs(modules, pcs, lang);
    if (suspects.empty()) {
      continue;
    }

    const std::uint32_t topScore = suspects[0].score;
    const bool prefer = (best.tid != excTid && tid == excTid);
    if (best.suspects.empty() || prefer || topScore > best.topScore) {
      best.tid = tid;
      best.pcs = std::move(pcs);
      best.suspects = std::move(suspects);
      best.topScore = topScore;
    }
  }

  if (best.suspects.empty()) {
    if (!bestAny.pcs.empty()) {
      out.stackwalk_primary_tid = bestAny.tid;
      out.stackwalk_primary_frames = stackwalk::FormatCallstackForDisplay(
        sym.process,
        modules,
        bestAny.pcs,
        /*maxFrames=*/12,
        &out.stackwalk_total_frames,
        &out.stackwalk_symbolized_frames,
        &out.stackwalk_source_line_frames);
    }
    return false;
  }

  // Boost confidence when Crash Logger agrees with our top module (best-effort).
  if (!out.crash_logger_top_modules.empty() && !best.suspects.empty()) {
    const auto topLower = WideLower(best.suspects[0].module_filename);
    for (const auto& m : out.crash_logger_top_modules) {
      if (WideLower(m) == topLower) {
        best.suspects[0].confidence_level = i18n::ConfidenceLevel::kHigh;
        best.suspects[0].confidence = ConfidenceText(lang, best.suspects[0].confidence_level);
        best.suspects[0].reason += en ? L" (also in Crash Logger callstack)" : L" (Crash Logger 콜스택에도 등장)";
        break;
      }
    }
  }

  // Also boost when Crash Logger provides an explicit C++ exception module that matches our top suspect.
  if (!out.crash_logger_cpp_exception_module.empty() && !best.suspects.empty()) {
    const auto topLower = WideLower(best.suspects[0].module_filename);
    if (WideLower(out.crash_logger_cpp_exception_module) == topLower) {
      best.suspects[0].confidence_level = i18n::ConfidenceLevel::kHigh;
      best.suspects[0].confidence = ConfidenceText(lang, best.suspects[0].confidence_level);
      best.suspects[0].reason += en ? L" (Crash Logger C++ exception module)" : L" (Crash Logger C++ 예외 모듈)";
    }
  }

  out.suspects_from_stackwalk = true;
  out.suspects = std::move(best.suspects);
  out.stackwalk_primary_tid = best.tid;
  out.stackwalk_primary_frames = stackwalk::FormatCallstackForDisplay(
    sym.process,
    modules,
    best.pcs,
    /*maxFrames=*/12,
    &out.stackwalk_total_frames,
    &out.stackwalk_symbolized_frames,
    &out.stackwalk_source_line_frames);
  return true;
}

}  // namespace skydiag::dump_tool::internal
