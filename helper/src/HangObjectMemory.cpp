#include "HangObjectMemory.h"

#include <Psapi.h>
#include <TlHelp32.h>

#include <algorithm>
#include <utility>

#include "SkyrimDiagHangObjectThreads.h"

namespace skydiag::helper::internal {
namespace {

using NtQueryInformationThreadFn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
// THREADINFOCLASS::ThreadQuerySetWin32StartAddress.
constexpr ULONG kThreadWin32StartAddress = 9;

// The game executable's image range (the process's first module).
bool GameImageRange(HANDLE process, std::uint64_t& base, std::uint64_t& end)
{
  HMODULE modules[1]{};
  DWORD needed = 0;
  MODULEINFO info{};
  if (!EnumProcessModulesEx(process, modules, sizeof(modules), &needed, LIST_MODULES_64BIT) || !modules[0] ||
      !GetModuleInformation(process, modules[0], &info, sizeof(info))) {
    return false;
  }
  base = reinterpret_cast<std::uint64_t>(info.lpBaseOfDll);
  end = base + info.SizeOfImage;
  return true;
}

// Threads of the process whose start address is in the game executable.
std::vector<std::uint32_t> GameThreadIds(HANDLE process)
{
  std::vector<std::uint32_t> tids;
  std::uint64_t base = 0;
  std::uint64_t end = 0;
  const auto query = reinterpret_cast<NtQueryInformationThreadFn>(
    GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
  if (!query || !GameImageRange(process, base, end)) {
    return tids;
  }
  const DWORD pid = GetProcessId(process);
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return tids;
  }
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid) {
      continue;
    }
    HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
    if (!thread) {
      continue;
    }
    PVOID start = nullptr;
    if (query(thread, kThreadWin32StartAddress, static_cast<PVOID>(&start), sizeof(start), nullptr) >= 0) {
      const auto address = reinterpret_cast<std::uint64_t>(start);
      if (address >= base && address < end) {
        tids.push_back(entry.th32ThreadID);
      }
    }
    CloseHandle(thread);
  }
  CloseHandle(snapshot);
  return tids;
}

std::uint64_t TotalBytes(const std::vector<ExtraDumpMemory>& ranges)
{
  std::uint64_t total = 0;
  for (const auto& range : ranges) {
    total += range.size;
  }
  return total;
}

}  // namespace

std::vector<DWORD> SelectHangObjectThreads(HANDLE process, std::optional<DWORD> mainTid, std::string_view wctJsonUtf8)
{
  const auto gameThreads = process ? GameThreadIds(process) : std::vector<std::uint32_t>{};
  const auto selected = skydiag::hang_objects::SelectThreadIds(mainTid.value_or(0u), wctJsonUtf8, gameThreads);
  return std::vector<DWORD>(selected.begin(), selected.end());
}

HangObjectMemoryCollector::HangObjectMemoryCollector(
  HANDLE process,
  std::vector<DWORD> tids,
  std::size_t budgetBytes,
  std::chrono::milliseconds timeBudget)
  : process_(process),
    tids_(std::move(tids)),
    contexts_(tids_.size()),
    budgetBytes_(budgetBytes),
    timeBudget_(timeBudget)
{
  stats_.threads_selected = static_cast<std::uint32_t>(tids_.size());
}

void HangObjectMemoryCollector::OnThreadContext(DWORD tid, const CONTEXT& context)
{
  const auto it = std::find(tids_.begin(), tids_.end(), tid);
  if (it != tids_.end()) {
    contexts_[static_cast<std::size_t>(it - tids_.begin())] = context;
  }
}

const std::vector<ExtraDumpMemory>& HangObjectMemoryCollector::Collect()
{
  if (collected_) {
    return ranges_;
  }
  collected_ = true;
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + timeBudget_;
  std::vector<ExtraDumpMemory> ranges;
  std::uint64_t used = 0;
  for (const auto& context : contexts_) {
    if (!context) {
      continue;  // the writer did not report this thread (it exited)
    }
    if (used >= budgetBytes_ || std::chrono::steady_clock::now() >= deadline) {
      stats_.budget_reached = true;
      break;
    }
    const auto budget = static_cast<std::size_t>(std::min<std::uint64_t>(budgetBytes_ - used, kHangObjectThreadBudget));
    auto thread = CollectThreadObjectMemory(process_, *context, budget, deadline);
    // Counted before merging across threads, so shared RTTI counts twice:
    // the real total can only be smaller.
    used += TotalBytes(thread);
    ranges.insert(ranges.end(), thread.begin(), thread.end());
    ++stats_.threads_read;
  }
  ranges_ = MergeExtraDumpMemory(std::move(ranges));
  stats_.ranges = static_cast<std::uint32_t>(ranges_.size());
  stats_.bytes = TotalBytes(ranges_);
  stats_.elapsed_us = static_cast<std::uint32_t>(
    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
  if (std::chrono::steady_clock::now() >= deadline) {
    stats_.budget_reached = true;
  }
  return ranges_;
}

}  // namespace skydiag::helper::internal
