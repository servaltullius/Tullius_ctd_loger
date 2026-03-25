#include "SkyrimDiagHelper/WctCapture.h"

#include <Windows.h>

#include <TlHelp32.h>
#include <wct.h>

#include <nlohmann/json.hpp>

#include "SkyrimDiagShared.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace skydiag::helper {
namespace {

constexpr DWORD kConsensusCaptureDelayMs = 15;

std::vector<DWORD> EnumerateThreads(DWORD pid)
{
  std::vector<DWORD> tids;

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return tids;
  }

  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID == pid) {
      tids.push_back(te.th32ThreadID);
    }
  }

  CloseHandle(snap);
  return tids;
}

std::string WideToUtf8(const wchar_t* s)
{
  if (!s) {
    return {};
  }
  const int wlen = static_cast<int>(wcslen(s));
  if (wlen == 0) {
    return {};
  }

  const int bytes = WideCharToMultiByte(CP_UTF8, 0, s, wlen, nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) {
    return {};
  }

  std::string out(bytes, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, wlen, out.data(), bytes, nullptr, nullptr);
  return out;
}

template <std::size_t N>
std::string WideToUtf8Bounded(const wchar_t (&buf)[N])
{
  std::size_t wlen = 0;
  while (wlen < N && buf[wlen] != L'\0') {
    wlen++;
  }
  if (wlen == 0) {
    return {};
  }
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(wlen), nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(wlen), out.data(), bytes, nullptr, nullptr);
  return out;
}

struct WctPassResult
{
  nlohmann::json threads = nlohmann::json::array();
  std::unordered_set<std::uint32_t> cycleTids;
  bool hasUsableData = false;
  bool hasLoadingSignal = false;
  std::uint32_t longestWaitTid = 0;
  std::uint64_t longestWaitMs = 0;
};

std::vector<std::uint32_t> SortedCycleThreadIds(const std::unordered_set<std::uint32_t>& tids)
{
  std::vector<std::uint32_t> out(tids.begin(), tids.end());
  std::sort(out.begin(), out.end());
  return out;
}

bool ReadLoadingSignal(const volatile std::uint32_t* stateFlags)
{
  if (!stateFlags) {
    return false;
  }
  return ((*stateFlags & skydiag::kState_Loading) != 0u);
}

void CaptureWctPass(HWCT session, std::uint32_t pid, const volatile std::uint32_t* captureStateFlags, WctPassResult& out)
{
  out = WctPassResult{};
  out.hasLoadingSignal = ReadLoadingSignal(captureStateFlags);

  const auto tids = EnumerateThreads(pid);
  for (const auto tid : tids) {
    DWORD nodeCount = WCT_MAX_NODE_COUNT;
    WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT]{};
    BOOL isCycle = FALSE;

    const DWORD flags = WCTP_GETINFO_ALL_FLAGS;
    const BOOL ok = GetThreadWaitChain(session, /*Context=*/0, flags, tid, &nodeCount, nodes, &isCycle);
    const DWORD lastErr = GetLastError();

    nlohmann::json thread = nlohmann::json::object();
    thread["tid"] = tid;
    thread["isCycle"] = isCycle ? true : false;
    thread["nodes"] = nlohmann::json::array();

    if (!ok && lastErr != ERROR_MORE_DATA) {
      thread["error"] = lastErr;
      out.threads.push_back(std::move(thread));
      continue;
    }

    out.hasUsableData = true;

    if (isCycle) {
      out.cycleTids.insert(tid);
    }

    std::uint64_t threadLongestWait = 0;
    for (DWORD i = 0; i < nodeCount; ++i) {
      const auto& n = nodes[i];
      nlohmann::json node = nlohmann::json::object();
      node["objectType"] = static_cast<std::uint32_t>(n.ObjectType);
      node["objectStatus"] = static_cast<std::uint32_t>(n.ObjectStatus);
      if (n.ObjectType == WctThreadType) {
        const std::uint64_t waitTime = n.ThreadObject.WaitTime;
        threadLongestWait = std::max(threadLongestWait, waitTime);
        node["thread"] = {
          { "processId", n.ThreadObject.ProcessId },
          { "threadId", n.ThreadObject.ThreadId },
          { "waitTime", waitTime },
          { "contextSwitches", n.ThreadObject.ContextSwitches },
        };
      } else {
        // For non-thread nodes, LockObject.ObjectName is valid. For thread nodes it's a union and reading it is garbage.
        node["objectName"] = WideToUtf8Bounded(n.LockObject.ObjectName);
      }

      thread["nodes"].push_back(std::move(node));
    }

    if (threadLongestWait > out.longestWaitMs) {
      out.longestWaitMs = threadLongestWait;
      out.longestWaitTid = tid;
    }

    out.threads.push_back(std::move(thread));
  }
}

}  // namespace

struct DebugPrivilegeResult
{
  bool enabled = false;
  DWORD error = 0;
};

