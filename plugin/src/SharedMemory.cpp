#include "SkyrimDiag/SharedMemory.h"

#include <Windows.h>

#include <cstring>
#include <iterator>
#include <string>

#include "SkyrimDiag/Blackbox.h"
#include "SkyrimDiagProtocol.h"
#include "SkyrimDiagSecondFault.h"
#include "SkyrimDiagShared.h"

namespace skydiag::plugin {
namespace {

HANDLE g_mapping = nullptr;
HANDLE g_crashEvent = nullptr;
HANDLE g_unhandledEvent = nullptr;
wchar_t g_dumpSettledEventName[64]{};
HANDLE g_secondFaultMapping = nullptr;
HANDLE g_secondFaultPostedEvent = nullptr;
second_fault::Slot* g_secondFaultSlot = nullptr;
wchar_t g_secondFaultSettledEventName[64]{};
skydiag::SharedLayout* g_shared = nullptr;
std::uint32_t g_mainThreadId = 0;

// Fixed buffers: this optional part must not throw before the v4 crash event
// exists.
bool FormatSecondFaultName(wchar_t (&out)[64], const wchar_t* suffix) noexcept
{
  return _snwprintf_s(
           out, _TRUNCATE, L"%ls%lu%ls", skydiag::protocol::kKernelObjectPrefix, GetCurrentProcessId(), suffix) > 0;
}

void InitSecondFault() noexcept
{
  wchar_t mappingName[64]{};
  wchar_t postedName[64]{};
  if (!FormatSecondFaultName(mappingName, second_fault::kMappingSuffix) ||
      !FormatSecondFaultName(postedName, second_fault::kPostedSuffix) ||
      !FormatSecondFaultName(g_secondFaultSettledEventName, second_fault::kSettledSuffix)) {
    g_secondFaultSettledEventName[0] = L'\0';
    return;
  }
  g_secondFaultMapping = CreateFileMappingW(
    INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(second_fault::Slot), mappingName);
  if (!g_secondFaultMapping) {
    return;
  }
  g_secondFaultSlot = static_cast<second_fault::Slot*>(
    MapViewOfFile(g_secondFaultMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(second_fault::Slot)));
  if (g_secondFaultSlot) {
    std::memset(g_secondFaultSlot, 0, sizeof(*g_secondFaultSlot));
    g_secondFaultSlot->magic = second_fault::kMagic;
    g_secondFaultSlot->version = second_fault::kVersion;
    g_secondFaultSlot->size = sizeof(*g_secondFaultSlot);
    g_secondFaultPostedEvent = CreateEventW(nullptr, FALSE, FALSE, postedName);
  }
}

void ShutdownSecondFault() noexcept
{
  if (g_secondFaultPostedEvent) {
    CloseHandle(g_secondFaultPostedEvent);
    g_secondFaultPostedEvent = nullptr;
  }
  if (g_secondFaultSlot) {
    UnmapViewOfFile(g_secondFaultSlot);
    g_secondFaultSlot = nullptr;
  }
  if (g_secondFaultMapping) {
    CloseHandle(g_secondFaultMapping);
    g_secondFaultMapping = nullptr;
  }
}

}  // namespace

std::wstring MakeKernelName(const wchar_t* suffix)
{
  const auto pid = GetCurrentProcessId();
  std::wstring name;
  name.reserve(64);
  name.append(skydiag::protocol::kKernelObjectPrefix);
  name.append(std::to_wstring(pid));
  name.append(suffix);
  return name;
}

bool InitSharedMemory()
{
  if (g_shared) {
    return true;
  }

  const auto pid = GetCurrentProcessId();
  const std::wstring shmName = MakeKernelName(skydiag::protocol::kKernelObjectSuffix_SharedMemory);
  const std::wstring crashEventName = MakeKernelName(skydiag::protocol::kKernelObjectSuffix_CrashEvent);

  g_mapping = CreateFileMappingW(
    INVALID_HANDLE_VALUE,
    nullptr,
    PAGE_READWRITE,
    0,
    static_cast<DWORD>(sizeof(skydiag::SharedLayout)),
    shmName.c_str());
  if (!g_mapping) {
    return false;
  }

  void* view = MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(skydiag::SharedLayout));
  if (!view) {
    CloseHandle(g_mapping);
    g_mapping = nullptr;
    return false;
  }

