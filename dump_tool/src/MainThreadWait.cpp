#include "MainThreadWait.h"

#include <algorithm>
#include <string>

namespace skydiag::dump_tool {
namespace {

std::wstring LowerAscii(std::wstring_view value)
{
  std::wstring out(value);
  for (auto& ch : out) {
    if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
    }
  }
  return out;
}

template <std::size_t N>
bool Contains(const std::wstring_view (&names)[N], std::wstring_view value)
{
  return std::find(std::begin(names), std::end(names), value) != std::end(names);
}

// Export names, so they resolve without PDBs, plus the internal names that
// appear when public symbols are available.
constexpr std::wstring_view kSleepApis[] = {
  L"NtDelayExecution", L"ZwDelayExecution", L"RtlDelayExecution", L"SleepEx", L"Sleep",
  L"NtYieldExecution", L"ZwYieldExecution", L"SwitchToThread",
};
constexpr std::wstring_view kSyncApis[] = {
  L"NtWaitForSingleObject", L"ZwWaitForSingleObject", L"NtWaitForMultipleObjects", L"ZwWaitForMultipleObjects",
  L"WaitForSingleObject", L"WaitForSingleObjectEx", L"WaitForMultipleObjects", L"WaitForMultipleObjectsEx",
  L"NtWaitForAlertByThreadId", L"ZwWaitForAlertByThreadId", L"NtWaitForKeyedEvent", L"ZwWaitForKeyedEvent",
  L"RtlSleepConditionVariableSRW", L"RtlSleepConditionVariableCS", L"SleepConditionVariableSRW",
  L"SleepConditionVariableCS", L"RtlWaitOnAddress", L"WaitOnAddress", L"RtlpWaitOnAddress",
  L"RtlpWaitOnCriticalSection", L"RtlpEnterCriticalSectionContended", L"RtlEnterCriticalSection",
  L"RtlAcquireSRWLockExclusive", L"RtlAcquireSRWLockShared", L"NtRemoveIoCompletion", L"ZwRemoveIoCompletion",
  L"GetQueuedCompletionStatus", L"GetQueuedCompletionStatusEx", L"NtUserMsgWaitForMultipleObjectsEx",
  L"MsgWaitForMultipleObjects", L"MsgWaitForMultipleObjectsEx", L"NtSignalAndWaitForSingleObject",
  L"SignalObjectAndWait",
};
// Export-only symbols name any address after an export by that export; keep
// the match close to the entry point.
constexpr std::uint64_t kMaxWaitApiDisplacement = 0x2000;

constexpr std::wstring_view kGraphicsDrivers[] = {
  L"nvwgf2umx.dll", L"nvwgf2um.dll", L"nvldumdx.dll", L"nvldumd.dll", L"nvd3dumx.dll", L"nvd3dum.dll",
  L"nvoglv64.dll", L"nvgpucomp64.dll", L"amdxx64.dll", L"atidxx64.dll", L"amdxc64.dll", L"atiumd64.dll",
  L"aticfx64.dll", L"amdihk64.dll", L"igd10iumd64.dll", L"igd12umd64.dll", L"igdumdim64.dll", L"igc64.dll",
  L"igd11dxva64.dll",
};
constexpr std::wstring_view kGraphicsRuntimes[] = {
  L"d3d11.dll", L"d3d12.dll", L"d3d12core.dll", L"dxgi.dll", L"d3d9.dll", L"dxcore.dll",
};

constexpr std::size_t kMaxPathModules = 6;

}  // namespace

bool LooksLikeGpuQueryPoll(const std::vector<std::uint8_t>& codeBefore)
{
  const std::size_t n = codeBefore.size();
  // The instruction ending at the return address is the Sleep call:
  // call [rip+disp32] (FF 15 xx xx xx xx) or call rel32 (E8 xx xx xx xx).
  std::size_t callStart = 0;
  if (n >= 6 && codeBefore[n - 6] == 0xFF && codeBefore[n - 5] == 0x15) {
    callStart = n - 6;
  } else if (n >= 5 && codeBefore[n - 5] == 0xE8) {
    callStart = n - 5;
  } else {
    return false;
  }
  // call qword ptr [reg+0E8h]: FF /2 with mod=10 and a 32-bit displacement
  // (0xE8 does not fit a signed 8-bit one). rsp (rm=100) would need a SIB byte.
  for (std::size_t i = 0; i + 6 <= callStart; ++i) {
    const std::uint8_t modrm = codeBefore[i + 1];
    if (codeBefore[i] == 0xFF && (modrm & 0xF8) == 0x90 && modrm != 0x94 && codeBefore[i + 2] == 0xE8 &&
        codeBefore[i + 3] == 0x00 && codeBefore[i + 4] == 0x00 && codeBefore[i + 5] == 0x00) {
      return true;
    }
  }
  return false;
}

bool IsGraphicsDriverModule(std::wstring_view moduleFilename)
{
  return Contains(kGraphicsDrivers, LowerAscii(moduleFilename));
}

