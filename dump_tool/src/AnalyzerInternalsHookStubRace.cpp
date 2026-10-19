#include "AnalyzerInternals.h"

#include "AnalyzerInternalsStackwalkPriv.h"
#include "HookStubRace.h"

#include <array>
#include <cwchar>

namespace skydiag::dump_tool::internal {

using stackwalk_internal::MinidumpMemoryView;

HookStubRaceInfo DetectHookStubRace(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& modules,
  const std::vector<minidump::ThreadRecord>& threads,
  const AnalysisResult& out,
  const std::optional<CONTEXT>& excCtx)
{
  HookStubRaceInfo info{};
  if (out.exc_code != kExceptionBreakpoint || out.exc_addr == 0 ||
      minidump::FindModuleIndexForAddress(modules, out.exc_addr)) {
    return info;
  }

  // The dump's own memory only: what the stub holds now.
  MinidumpMemoryView mem;
  if (!mem.Init(dumpBase, dumpSize, &threads)) {
    return info;
  }
  std::array<std::uint8_t, kHookStubBytes> stub{};
  std::size_t got = 0;
  if (!mem.Read(out.exc_addr, stub.data(), stub.size(), got)) {
    return info;
  }
  const auto target = HookStubJumpTarget(stub.data(), got);
  if (!target) {
    return info;  // still an int3, or not a stub: nothing was being written
  }
  const auto mi = minidump::FindModuleIndexForAddress(modules, *target);
  if (!mi) {
    return info;
  }
  const auto& m = modules[*mi];
  if (m.is_systemish || m.is_game_exe || minidump::IsLikelyWindowsSystemModulePath(m.path)) {
    return info;
  }

  info.detected = true;
  info.stub_address = out.exc_addr;
  info.target = *target;
  info.target_module = m.filename;
  info.target_module_path = m.path;
  info.target_mod_name = m.inferred_mod_name;
  info.target_offset = *target - m.base;

  // An int3 does not push anything, so the top of the stack is the return
  // address of the call that entered the stub.
  if (excCtx && excCtx->Rsp != 0) {
    std::uint64_t ret = 0;
    std::array<std::uint8_t, 5> call{};
    std::size_t gotRet = 0;
    std::size_t gotCall = 0;
    if (mem.Read(excCtx->Rsp, &ret, sizeof(ret), gotRet) && gotRet == sizeof(ret) && ret > call.size() &&
        mem.Read(ret - call.size(), call.data(), call.size(), gotCall) && gotCall == call.size() &&
        IsCallRel32To(call.data(), ret, out.exc_addr)) {
      const std::uint64_t site = ret - call.size();
      if (const auto ci = minidump::FindModuleIndexForAddress(modules, site)) {
        info.call_site = minidump::ModulePlusOffsetText(modules[*ci].filename, site - modules[*ci].base);
      }
    }
  }
  // Only a patched game call that entered exactly this stub shows the CPU
  // took a hook while it was being written; without it (a breakpoint set and
  // removed, code reached some other way) nothing is claimed (v0.2.65 audit).
  if (info.call_site.empty()) {
    return HookStubRaceInfo{};
  }
  return info;
}

}  // namespace skydiag::dump_tool::internal
