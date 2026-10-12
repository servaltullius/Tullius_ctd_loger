#include "AnalyzerInternals.h"

#include "AddressResolver.h"
#include "AnalyzerScoringPolicy.h"
#include "AnalyzerInternalsStackwalkPriv.h"

#include <algorithm>
#include <cstring>

namespace skydiag::dump_tool::internal {
namespace {

using skydiag::dump_tool::internal::stackwalk_internal::AddDumpExportSymbols;
using skydiag::dump_tool::internal::stackwalk_internal::DumpImageData;
using skydiag::dump_tool::internal::stackwalk_internal::FindFunctionStart;
using skydiag::dump_tool::internal::stackwalk_internal::LocalImageMemory;
using skydiag::dump_tool::internal::stackwalk_internal::MinidumpMemoryView;
using skydiag::dump_tool::internal::stackwalk_internal::StackWalkAddrsForContext;
using skydiag::dump_tool::internal::stackwalk_internal::SymSession;

using skydiag::dump_tool::minidump::FindModuleIndexForAddress;
using skydiag::dump_tool::minidump::ModuleInfo;
using skydiag::dump_tool::minidump::ReadThreadContextWin64;
using skydiag::dump_tool::minidump::ThreadRecord;

// Name of the SkyrimSE.exe function containing pc, with the offset into it
// ("BSReadWriteLock::LockForRead+0xf"), or empty. Only an exact match of
// the .pdata function start counts. Also records the name in
// out.resolved_functions under the frame's module offset.
std::wstring LabelGameFrame(
  const MinidumpMemoryView& mem,
  const std::vector<ModuleInfo>& modules,
  std::uint64_t pc,
  bool returnAddress,
  const AddressResolver& gameFunctions,
  AnalysisResult& out)
{
  const auto idx = FindModuleIndexForAddress(modules, pc);
  if (!idx || !modules[*idx].is_game_exe) {
    return {};
  }
  const auto& module = modules[*idx];
  // A return address can be the first byte after its function when the call
  // was the function's last instruction.
  const std::uint64_t probe = (returnAddress && pc > module.base) ? pc - 1u : pc;
  const auto start = FindFunctionStart(mem, module.base, probe);
  if (!start) {
    return {};
  }
  const auto name = gameFunctions.Resolve(*start - module.base);
  if (!name) {
    return {};
  }
  out.resolved_functions[pc - module.base] = *name;
  wchar_t disp[32]{};
  swprintf_s(disp, L"+0x%llx", static_cast<unsigned long long>(pc - *start));
  return std::wstring(name->begin(), name->end()) + disp;
}

std::vector<std::wstring> LabelGameFrames(
  const MinidumpMemoryView& mem,
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  const AddressResolver* gameFunctions,
  AnalysisResult& out)
{
  std::vector<std::wstring> labels;
  if (!gameFunctions) {
    return labels;
  }
  labels.reserve(pcs.size());
  for (std::size_t i = 0; i < pcs.size(); ++i) {
    labels.push_back(LabelGameFrame(mem, modules, pcs[i], /*returnAddress=*/i > 0u, *gameFunctions, out));
  }
  return labels;
}

}  // namespace

namespace stackwalk {

std::vector<SuspectItem> ComputeCallstackSuspectsFromAddrs(
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  i18n::Language lang);

std::vector<CrashBucketFrame> BuildCanonicalCallstackFrames(
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  std::size_t maxFrames);

std::vector<ModalStackFrame> BuildModalProbeFrames(
  HANDLE process,
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  std::size_t maxFrames);

std::vector<std::wstring> FormatCallstackForDisplay(
  HANDLE process,
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  std::size_t maxFrames,
  std::uint32_t* outTotalFrames,
  std::uint32_t* outSymbolizedFrames,
  std::uint32_t* outSourceLineFrames,
  bool fromTop,
  const std::vector<std::wstring>* frameLabels);

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
  std::uint32_t preferredTid,
  std::uint32_t excTid,
  const std::optional<CONTEXT>& excCtx,
  const std::vector<ThreadRecord>& threads,
  i18n::Language lang,
  AnalysisResult& out,
  std::uint32_t modalProbeTid,
  std::vector<ModalStackFrame>* outModalProbeFrames,
  const std::vector<std::uint32_t>* sideTids,
  std::vector<ThreadTopFrames>* outSideFrames,
  const AddressResolver* gameFunctions)
{
  if (!dumpBase || modules.empty() || targetTids.empty() || threads.empty()) {
    return false;
  }

  MinidumpMemoryView mem;
  if (!mem.Init(dumpBase, dumpSize, &threads)) {
    return false;
  }
  // Dumps usually omit module images, so unwind data for frames in system
  // DLLs and plugins has to come from matching local files.
  const LocalImageMemory localImages(modules);
  mem.image_fallback = &localImages;

  SymSession sym(modules, out.online_symbol_source_allowed);
  out.symbol_search_path = sym.searchPath;
  out.symbol_cache_path = sym.cachePath;
  out.dbghelp_path = sym.dbghelpPath;
  out.dbghelp_version = sym.dbghelpVersion;
  out.msdia_path = sym.msdiaPath;
  out.msdia_available = sym.msdiaAvailable;
  out.symbol_cache_ready = sym.symbolCacheReady;
  out.symbol_runtime_degraded = sym.runtimeDegraded;
  out.online_symbol_source_used = sym.usedOnlineSymbolSource;
  for (const auto& diagnostic : sym.runtimeDiagnostics) {
    out.diagnostics.push_back(diagnostic);
  }
  if (!sym.ok) {
    return false;
  }
  // ADR-0011: unwind entries and export names from the dump itself, for
  // modules whose local file does not match.
  const DumpImageData dumpImages(mem, modules);
  mem.dump_images = &dumpImages;
  AddDumpExportSymbols(sym, dumpImages);

  // The faulting game function, for the evidence line, whichever thread's
  // stack is shown.
  if (gameFunctions && excCtx) {
    (void)LabelGameFrame(mem, modules, excCtx->Rip, /*returnAddress=*/false, *gameFunctions, out);
  }

  struct Candidate
  {
    std::uint32_t tid = 0;
    std::vector<std::uint64_t> pcs;
    std::vector<SuspectItem> suspects;
    std::uint32_t topScore = 0;
  };

  // Other threads' top frames, walked in the same symbol session; they do
  // not compete for the suspects (FindBlockedGameThread reads them).
  if (sideTids && outSideFrames) {
    for (const auto tid : *sideTids) {
      const auto it = std::find_if(threads.begin(), threads.end(), [&](const ThreadRecord& tr) { return tr.tid == tid; });
      CONTEXT ctx{};
      if (it == threads.end() || !ReadThreadContextWin64(dumpBase, dumpSize, *it, ctx) || ctx.Rip == 0 || ctx.Rsp == 0) {
        continue;
      }
      const auto pcs = StackWalkAddrsForContext(sym.process, mem, ctx, /*maxFrames=*/kBlockedThreadTopFrames + 4u);
      if (pcs.empty()) {
        continue;
      }
      outSideFrames->push_back(ThreadTopFrames{
        tid, stackwalk::BuildModalProbeFrames(sym.process, modules, pcs, kBlockedThreadTopFrames) });
    }
  }

  Candidate best{};
  Candidate bestAny{};
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
    if (outModalProbeFrames && modalProbeTid != 0u && tid == modalProbeTid) {
      *outModalProbeFrames = stackwalk::BuildModalProbeFrames(
        sym.process,
        modules,
        pcs,
        kModalApiMaxDepth + 16u);
      // The calls the top frames made, for ClassifyMainThreadWait. Only the
      // dump's own memory: a local SkyrimSE.exe has its code encrypted on disk.
      MinidumpMemoryView dumpOnly = mem;
      dumpOnly.image_fallback = nullptr;
      constexpr std::size_t kCodeBeforeBytes = 0x60;
      constexpr std::size_t kCodeFrames = 12;
      for (std::size_t i = 0; i < outModalProbeFrames->size() && i < kCodeFrames; ++i) {
        auto& frame = (*outModalProbeFrames)[i];
        if (!frame.has_module || frame.is_system || frame.pc < kCodeBeforeBytes) {
          continue;
        }
        std::vector<std::uint8_t> bytes(kCodeBeforeBytes);
        std::size_t got = 0;
        if (dumpOnly.Read(frame.pc - kCodeBeforeBytes, bytes.data(), bytes.size(), got) && got == bytes.size()) {
          frame.code_before = std::move(bytes);
        }
      }
    }

    if (policy::ShouldSelectStackwalkCandidate(
          !bestAny.pcs.empty(),
          bestAny.tid,
          static_cast<std::uint32_t>(bestAny.pcs.size()),
          !pcs.empty(),
          tid,
          static_cast<std::uint32_t>(pcs.size()),
          preferredTid)) {
      bestAny.tid = tid;
      bestAny.pcs = pcs;
    }

    auto suspects = stackwalk::ComputeCallstackSuspectsFromAddrs(modules, pcs, lang);
    if (suspects.empty()) {
      continue;
    }

    const std::uint32_t topScore = suspects[0].score;
    if (policy::ShouldSelectStackwalkCandidate(
          !best.suspects.empty(),
          best.tid,
          best.topScore,
          !suspects.empty(),
          tid,
          topScore,
          preferredTid)) {
      best.tid = tid;
      best.pcs = std::move(pcs);
      best.suspects = std::move(suspects);
      best.topScore = topScore;
    }
  }

