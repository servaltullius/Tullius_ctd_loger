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

void DetachSecondFault(AttachedProcess& proc) noexcept
{
  if (proc.secondFaultSettled) {
    CloseHandle(proc.secondFaultSettled);
    proc.secondFaultSettled = nullptr;
  }
  if (proc.secondFaultPosted) {
    CloseHandle(proc.secondFaultPosted);
    proc.secondFaultPosted = nullptr;
  }
  if (proc.secondFault) {
    UnmapViewOfFile(proc.secondFault);
    proc.secondFault = nullptr;
  }
  if (proc.secondFaultMapping) {
    CloseHandle(proc.secondFaultMapping);
    proc.secondFaultMapping = nullptr;
  }
}

// ADR-0036 is optional: whatever fails here, C++ exceptions included, only
// turns it off, and v4 attaches as before.
void AttachSecondFault(AttachedProcess& proc) noexcept
{
  namespace fault = skydiag::second_fault;
  try {
    proc.secondFaultMapping = OpenFileMappingW(
      FILE_MAP_READ | FILE_MAP_WRITE, FALSE, MakeKernelName(proc.pid, fault::kMappingSuffix).c_str());
    if (!proc.secondFaultMapping) {
      proc.secondFaultAttachError = GetLastError();
      return;
    }
    proc.secondFault = static_cast<fault::Slot*>(MapViewOfFile(
      proc.secondFaultMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(fault::Slot)));
    if (!proc.secondFault) {
      proc.secondFaultAttachError = GetLastError();
    } else if (proc.secondFault->magic != fault::kMagic || proc.secondFault->version != fault::kVersion ||
               proc.secondFault->size != sizeof(fault::Slot)) {
      proc.secondFaultAttachError = ERROR_REVISION_MISMATCH;
    } else {
      proc.secondFaultPosted = OpenEventW(SYNCHRONIZE, FALSE, MakeKernelName(proc.pid, fault::kPostedSuffix).c_str());
      proc.secondFaultAttachError = proc.secondFaultPosted ? ERROR_SUCCESS : GetLastError();
    }
  } catch (...) {
    proc.secondFaultAttachError = ERROR_NOT_ENOUGH_MEMORY;
  }
  if (proc.secondFaultAttachError != ERROR_SUCCESS) {
    DetachSecondFault(proc);
  }
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
  // Include SYNCHRONIZE so the helper can detect process exit via WaitForSingleObject.
  // Include PROCESS_DUP_HANDLE for MiniDumpWithHandleData compatibility.
  out.process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid);
  if (!out.process) {
    if (err) *err = L"OpenProcess failed: " + std::to_wstring(GetLastError());
    return false;
  }

  out.shmMapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, shmName.c_str());
  if (!out.shmMapping) {
    if (err) *err = L"OpenFileMappingW failed: " + std::to_wstring(GetLastError());
    Detach(out);
    return false;
  }

  void* view = MapViewOfFile(out.shmMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
  if (!view) {
    if (err) *err = L"MapViewOfFile failed: " + std::to_wstring(GetLastError());
    Detach(out);
    return false;
  }
  out.shmWritable = static_cast<skydiag::SharedLayout*>(view);
  out.shm = out.shmWritable;
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

  out.pid = pid;
  TryAttachCrashEvent(out, nullptr);
  AttachSecondFault(out);
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

bool TryAttachCrashEvent(AttachedProcess& proc, std::wstring* err)
{
  if (proc.crashEvent) {
    proc.crashEventOpenError = ERROR_SUCCESS;
    if (err) {
      err->clear();
    }
    return true;
  }
  if (proc.pid == 0) {
    proc.crashEventOpenError = ERROR_INVALID_PARAMETER;
    if (err) {
      *err = L"Cannot open crash event before pid is initialized.";
    }
    return false;
  }

  const std::wstring crashEventName = MakeKernelName(proc.pid, skydiag::protocol::kKernelObjectSuffix_CrashEvent);
  proc.crashEvent = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, crashEventName.c_str());
  if (!proc.crashEvent) {
    proc.crashEventOpenError = GetLastError();
    if (err) {
      *err = L"OpenEventW failed: " + std::to_wstring(proc.crashEventOpenError);
    }
    return false;
  }

  proc.crashEventOpenError = ERROR_SUCCESS;
  if (err) {
    err->clear();
  }

  // ADR-0032: the plugin makes its "unhandled" event before the crash event,
  // so it is there now if the plugin has it at all.
  (void)TryAttachUnhandledSignal(proc);
  return true;
}

