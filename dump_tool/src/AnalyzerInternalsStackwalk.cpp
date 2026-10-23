#include "AnalyzerInternals.h"

#include "AddressResolver.h"
#include "AnalyzerScoringPolicy.h"
#include "AnalyzerInternalsStackwalkPriv.h"
#include "EngineSpinLoop.h"
#include "ThreadEntryHooks.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace skydiag::dump_tool::internal {
namespace {

using skydiag::dump_tool::internal::stackwalk_internal::AddDumpExportSymbols;
using skydiag::dump_tool::internal::stackwalk_internal::DumpImageData;
using skydiag::dump_tool::internal::stackwalk_internal::FindFunctionStart;
using skydiag::dump_tool::internal::stackwalk_internal::LocalImageMemory;
using skydiag::dump_tool::internal::stackwalk_internal::MinidumpMemoryView;
using skydiag::dump_tool::internal::stackwalk_internal::StackWalkAddrsForContext;
using skydiag::dump_tool::internal::stackwalk_internal::StackWalkEnd;
using skydiag::dump_tool::internal::stackwalk_internal::StackWalkStop;
using skydiag::dump_tool::internal::stackwalk_internal::SymSession;

using skydiag::dump_tool::minidump::FindModuleIndexForAddress;
using skydiag::dump_tool::minidump::ModuleInfo;
using skydiag::dump_tool::minidump::ReadThreadContextWin64;
using skydiag::dump_tool::minidump::ThreadRecord;

// DbgHelp ended the walk itself (normally at the thread start), so the last
// frames are the bottom of the stack (ThreadEntryHooks.h relies on that).
bool WalkReachedThreadStart(const StackWalkEnd& end)
{
  return end.stop == StackWalkStop::kWalkEnded || end.stop == StackWalkStop::kNoProgress;
}

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
  i18n::Language lang,
  bool walkReachedThreadStart = true);

