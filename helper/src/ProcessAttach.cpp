#include "SkyrimDiagHelper/ProcessAttach.h"

#include <Windows.h>

#include <TlHelp32.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#include "SkyrimDiagProtocol.h"

namespace skydiag::helper {
namespace {

std::wstring MakeKernelName(std::uint32_t pid, const wchar_t* suffix)
{
  std::wstring name;
  name.reserve(64);
  name.append(skydiag::protocol::kKernelObjectPrefix);
  name.append(std::to_wstring(pid));
  name.append(suffix);
  return name;
}

bool IEquals(const std::wstring& a, const wchar_t* b)
{
  if (!b) {
    return false;
  }
  const std::size_t blen = std::wcslen(b);
  if (a.size() != blen) {
    return false;
  }
  for (std::size_t i = 0; i < blen; ++i) {
    if (std::towlower(a[i]) != std::towlower(b[i])) {
      return false;
    }
  }
  return true;
}

std::vector<std::uint32_t> EnumerateCandidatePids()
{
  std::vector<std::uint32_t> pids;

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return pids;
  }

  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
    const std::wstring exe = pe.szExeFile;
    if (IEquals(exe, L"SkyrimSE.exe") || IEquals(exe, L"SkyrimVR.exe") || IEquals(exe, L"Skyrim.exe")) {
      pids.push_back(static_cast<std::uint32_t>(pe.th32ProcessID));
    }
  }

  CloseHandle(snap);
  return pids;
}

}  // namespace

bool AttachByPid(std::uint32_t pid, AttachedProcess& out, std::wstring* err)
{
  Detach(out);

  const std::wstring shmName = MakeKernelName(pid, skydiag::protocol::kKernelObjectSuffix_SharedMemory);
  const std::wstring crashEventName = MakeKernelName(pid, skydiag::protocol::kKernelObjectSuffix_CrashEvent);

  // Include SYNCHRONIZE so the helper can detect process exit via WaitForSingleObject.
  out.process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
  if (!out.process) {
    if (err) *err = L"OpenProcess failed: " + std::to_wstring(GetLastError());
    return false;
  }

  out.shmMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, shmName.c_str());
  if (!out.shmMapping) {
    if (err) *err = L"OpenFileMappingW failed: " + std::to_wstring(GetLastError());
    Detach(out);
    return false;
  }

  void* view = MapViewOfFile(out.shmMapping, FILE_MAP_READ, 0, 0, 0);
  if (!view) {
    if (err) *err = L"MapViewOfFile failed: " + std::to_wstring(GetLastError());
    Detach(out);
    return false;
  }
  out.shm = static_cast<const skydiag::SharedLayout*>(view);
  out.shmSize = sizeof(skydiag::SharedLayout);
  {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(view, &mbi, sizeof(mbi)) != 0 && mbi.RegionSize > 0) {
      if (mbi.RegionSize < out.shmSize) {
        if (err) *err = L"Shared memory view too small (RegionSize=" + std::to_wstring(static_cast<std::uint64_t>(mbi.RegionSize)) +
          L", expected=" + std::to_wstring(static_cast<std::uint64_t>(out.shmSize)) + L")";
        Detach(out);
        return false;
      }
    }
  }

  // Need EVENT_MODIFY_STATE to consume manual-reset crash events via ResetEvent
  // after handling, otherwise the helper can re-handle the same signal repeatedly.
  out.crashEvent = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, crashEventName.c_str());
  if (!out.crashEvent) {
    // Not fatal for hang-only mode; still allow attach.
    if (err) err->clear();
  }

  out.pid = pid;
  if (err) err->clear();
  return true;
}

bool FindAndAttach(AttachedProcess& out, std::wstring* err)
{
  const auto pids = EnumerateCandidatePids();
  for (const auto pid : pids) {
    if (AttachByPid(pid, out, nullptr)) {
      if (err) err->clear();
      return true;
    }
  }

  if (err) {
    *err = L"Could not find a running Skyrim process with an active SkyrimDiag shared memory mapping.";
  }
  return false;
}

void Detach(AttachedProcess& p)
{
  if (p.shm) {
    UnmapViewOfFile(p.shm);
    p.shm = nullptr;
  }
  if (p.shmMapping) {
    CloseHandle(p.shmMapping);
    p.shmMapping = nullptr;
  }
  if (p.crashEvent) {
    CloseHandle(p.crashEvent);
    p.crashEvent = nullptr;
  }
  if (p.process) {
    CloseHandle(p.process);
    p.process = nullptr;
  }
  p.pid = 0;
  p.shmSize = 0;
}

}  // namespace skydiag::helper
