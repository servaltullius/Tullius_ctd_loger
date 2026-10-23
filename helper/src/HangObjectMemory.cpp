#include "HangObjectMemory.h"

#include <Psapi.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "SkyrimDiagGameLocks.h"
#include "SkyrimDiagHangObjectThreads.h"

namespace skydiag::helper::internal {
namespace {

using NtQueryInformationThreadFn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
// THREADINFOCLASS::ThreadQuerySetWin32StartAddress.
constexpr ULONG kThreadWin32StartAddress = 9;

std::uint64_t TotalBytes(const std::vector<ExtraDumpMemory>& ranges)
{
  std::uint64_t total = 0;
  for (const auto& range : ranges) {
    total += range.size;
  }
  return total;
}

std::uint32_t ElapsedUs(std::chrono::steady_clock::time_point start)
{
  return static_cast<std::uint32_t>(
    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}

// The readable bytes from `address` on, up to `size`, inside one region that
// IsReadableRegion accepts (0 when the page there is not one); `image`
// further asks for a page of a module image.
std::size_t ReadableBytes(HANDLE process, std::uint64_t address, std::size_t size, bool image)
{
  MEMORY_BASIC_INFORMATION info{};
  if (size == 0 || VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) != sizeof(info) ||
      !IsReadableRegion(info) || (image && info.Type != MEM_IMAGE)) {
    return 0;
  }
  const auto regionEnd = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
  return regionEnd <= address ? 0 : static_cast<std::size_t>(std::min<std::uint64_t>(size, regionEnd - address));
}

bool ReadChecked(HANDLE process, std::uint64_t address, void* destination, std::size_t size, bool image)
{
  SIZE_T got = 0;
  return ReadableBytes(process, address, size, image) == size &&
         ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), destination, size, &got) && got == size;
}

}  // namespace

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