std::vector<bool> MarkThreadEntryHookPcs(
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  bool walkReachedThreadStart);

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
    StackWalkEnd end;
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

    StackWalkEnd end{};
    auto pcs = StackWalkAddrsForContext(sym.process, mem, ctx, /*maxFrames=*/64, &end);
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
      bestAny.end = end;
    }

    auto suspects = stackwalk::ComputeCallstackSuspectsFromAddrs(modules, pcs, lang, WalkReachedThreadStart(end));
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
      best.end = end;
      best.suspects = std::move(suspects);
      best.topScore = topScore;
    }
  }

  // Where the shown thread's walk was cut short, for the report: the frames
  // below the cut (often the mod that started the call) are not visible.
  const auto unloaded = minidump::LoadUnloadedModules(dumpBase, dumpSize);
  const auto recordTruncation = [&](const std::vector<std::uint64_t>& pcs, const StackWalkEnd& end) {
    const char* reason = end.stop == StackWalkStop::kOutsideModules ? "outside_modules"
                         : end.stop == StackWalkStop::kNonCode      ? "non_code_address"
                         : end.stop == StackWalkStop::kNoUnwind     ? "no_unwind_info"
                                                                    : nullptr;
    if (!reason || pcs.empty()) {
      return;
    }
    out.stackwalk_truncation = reason;
    out.stackwalk_truncation_depth = static_cast<std::uint32_t>(pcs.size());
    out.stackwalk_truncation_address = end.address;
    if (end.stop == StackWalkStop::kNoUnwind) {
      if (const auto mi = minidump::FindModuleIndexForAddress(modules, end.address)) {
        out.stackwalk_truncation_module = modules[*mi].filename;
      }
    } else if (const auto* gone = minidump::FindUnloadedModule(unloaded, end.address)) {
      out.stackwalk_truncation_unloaded_module = gone->filename;
    }
  };
  // Only when fetching the instruction itself faulted: code that ran there
  // and faulted on data is something mapped since (a trampoline, JIT code).
  const bool instructionFetchFault =
    out.exc_code == 0xC0000005u && out.exc_info.size() >= 2u && excCtx && out.exc_info[1] == excCtx->Rip;
  if (instructionFetchFault && !minidump::FindModuleIndexForAddress(modules, excCtx->Rip)) {
    if (const auto* gone = minidump::FindUnloadedModule(unloaded, excCtx->Rip)) {
      out.fault_unloaded_module = gone->filename;
    }
  }

  // Plugins that only wrap the entry of the shown thread, for the report.
  const auto recordThreadEntryHooks = [&](const std::vector<std::uint64_t>& pcs, const StackWalkEnd& end) {
    const auto marked = stackwalk::MarkThreadEntryHookPcs(modules, pcs, WalkReachedThreadStart(end));
    out.stackwalk_only_thread_entry_hook_plugins =
      std::find(marked.begin(), marked.end(), true) != marked.end();
    std::vector<std::wstring> frameModules;
    frameModules.reserve(pcs.size());
    for (const auto pc : pcs) {
      const auto mi = minidump::FindModuleIndexForAddress(modules, pc);
      frameModules.push_back(mi ? modules[*mi].filename : std::wstring{});
    }
    for (const auto& module : ThreadEntryHookOnlyModules(frameModules, marked)) {
      if (std::find(out.thread_entry_hook_modules.begin(), out.thread_entry_hook_modules.end(), module) ==
          out.thread_entry_hook_modules.end()) {
        out.thread_entry_hook_modules.push_back(module);
      }
    }
  };

  if (best.suspects.empty()) {
    if (!bestAny.pcs.empty()) {
      recordThreadEntryHooks(bestAny.pcs, bestAny.end);
      recordTruncation(bestAny.pcs, bestAny.end);
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

  recordThreadEntryHooks(best.pcs, best.end);
  recordTruncation(best.pcs, best.end);
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

CrashLoggerThreadDumpProbe ProbeCrashLoggerThreadDump(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<ModuleInfo>& modules,
  const std::vector<ThreadRecord>& threads,
  std::uint32_t mainTid,
  std::uint32_t ownerTid)
{
  CrashLoggerThreadDumpProbe probe{};
  MinidumpMemoryView mem;
  if (!dumpBase || threads.empty() || !mem.Init(dumpBase, dumpSize, &threads)) {
    return probe;
  }
  // Code the dump left out (it usually keeps ntdll's) is read from the
  // matching local file.
  const LocalImageMemory localImages(modules);
  mem.image_fallback = &localImages;
  const auto codeBefore = [&](std::uint64_t addr, std::size_t n) {
    std::vector<std::uint8_t> bytes(n);
    std::size_t got = 0;
    if (addr < n || !mem.Read(addr - n, bytes.data(), n, got) || got != n) {
      bytes.clear();
    }
    return bytes;
  };

  if (ownerTid != 0u) {
    const auto it = std::find_if(threads.begin(), threads.end(), [&](const ThreadRecord& tr) { return tr.tid == ownerTid; });
    CONTEXT ctx{};
    if (it != threads.end() && ReadThreadContextWin64(dumpBase, dumpSize, *it, ctx) && ctx.Rip != 0) {
      const auto bytes = codeBefore(ctx.Rip, 2u);
      if (!bytes.empty()) {
        probe.owner_ip_state = EndsWithSyscall(bytes) ? "after_syscall" : "not_after_syscall";
      }
    }
  }

  std::vector<const ModuleInfo*> crashLogger;
  for (const auto& m : modules) {
    if (IsCrashLoggerModuleName(m.filename) && m.end > m.base) {
      crashLogger.push_back(&m);
    }
  }
  if (crashLogger.empty()) {
    return probe;
  }
  constexpr std::size_t kMaxScanBytes = 0x10000;
  std::uint32_t bestCount = 0;
  for (const auto& thread : threads) {
    if (thread.tid == 0u || thread.tid == mainTid || thread.tid == ownerTid) {
      continue;
    }
    CONTEXT ctx{};
    const std::uint8_t* stackBytes = nullptr;
    std::size_t stackSize = 0;
    std::uint64_t stackBase = 0;
    if (!ReadThreadContextWin64(dumpBase, dumpSize, thread, ctx) ||
        !minidump::GetThreadStackBytes(dumpBase, dumpSize, thread, stackBytes, stackSize, stackBase)) {
      continue;
    }
    std::size_t start = 0;
    if (ctx.Rsp >= stackBase && ctx.Rsp - stackBase < stackSize) {
      start = static_cast<std::size_t>(ctx.Rsp - stackBase);
    }
    const std::size_t end = start + std::min<std::size_t>(stackSize - start, kMaxScanBytes);
    std::uint32_t count = 0;
    const ModuleInfo* hitModule = nullptr;
    for (std::size_t off = start; off + sizeof(std::uint64_t) <= end; off += sizeof(std::uint64_t)) {
      std::uint64_t value = 0;
      std::memcpy(&value, stackBytes + off, sizeof(value));
      const auto moduleIt = std::find_if(crashLogger.begin(), crashLogger.end(), [&](const ModuleInfo* m) {
        return value >= m->base && value < m->end;
      });
      if (moduleIt != crashLogger.end() && EndsWithCallInstruction(codeBefore(value, 7u))) {
        ++count;
        hitModule = *moduleIt;
      }
    }
    if (count > bestCount && hitModule) {
      bestCount = count;
      probe.crash_logger_tid = thread.tid;
      probe.crash_logger_module = hitModule->filename;
    }
  }
  return probe;
}

FloatCompareSpinProbe ProbeFloatCompareSpin(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<ModuleInfo>& modules,
  const std::vector<ThreadRecord>& threads,
  std::uint32_t tid)
{
  FloatCompareSpinProbe probe{};
  const auto it = std::find_if(threads.begin(), threads.end(), [&](const ThreadRecord& tr) { return tr.tid == tid; });
  CONTEXT ctx{};
  const std::uint8_t* contextBytes = nullptr;
  std::size_t contextSize = 0;
  if (tid == 0u || it == threads.end() || !ReadThreadContextWin64(dumpBase, dumpSize, *it, ctx) || ctx.Rip == 0 ||
      !minidump::GetThreadContextBytes(dumpBase, dumpSize, *it, contextBytes, contextSize)) {
    return probe;
  }
  probe.rip = ctx.Rip;
  MinidumpMemoryView mem;
  if (!mem.Init(dumpBase, dumpSize, &threads)) {
    return probe;
  }
  // The dump's own code bytes first (a local SkyrimSE.exe has its code
  // encrypted on disk), then the matching local image.
  const LocalImageMemory localImages(modules);
  const auto readWindow = [&](const MinidumpMemoryView& view, std::size_t before, std::size_t after,
                              std::vector<std::uint8_t>& out) {
    if (ctx.Rip < before) {
      return false;
    }
    out.assign(before + after, 0);
    std::size_t got = 0;
    return view.Read(ctx.Rip - before, out.data(), out.size(), got) && got == out.size();
  };
  MinidumpMemoryView withImages = mem;
  withImages.image_fallback = &localImages;
  std::vector<std::uint8_t> code;
  std::size_t ripOffset = 0;
  // A shorter window when the full one crosses the end of what is readable.
  for (const std::size_t divisor : { 1u, 2u }) {
    const std::size_t before = kSpinLoopWindowBefore / divisor;
    const std::size_t after = kSpinLoopWindowAfter / divisor;
    if (readWindow(mem, before, after, code) || readWindow(withImages, before, after, code)) {
      ripOffset = before;
      break;
    }
    code.clear();
  }
  if (code.empty()) {
    return probe;
  }
  const auto loop = FindFloatCompareLoop(code, ripOffset);
  if (!loop.found) {
    return probe;
  }
  probe.loop_found = true;
  probe.compare = FloatCompareMnemonic(loop);
  probe.mxcsr = ReadContextMxCsr(contextBytes, contextSize).value_or(0u);
  std::uint64_t bits = 0;
  if (const auto reg = FindNaNCompareRegister(loop, contextBytes, contextSize, &bits)) {
    probe.nan = true;
    probe.nan_register = "xmm" + std::to_string(static_cast<unsigned>(*reg));
    probe.nan_bits = bits;
  }
  return probe;
}

}  // namespace skydiag::dump_tool::internal
