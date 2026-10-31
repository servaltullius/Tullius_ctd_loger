#pragma once

#include <Windows.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace skydiag::helper::internal {

// ADR-0024: the hook stubs in the game's executable private memory, read
// while the game runs. A crash that jumps into freed code finds there whose
// hook the freed address was: the page itself is gone by then, and no dump
// holds it.
struct RecordedHookStub
{
  std::uint32_t offset = 0;
  std::uint32_t length = 0;
  std::string kind;
  std::uint64_t target = 0;
  std::wstring module;  // where the target lay when read; empty outside every module
  std::uint64_t moduleOffset = 0;
};

struct RecordedHookPage
{
  std::uint64_t base = 0;
  std::uint64_t size = 0;
  std::uint32_t protect = 0;
  std::vector<RecordedHookStub> stubs;
  std::uint32_t firstScan = 0;
  std::uint32_t lastScan = 0;
  std::uint64_t lastSeenTick = 0;
};

// What every scan found, merged: a page a later scan no longer finds keeps
// its last content (that is the record a freed address needs).
class HookPageRecord
{
public:
  static constexpr std::size_t kMaxPages = 512;

  // One scan's pages (those with stubs), read at nowTick.
  void Merge(std::vector<RecordedHookPage> scanned, std::uint64_t nowTick);
  std::uint32_t Scans() const;
  std::size_t PageCount() const;
  // The dump stream (kMinidumpUserStream_HookPages); empty before any scan.
  // Ages are counted back from nowTick.
  std::string ToJson(std::uint64_t nowTick) const;

private:
  mutable std::mutex mutex_;
  std::map<std::uint64_t, RecordedHookPage> pages_;
  std::uint32_t scans_ = 0;
  std::uint64_t lastScanTick_ = 0;
};

// The helper's record (one game process per helper).
HookPageRecord& HookPages();

// Executable private regions of up to 1 MB with jump stubs, their first
// 64 KB read and the targets named from the process's modules.
std::vector<RecordedHookPage> ScanHookPages(HANDLE process);

// Scans every kHookPageScanIntervalMs while the game runs; not while a crash
// is being handled. Returns true when it scanned.
inline constexpr std::uint64_t kHookPageScanIntervalMs = 30000;
bool MaybeRecordHookPages(HANDLE process, bool crashLatched, std::uint64_t nowTick, std::uint64_t* nextScanTick);

}  // namespace skydiag::helper::internal
