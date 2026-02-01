#include "SkyrimDiagHelper/WctCapture.h"

#include <Windows.h>

#include <TlHelp32.h>
#include <wct.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace skydiag::helper {
namespace {

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

bool CaptureWct(std::uint32_t pid, nlohmann::json& out, std::wstring* err)
{
  out = nlohmann::json::object();
  out["pid"] = pid;
  out["threads"] = nlohmann::json::array();

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
      out["threads"].push_back(std::move(thread));
      continue;
    }

    for (DWORD i = 0; i < nodeCount; ++i) {
      const auto& n = nodes[i];
      nlohmann::json node = nlohmann::json::object();
      node["objectType"] = static_cast<std::uint32_t>(n.ObjectType);
      node["objectStatus"] = static_cast<std::uint32_t>(n.ObjectStatus);
      if (n.ObjectType == WctThreadType) {
        node["thread"] = {
          { "processId", n.ThreadObject.ProcessId },
          { "threadId", n.ThreadObject.ThreadId },
          { "waitTime", n.ThreadObject.WaitTime },
          { "contextSwitches", n.ThreadObject.ContextSwitches },
        };
      } else {
        // For non-thread nodes, LockObject.ObjectName is valid. For thread nodes it's a union and reading it is garbage.
        node["objectName"] = WideToUtf8Bounded(n.LockObject.ObjectName);
      }

      thread["nodes"].push_back(std::move(node));
    }

    out["threads"].push_back(std::move(thread));
  }

  CloseThreadWaitChainSession(session);
  if (err) err->clear();
  return true;
}

}  // namespace skydiag::helper