DebugPrivilegeResult EnableDebugPrivilege()
{
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    return DebugPrivilegeResult{ false, GetLastError() };
  }

  LUID luid{};
  if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
    const DWORD le = GetLastError();
    CloseHandle(token);
    return DebugPrivilegeResult{ false, le };
  }

  TOKEN_PRIVILEGES tp{};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = luid;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

  const BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
  const DWORD le = GetLastError();
  CloseHandle(token);

  return DebugPrivilegeResult{ ok && le == ERROR_SUCCESS, le };
}

bool CaptureWct(
  std::uint32_t pid,
  const volatile std::uint32_t* captureStateFlags,
  nlohmann::json& out,
  std::wstring* err)
{
  out = nlohmann::json::object();
  out["pid"] = pid;
  out["threads"] = nlohmann::json::array();
  out["passes"] = nlohmann::json::array();
  out["capture_passes"] = 0;
  out["cycle_consensus"] = false;
  out["repeated_cycle_tids"] = nlohmann::json::array();
  out["consistent_loading_signal"] = false;
  out["longest_wait_tid_consensus"] = false;

  {
    const auto dbg = EnableDebugPrivilege();  // best-effort
    out["debugPrivilegeEnabled"] = dbg.enabled;
    if (!dbg.enabled && dbg.error != 0) {
      out["debugPrivilegeError"] = static_cast<std::uint32_t>(dbg.error);
    }
  }

  // Synchronous session: Flags=0. (Some SDKs only define WCT_ASYNC_OPEN_FLAG.)
  HWCT session = OpenThreadWaitChainSession(0, nullptr);
  if (!session) {
    if (err) *err = L"OpenThreadWaitChainSession failed: " + std::to_wstring(GetLastError());
    return false;
  }

  // Best-effort COM wait-chain support.
  bool comCallbackRegistered = false;
  if (HMODULE ole32 = GetModuleHandleW(L"ole32.dll"); ole32) {
    const auto pCoGetCallState = GetProcAddress(ole32, "CoGetCallState");
    const auto pCoGetActivationState = GetProcAddress(ole32, "CoGetActivationState");
    if (pCoGetCallState && pCoGetActivationState) {
      RegisterWaitChainCOMCallback(
        reinterpret_cast<PCOGETCALLSTATE>(pCoGetCallState),
        reinterpret_cast<PCOGETACTIVATIONSTATE>(pCoGetActivationState));
      comCallbackRegistered = true;
    }
  }
  out["comCallbackRegistered"] = comCallbackRegistered;

  std::vector<WctPassResult> passes;
  passes.reserve(2);

  WctPassResult firstPass{};
  CaptureWctPass(session, pid, captureStateFlags, firstPass);
  out["passes"].push_back({
    { "pass_index", 0u },
    { "capture_usable", firstPass.hasUsableData },
    { "threads", firstPass.threads },
    { "cycle_thread_ids", SortedCycleThreadIds(firstPass.cycleTids) },
    { "has_loading_signal", firstPass.hasLoadingSignal },
    { "longest_wait_tid", firstPass.longestWaitTid },
    { "longest_wait_ms", firstPass.longestWaitMs },
  });
  passes.push_back(std::move(firstPass));

  Sleep(kConsensusCaptureDelayMs);

  WctPassResult secondPass{};
  CaptureWctPass(session, pid, captureStateFlags, secondPass);
  out["passes"].push_back({
    { "pass_index", 1u },
    { "capture_usable", secondPass.hasUsableData },
    { "threads", secondPass.threads },
    { "cycle_thread_ids", SortedCycleThreadIds(secondPass.cycleTids) },
    { "has_loading_signal", secondPass.hasLoadingSignal },
    { "longest_wait_tid", secondPass.longestWaitTid },
    { "longest_wait_ms", secondPass.longestWaitMs },
  });
  passes.push_back(std::move(secondPass));

  std::uint32_t usablePassCount = 0;
  for (const auto& pass : passes) {
    if (pass.hasUsableData) {
      usablePassCount++;
    }
  }
  out["capture_passes"] = usablePassCount;

  std::size_t primaryPassIndex = 0;
  if (!passes.empty() && !passes[0].hasUsableData) {
    for (std::size_t i = 1; i < passes.size(); ++i) {
      if (passes[i].hasUsableData) {
        primaryPassIndex = i;
        break;
      }
    }
  }
  out["threads"] = passes[primaryPassIndex].threads;

  if (passes.size() >= 2 && passes[0].hasUsableData && passes[1].hasUsableData) {
    std::vector<std::uint32_t> repeatedCycleTids;
    for (const auto tid : passes[0].cycleTids) {
      if (passes[1].cycleTids.find(tid) != passes[1].cycleTids.end()) {
        repeatedCycleTids.push_back(tid);
      }
    }
    std::sort(repeatedCycleTids.begin(), repeatedCycleTids.end());
    out["repeated_cycle_tids"] = repeatedCycleTids;
    out["cycle_consensus"] = !repeatedCycleTids.empty();
    out["consistent_loading_signal"] = passes[0].hasLoadingSignal && passes[1].hasLoadingSignal;
    out["longest_wait_tid_consensus"] =
      passes[0].longestWaitTid != 0 &&
      passes[0].longestWaitTid == passes[1].longestWaitTid;
  }

  CloseThreadWaitChainSession(session);
  if (err) err->clear();
  return true;
}

}  // namespace skydiag::helper