bool TryAttachUnhandledSignal(AttachedProcess& proc)
{
  if (proc.pid == 0) {
    return false;
  }
  if (!proc.unhandledEvent) {
    proc.unhandledEvent = OpenEventW(
      EVENT_MODIFY_STATE | SYNCHRONIZE,
      FALSE,
      MakeKernelName(proc.pid, skydiag::protocol::kKernelObjectSuffix_UnhandledEvent).c_str());
    proc.unhandledEventOpenError = proc.unhandledEvent ? ERROR_SUCCESS : GetLastError();
  }
  if (proc.unhandledEvent && !proc.dumpSettledEvent) {
    proc.dumpSettledEvent = CreateEventW(
      nullptr,
      /*bManualReset=*/TRUE,
      /*bInitialState=*/FALSE,
      MakeKernelName(proc.pid, skydiag::protocol::kKernelObjectSuffix_DumpSettledEvent).c_str());
    proc.dumpSettledEventCreateError = proc.dumpSettledEvent ? ERROR_SUCCESS : GetLastError();
  }
  return proc.unhandledEvent && proc.dumpSettledEvent;
}

bool ShouldRetryUnhandledSignalAttach(const AttachedProcess& proc) noexcept
{
  if (!proc.crashEvent || (proc.unhandledEvent && proc.dumpSettledEvent)) {
    return false;
  }
  return proc.unhandledEvent || proc.unhandledEventOpenError != ERROR_FILE_NOT_FOUND;
}

bool StartSecondFaultSignal(AttachedProcess& proc, const std::filesystem::path& outBase) noexcept
{
  if (!proc.secondFault || !proc.secondFaultPosted) {
    return false;
  }
  try {
    proc.secondFaultOutputBase = outBase;
    proc.secondFaultSettled = CreateEventW(
      nullptr, FALSE, FALSE, MakeKernelName(proc.pid, skydiag::second_fault::kSettledSuffix).c_str());
    if (!proc.secondFaultSettled) {
      proc.secondFaultAttachError = GetLastError();
    }
  } catch (...) {
    proc.secondFaultAttachError = ERROR_NOT_ENOUGH_MEMORY;
  }
  if (!proc.secondFaultSettled) {
    // Without a consumer the slot is not used: turn the feature off.
    DetachSecondFault(proc);
    return false;
  }
  return true;
}

void StopSecondFaultSignal(const AttachedProcess& proc) noexcept
{
  if (proc.secondFaultSettled) {
    CloseHandle(proc.secondFaultSettled);
    proc.secondFaultSettled = nullptr;
  }
}

void Detach(AttachedProcess& p)
{
  DetachSecondFault(p);
  p.secondFaultAttachError = ERROR_SUCCESS;
  p.secondFaultOutputBase.clear();
  p.secondFaultRecord.clear();
  p.secondFaultRetentionPending = false;
  if (p.shm) {
    UnmapViewOfFile(p.shm);
    p.shm = nullptr;
    p.shmWritable = nullptr;
  }
  if (p.shmMapping) {
    CloseHandle(p.shmMapping);
    p.shmMapping = nullptr;
  }
  if (p.crashEvent) {
    CloseHandle(p.crashEvent);
    p.crashEvent = nullptr;
  }
  if (p.unhandledEvent) {
    CloseHandle(p.unhandledEvent);
    p.unhandledEvent = nullptr;
  }
  if (p.dumpSettledEvent) {
    CloseHandle(p.dumpSettledEvent);
    p.dumpSettledEvent = nullptr;
  }
  if (p.process) {
    CloseHandle(p.process);
    p.process = nullptr;
  }
  p.pid = 0;
  p.shmSize = 0;
  p.crashEventOpenError = ERROR_SUCCESS;
  p.unhandledEventOpenError = ERROR_SUCCESS;
  p.dumpSettledEventCreateError = ERROR_SUCCESS;
}

}  // namespace skydiag::helper