std::vector<std::uint32_t> GameThreadIds(HANDLE process)
{
  std::vector<std::uint32_t> tids;
  std::uint64_t base = 0;
  std::uint64_t end = 0;
  const auto query = reinterpret_cast<NtQueryInformationThreadFn>(
    GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
  if (!process || !query || !GameImageRange(process, base, end)) {
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

std::vector<DWORD> SelectHangObjectThreads(HANDLE process, std::optional<DWORD> mainTid, std::string_view wctJsonUtf8)
{
  return SelectHangObjectThreads(mainTid, wctJsonUtf8, GameThreadIds(process));
}

std::vector<DWORD> SelectHangObjectThreads(
  std::optional<DWORD> mainTid,
  std::string_view wctJsonUtf8,
  const std::vector<std::uint32_t>& gameThreads)
{
  const auto selected = skydiag::hang_objects::SelectThreadIds(mainTid.value_or(0u), wctJsonUtf8, gameThreads);
  return std::vector<DWORD>(selected.begin(), selected.end());
}

std::uint64_t GameDelayStubAddress(HANDLE process)
{
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto stub = ntdll ? reinterpret_cast<std::uint64_t>(GetProcAddress(ntdll, "NtDelayExecution")) : 0u;
  if (!process || stub == 0u) {
    return 0;
  }
  std::array<std::uint8_t, 16> mine{};
  std::memcpy(mine.data(), reinterpret_cast<const void*>(stub), mine.size());
  std::array<std::uint8_t, 16> theirs{};
  if (!ReadChecked(process, stub, theirs.data(), theirs.size(), /*image=*/true) || mine != theirs) {
    return 0;
  }
  return stub;
}

std::vector<ExtraDumpMemory> CollectGameLockMemory(
  HANDLE process,
  const std::vector<CONTEXT>& delayThreads,
  std::chrono::steady_clock::time_point deadline,
  DumpObjectMemoryStats& stats)
{
  namespace locks = skydiag::game_locks;
  const auto start = std::chrono::steady_clock::now();
  stats.lock_threads = static_cast<std::uint32_t>(delayThreads.size());
  std::vector<ExtraDumpMemory> ranges;
  std::uint64_t base = 0;
  std::uint64_t end = 0;
  std::array<std::uint8_t, locks::kHeaderBytes> header{};
  if (delayThreads.empty() || !process || !GameImageRange(process, base, end) ||
      !ReadChecked(process, base, header.data(), header.size(), /*image=*/true)) {
    stats.lock_elapsed_us = ElapsedUs(start);
    return ranges;
  }
  auto writable = locks::WritableSections(header.data(), header.size(), base);
  std::erase_if(writable, [&](const locks::AddressRange& r) { return r.begin < base || r.end > end; });

  std::vector<std::vector<std::uint64_t>> values;
  for (const auto& c : delayThreads) {
    if (writable.empty()) {
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      stats.lock_budget_reached = true;
      break;
    }
    std::vector<std::uint64_t> threadValues = { c.Rbx, c.Rbp, c.Rsi, c.Rdi, c.R12, c.R13, c.R14, c.R15 };
    // The writer holds the thread suspended; its stack top is committed.
    std::array<std::uint64_t, locks::kStackBytes / 8> slots{};
    const std::size_t readable = ReadableBytes(process, c.Rsp, locks::kStackBytes, /*image=*/false) & ~std::size_t{ 7 };
    SIZE_T got = 0;
    if (readable != 0 &&
        ReadProcessMemory(process, reinterpret_cast<LPCVOID>(c.Rsp), slots.data(), readable, &got) && got == readable) {
      threadValues.insert(threadValues.end(), slots.begin(), slots.begin() + static_cast<std::ptrdiff_t>(readable / 8));
    }
    values.push_back(std::move(threadValues));
  }

  locks::WindowStats windowStats{};
  const auto windows = locks::SelectWindows(writable, values, &windowStats);
  stats.lock_pointers = windowStats.pointers;
  stats.lock_budget_reached = stats.lock_budget_reached || windowStats.budget_reached;
  for (const auto& window : windows) {
    if (std::chrono::steady_clock::now() >= deadline) {
      stats.lock_budget_reached = true;
      break;
    }
    if (ReadableBytes(process, window.base, window.size, /*image=*/true) == window.size) {
      ranges.push_back(ExtraDumpMemory{ window.base, window.size });
    }
  }
  if (!ranges.empty()) {
    // The analyzer learns from the header which sections are writable data.
    ranges.push_back(ExtraDumpMemory{ base, static_cast<std::uint32_t>(locks::kHeaderBytes) });
  }
  ranges = MergeExtraDumpMemory(std::move(ranges));
  stats.lock_ranges = static_cast<std::uint32_t>(ranges.size());
  stats.lock_bytes = TotalBytes(ranges);
  stats.lock_elapsed_us = ElapsedUs(start);
  return ranges;
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

void HangObjectMemoryCollector::EnableGameLocks(std::vector<std::uint32_t> gameThreads, std::uint64_t delayStub)
{
  lockThreads_ = std::move(gameThreads);
  delayStub_ = delayStub;
  stats_.lock_enabled = delayStub != 0u;
}

void HangObjectMemoryCollector::OnThreadContext(DWORD tid, const CONTEXT& context)
{
  const auto it = std::find(tids_.begin(), tids_.end(), tid);
  if (it != tids_.end()) {
    contexts_[static_cast<std::size_t>(it - tids_.begin())] = context;
  }
  if (skydiag::game_locks::InDelayStub(context.Rip, delayStub_) &&
      std::find(lockThreads_.begin(), lockThreads_.end(), tid) != lockThreads_.end()) {
    const auto known = std::find_if(lockContexts_.begin(), lockContexts_.end(),
      [tid](const std::pair<DWORD, CONTEXT>& entry) { return entry.first == tid; });
    if (known != lockContexts_.end()) {
      known->second = context;
    } else {
      lockContexts_.emplace_back(tid, context);
    }
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
  std::vector<ExtraDumpMemory> lockRanges;
  if (delayStub_ != 0u) {
    // First, and small: a deadlock on engine locks is told only from these.
    std::vector<CONTEXT> delayThreads;
    delayThreads.reserve(lockContexts_.size());
    for (const auto& entry : lockContexts_) {
      delayThreads.push_back(entry.second);
    }
    lockRanges = CollectGameLockMemory(process_, delayThreads, std::min(deadline, start + kGameLockTimeBudget), stats_);
  }
  std::vector<ExtraDumpMemory> ranges;
  std::uint64_t used = std::min<std::uint64_t>(stats_.lock_bytes, budgetBytes_);
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
  ranges = MergeExtraDumpMemory(std::move(ranges));
  stats_.ranges = static_cast<std::uint32_t>(ranges.size());
  stats_.bytes = TotalBytes(ranges);
  stats_.elapsed_us = ElapsedUs(start);
  if (std::chrono::steady_clock::now() >= deadline) {
    stats_.budget_reached = true;
  }
  ranges.insert(ranges.end(), lockRanges.begin(), lockRanges.end());
  ranges_ = MergeExtraDumpMemory(std::move(ranges));
  return ranges_;
}

}  // namespace skydiag::helper::internal
