#include "HookPageRecord.h"

#include <TlHelp32.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <iterator>

#include <nlohmann/json.hpp>

#include "HelperCommon.h"
#include "SkyrimDiagHookStubs.h"

namespace skydiag::helper::internal {
namespace {

struct ModuleRange
{
  std::uint64_t base = 0;
  std::uint64_t end = 0;
  std::wstring name;
};

// One Toolhelp snapshot reads the game's loader list once. GetModuleInformation
// and GetModuleBaseNameW walk it again for each module: with some 600 modules
// a scan took 140-280 ms in game (2026-10-31).
std::vector<ModuleRange> ListModules(HANDLE process)
{
  const DWORD pid = GetProcessId(process);
  HANDLE snapshot = INVALID_HANDLE_VALUE;
  // ERROR_BAD_LENGTH while a module is being loaded: try again.
  for (int attempt = 0; attempt < 3 && snapshot == INVALID_HANDLE_VALUE; ++attempt) {
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
  }
  if (snapshot == INVALID_HANDLE_VALUE) {
    return {};
  }
  std::vector<ModuleRange> modules;
  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry)) {
    const auto base = reinterpret_cast<std::uint64_t>(entry.modBaseAddr);
    modules.push_back({ base, base + entry.modBaseSize, entry.szModule });
  }
  CloseHandle(snapshot);
  std::sort(modules.begin(), modules.end(), [](const auto& a, const auto& b) { return a.base < b.base; });
  return modules;
}

const ModuleRange* ModuleOf(const std::vector<ModuleRange>& modules, std::uint64_t address)
{
  auto it = std::upper_bound(modules.begin(), modules.end(), address, [](std::uint64_t value, const ModuleRange& m) {
    return value < m.base;
  });
  if (it == modules.begin()) {
    return nullptr;
  }
  --it;
  return address < it->end ? &*it : nullptr;
}

bool IsHookPageCandidate(const MEMORY_BASIC_INFORMATION& mbi)
{
  constexpr DWORD kExecute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  return mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & kExecute) != 0u &&
    (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0u;
}

std::string HexText(std::uint64_t value)
{
  char text[24]{};
  std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
  return text;
}

}  // namespace

void HookPageRecord::Merge(std::vector<RecordedHookPage> scanned, std::uint64_t nowTick)
{
  std::lock_guard lock(mutex_);
  ++scans_;
  lastScanTick_ = nowTick;
  for (auto& page : scanned) {
    if (page.stubs.empty()) {
      continue;
    }
    page.firstScan = scans_;
    // A recorded page this one overlaps was freed and its memory used again:
    // the new content is the one that counts. The same page keeps its first
    // scan.
    for (auto it = pages_.begin(); it != pages_.end();) {
      const auto& old = it->second;
      if (old.base < page.base + page.size && page.base < old.base + old.size) {
        if (old.base == page.base) {
          page.firstScan = old.firstScan;
        }
        it = pages_.erase(it);
      } else {
        ++it;
      }
    }
    page.lastScan = scans_;
    page.lastSeenTick = nowTick;
    const auto key = page.base;
    pages_.emplace(key, std::move(page));
  }
  // Over the cap, the pages longest gone go first.
  while (pages_.size() > kMaxPages) {
    pages_.erase(std::min_element(pages_.begin(), pages_.end(), [](const auto& a, const auto& b) {
      return a.second.lastScan < b.second.lastScan;
    }));
  }
}

std::uint32_t HookPageRecord::Scans() const
{
  std::lock_guard lock(mutex_);
  return scans_;
}

std::size_t HookPageRecord::PageCount() const
{
  std::lock_guard lock(mutex_);
  return pages_.size();
}