  g_shared = static_cast<skydiag::SharedLayout*>(view);
  std::memset(g_shared, 0, sizeof(skydiag::SharedLayout));

  // ADR-0032. Not being able to create it only turns the signal off. Made
  // before the crash event: a helper that can open the crash event must
  // find this one too.
  g_unhandledEvent = CreateEventW(
    nullptr,
    /*bManualReset=*/TRUE,
    /*bInitialState=*/FALSE,
    MakeKernelName(skydiag::protocol::kKernelObjectSuffix_UnhandledEvent).c_str());
  const std::wstring dumpSettledEventName = MakeKernelName(skydiag::protocol::kKernelObjectSuffix_DumpSettledEvent);
  if (dumpSettledEventName.size() < std::size(g_dumpSettledEventName)) {
    std::memcpy(
      g_dumpSettledEventName,
      dumpSettledEventName.c_str(),
      (dumpSettledEventName.size() + 1u) * sizeof(wchar_t));
  }

  InitSecondFault();
  g_crashEvent = CreateEventW(nullptr, /*bManualReset=*/TRUE, /*bInitialState=*/FALSE, crashEventName.c_str());
  if (!g_crashEvent) {
    ShutdownSecondFault();
    if (g_unhandledEvent) {
      CloseHandle(g_unhandledEvent);
      g_unhandledEvent = nullptr;
    }
    UnmapViewOfFile(g_shared);
    g_shared = nullptr;
    CloseHandle(g_mapping);
    g_mapping = nullptr;
    return false;
  }

  LARGE_INTEGER freq{};
  LARGE_INTEGER now{};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&now);

  g_shared->header.magic = skydiag::kMagic;
  g_shared->header.version = skydiag::kVersion;
  g_shared->header.pid = pid;
  g_shared->header.capacity = skydiag::kEventCapacity;
  g_shared->header.qpc_freq = static_cast<std::uint64_t>(freq.QuadPart);
  g_shared->header.start_qpc = static_cast<std::uint64_t>(now.QuadPart);
  g_shared->header.last_heartbeat_qpc = static_cast<std::uint64_t>(now.QuadPart);
  g_shared->header.state_flags = skydiag::kState_Loading;

  g_mainThreadId = GetCurrentThreadId();

  // Session start marker.
  skydiag::EventPayload p{};
  p.a = pid;
  PushEventAlways(skydiag::EventType::kSessionStart, p, sizeof(p));

  return true;
}

void ShutdownSharedMemory()
{
  ShutdownSecondFault();
  if (g_shared) {
    UnmapViewOfFile(g_shared);
    g_shared = nullptr;
  }
  if (g_crashEvent) {
    CloseHandle(g_crashEvent);
    g_crashEvent = nullptr;
  }
  if (g_unhandledEvent) {
    CloseHandle(g_unhandledEvent);
    g_unhandledEvent = nullptr;
  }
  if (g_mapping) {
    CloseHandle(g_mapping);
    g_mapping = nullptr;
  }
}

std::uint32_t GetMainThreadId() noexcept
{
  return g_mainThreadId;
}

skydiag::SharedLayout* GetShared() noexcept
{
  return g_shared;
}

HANDLE GetCrashEvent() noexcept
{
  return g_crashEvent;
}

HANDLE GetUnhandledEvent() noexcept
{
  return g_unhandledEvent;
}

const wchar_t* GetDumpSettledEventName() noexcept
{
  return g_dumpSettledEventName;
}

second_fault::Slot* GetSecondFaultSlot() noexcept
{
  return g_secondFaultSlot;
}

HANDLE GetSecondFaultPostedEvent() noexcept
{
  return g_secondFaultPostedEvent;
}

const wchar_t* GetSecondFaultSettledEventName() noexcept
{
  return g_secondFaultSettledEventName;
}

}  // namespace skydiag::plugin