bool IsGraphicsStackModule(std::wstring_view moduleFilename)
{
  const auto lower = LowerAscii(moduleFilename);
  return Contains(kGraphicsDrivers, lower) || Contains(kGraphicsRuntimes, lower);
}

MainThreadWaitInfo ClassifyMainThreadWait(const std::vector<ModalStackFrame>& frames)
{
  MainThreadWaitInfo info{};
  if (frames.empty()) {
    return info;
  }

  // The first frame that is neither Windows nor the graphics stack is the code
  // that made the call the thread is sitting in.
  std::size_t caller = frames.size();
  std::size_t topEnd = frames.size();
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const auto& frame = frames[i];
    if (!frame.has_module) {
      topEnd = i;
      break;
    }
    if (!frame.is_system && !IsGraphicsStackModule(frame.module_filename)) {
      caller = i;
      topEnd = i;
      break;
    }
  }

  const ModalStackFrame* waitFrame = nullptr;
  const ModalStackFrame* graphicsFrame = nullptr;
  bool unnamedSystemFrame = false;
  for (std::size_t i = 0; i < topEnd; ++i) {
    const auto& frame = frames[i];
    if (IsGraphicsStackModule(frame.module_filename)) {
      if (!graphicsFrame ||
          (IsGraphicsDriverModule(frame.module_filename) && !IsGraphicsDriverModule(graphicsFrame->module_filename))) {
        graphicsFrame = &frame;
      }
      continue;
    }
    if (frame.symbol.empty() || frame.displacement > kMaxWaitApiDisplacement) {
      // Without a close name the function is unknown, so it can be neither
      // ruled a wait nor ruled out as one.
      unnamedSystemFrame = true;
      continue;
    }
    // Keep the outermost wait API: "KERNELBASE.dll!SleepEx" reads better
    // than the ntdll system call under it.
    if (Contains(kSleepApis, frame.symbol)) {
      waitFrame = &frame;
      info.wait_class = "sleep";
    } else if (Contains(kSyncApis, frame.symbol)) {
      waitFrame = &frame;
      info.wait_class = "sync";
    }
  }
  if (waitFrame) {
    info.wait_api = waitFrame->module_filename + L"!" + waitFrame->symbol;
  }

  const bool callerKnown = caller < frames.size();
  if (callerKnown && caller == 0u) {
    info.kind = "running";
    info.waiting_module = frames[0].module_filename;
    info.waiting_mod_name = frames[0].inferred_mod_name;
  } else if (graphicsFrame && waitFrame) {
    info.kind = "graphics_driver_wait";
    info.waiting_module = graphicsFrame->module_filename;
  } else if (graphicsFrame && !unnamedSystemFrame) {
    info.kind = "running";
    info.waiting_module = graphicsFrame->module_filename;
  } else if (waitFrame && callerKnown) {
    const auto& callerFrame = frames[caller];
    info.kind = callerFrame.is_game_exe ? "engine_wait" : "plugin_wait";
    info.waiting_module = callerFrame.module_filename;
    info.waiting_mod_name = callerFrame.inferred_mod_name;
    if (callerFrame.is_game_exe && info.wait_class == "sleep" && LooksLikeGpuQueryPoll(callerFrame.code_before)) {
      info.engine_wait_detail = "gpu_query_poll";
    }
  } else if (!waitFrame && !unnamedSystemFrame && callerKnown) {
    info.kind = "running";
    info.waiting_module = frames[caller].module_filename;
    info.waiting_mod_name = frames[caller].inferred_mod_name;
  } else {
    // A wait whose caller the walk did not reach, or system frames without
    // names: not enough to say more.
    info.kind = "unknown";
  }

  const auto waitingLower = LowerAscii(info.waiting_module);
  std::vector<std::wstring> seen;
  for (std::size_t i = callerKnown ? caller : topEnd; i < frames.size(); ++i) {
    const auto& frame = frames[i];
    if (!frame.has_module || frame.is_system || frame.is_game_exe || frame.is_skse_runtime ||
        IsGraphicsStackModule(frame.module_filename)) {
      continue;
    }
    const auto lower = LowerAscii(frame.module_filename);
    if (lower == waitingLower || std::find(seen.begin(), seen.end(), lower) != seen.end()) {
      continue;
    }
    seen.push_back(lower);
    info.path_modules.push_back(frame.module_filename);
    if (info.path_modules.size() >= kMaxPathModules) {
      break;
    }
  }
  return info;
}

bool IsBystanderWait(const MainThreadWaitInfo& wait)
{
  return wait.kind == "engine_wait" || wait.kind == "graphics_driver_wait";
}

bool IsGpuWait(const MainThreadWaitInfo& wait)
{
  return wait.kind == "graphics_driver_wait" ||
         (wait.kind == "engine_wait" && wait.engine_wait_detail == "gpu_query_poll");
}

}  // namespace skydiag::dump_tool