std::string HookPageRecord::ToJson(std::uint64_t nowTick) const
{
  std::lock_guard lock(mutex_);
  if (scans_ == 0u) {
    return {};
  }
  const auto ago = [&](std::uint64_t tick) { return nowTick >= tick ? nowTick - tick : 0u; };
  nlohmann::json pages = nlohmann::json::array();
  for (const auto& [base, page] : pages_) {
    nlohmann::json stubs = nlohmann::json::array();
    for (const auto& stub : page.stubs) {
      stubs.push_back({
        { "offset", stub.offset },
        { "length", stub.length },
        { "kind", stub.kind },
        { "target", HexText(stub.target) },
        { "module", WideToUtf8(stub.module) },
        { "module_offset", HexText(stub.moduleOffset) },
      });
    }
    pages.push_back({
      { "base", HexText(page.base) },
      { "size", page.size },
      { "protect", page.protect },
      { "first_scan", page.firstScan },
      { "last_scan", page.lastScan },
      { "last_seen_ms_ago", ago(page.lastSeenTick) },
      { "stubs", std::move(stubs) },
    });
  }
  const nlohmann::json root = {
    { "version", 1 },
    { "scans", scans_ },
    { "last_scan_ms_ago", ago(lastScanTick_) },
    { "pages", std::move(pages) },
  };
  return root.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

HookPageRecord& HookPages()
{
  static HookPageRecord record;
  return record;
}

std::vector<RecordedHookPage> ScanHookPages(HANDLE process)
{
  constexpr std::uint64_t kMaxRegionBytes = std::uint64_t{ 1024 } * 1024u;  // larger: a JIT heap, not a trampoline
  // SKSE's branch pool, which CommonLib-NG plugins take their trampolines
  // from while it has room, is one 64 KB page of many plugins' stubs.
  constexpr SIZE_T kReadBytes = SIZE_T{ 64 } * 1024u;
  constexpr std::size_t kMaxStubsPerPage = 4096;
  std::vector<RecordedHookPage> pages;
  if (!process) {
    return pages;
  }
  std::vector<ModuleRange> modules;
  bool modulesListed = false;
  std::vector<std::uint8_t> buffer(kReadBytes);
  std::uint64_t address = 0;
  for (int regions = 0; regions < 1000000; ++regions) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) {
      break;
    }
    const auto base = reinterpret_cast<std::uint64_t>(mbi.BaseAddress);
    const auto size = static_cast<std::uint64_t>(mbi.RegionSize);
    if (IsHookPageCandidate(mbi) && size <= kMaxRegionBytes) {
      SIZE_T read = 0;
      const SIZE_T want = static_cast<SIZE_T>(std::min<std::uint64_t>(size, kReadBytes));
      // A partial read (part of the region decommitted meanwhile) still counts.
      if (ReadProcessMemory(process, mbi.BaseAddress, buffer.data(), want, &read) || read > 0u) {
        const auto stubs = DecodeHookStubs(buffer.data(), read, kMaxStubsPerPage);
        if (!stubs.empty()) {
          if (!modulesListed) {
            modules = ListModules(process);
            modulesListed = true;
          }
          RecordedHookPage page{};
          page.base = base;
          page.size = size;
          page.protect = mbi.Protect;
          page.stubs.reserve(stubs.size());
          for (const auto& stub : stubs) {
            RecordedHookStub recorded{};
            recorded.offset = stub.offset;
            recorded.length = stub.length;
            recorded.kind = stub.kind;
            recorded.target = stub.target;
            if (const auto* module = ModuleOf(modules, stub.target)) {
              recorded.module = module->name;
              recorded.moduleOffset = stub.target - module->base;
            }
            page.stubs.push_back(std::move(recorded));
          }
          pages.push_back(std::move(page));
        }
      }
    }
    const auto next = base + size;
    if (size == 0u || next <= address) {
      break;
    }
    address = next;
  }
  return pages;
}

bool MaybeRecordHookPages(HANDLE process, bool crashLatched, std::uint64_t nowTick, std::uint64_t* nextScanTick)
{
  if (!process || !nextScanTick || crashLatched || nowTick < *nextScanTick) {
    return false;
  }
  *nextScanTick = nowTick + kHookPageScanIntervalMs;
  if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
    return false;  // the game has exited: nothing left to read
  }
  // Best effort: a failed scan (out of memory) must not stop the helper.
  try {
    HookPages().Merge(ScanHookPages(process), nowTick);
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

}  // namespace skydiag::helper::internal