  if (best.suspects.empty()) {
    if (!bestAny.pcs.empty()) {
      out.stackwalk_primary_tid = bestAny.tid;
      out.stackwalk_primary_bucket_frames = stackwalk::BuildCanonicalCallstackFrames(
        modules,
        bestAny.pcs,
        /*maxFrames=*/12);
      const bool fromTop = modalProbeTid != 0u && bestAny.tid == modalProbeTid;
      const auto labels = LabelGameFrames(mem, modules, bestAny.pcs, gameFunctions, out);
      out.stackwalk_primary_frames = stackwalk::FormatCallstackForDisplay(
        sym.process,
        modules,
        bestAny.pcs,
        /*maxFrames=*/fromTop ? 16u : 12u,
        &out.stackwalk_total_frames,
        &out.stackwalk_symbolized_frames,
        &out.stackwalk_source_line_frames,
        fromTop,
        &labels);
    }
    return false;
  }

  out.suspects_from_stackwalk = true;
  out.suspects = std::move(best.suspects);
  out.stackwalk_primary_tid = best.tid;
  out.stackwalk_primary_bucket_frames = stackwalk::BuildCanonicalCallstackFrames(
    modules,
    best.pcs,
    /*maxFrames=*/12);
  const bool fromTop = modalProbeTid != 0u && best.tid == modalProbeTid;
  const auto labels = LabelGameFrames(mem, modules, best.pcs, gameFunctions, out);
  out.stackwalk_primary_frames = stackwalk::FormatCallstackForDisplay(
    sym.process,
    modules,
    best.pcs,
    /*maxFrames=*/fromTop ? 16u : 12u,
    &out.stackwalk_total_frames,
    &out.stackwalk_symbolized_frames,
    &out.stackwalk_source_line_frames,
    fromTop,
    &labels);
  return true;
}

}  // namespace skydiag::dump_tool::internal
