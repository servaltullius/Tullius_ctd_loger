#include "CrashObjectMemory.h"

#include <algorithm>
#include <array>
#include <unordered_set>
#include <utility>

#include "SkyrimDiagRtti.h"

namespace skydiag::helper::internal {
namespace {

// Bytes kept from the start of each object found, for fields read later.
constexpr std::uint32_t kObjectWindowBytes = 0x100;
// Regions PageCheck remembers before it starts over.
constexpr std::size_t kMaxCachedRegions = 64;

// Committed and readable, and never a guard page: a read there would take the
// guard away (a thread stack could then no longer grow). Executable pages
// count only inside a module image, where a hook may have left the RTTI in
// .rdata writable and executable; elsewhere they hold no objects.
bool ReadableRegion(const MEMORY_BASIC_INFORMATION& info)
{
  if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
    return false;
  }
  const DWORD access = info.Protect & 0xFFu;
  constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  if ((access & kExecutable) != 0) {
    return info.Type == MEM_IMAGE && access != PAGE_EXECUTE;
  }
  return (access & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)) != 0;
}

// Which pages of a process a read may touch, from VirtualQueryEx, remembered
// per region.
class PageCheck
{
public:
  explicit PageCheck(HANDLE process) : process_(process) {}

  bool Readable(std::uint64_t address, std::size_t size)
  {
    if (size == 0 || address + size < address) {
      return false;
    }
    const std::uint64_t end = address + size;
    for (std::uint64_t at = address; at < end;) {
      const Region* region = Find(at);
      if (!region) {
        region = Query(at);
      }
      if (!region || !region->readable) {
        return false;
      }
      at = region->end;
    }
    return true;
  }

private:
  struct Region
  {
    std::uint64_t base = 0;
    std::uint64_t end = 0;
    bool readable = false;
  };

  const Region* Find(std::uint64_t address) const
  {
    for (const auto& region : regions_) {
      if (address >= region.base && address < region.end) {
        return &region;
      }
    }
    return nullptr;
  }

  const Region* Query(std::uint64_t address)
  {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQueryEx(process_, reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) != sizeof(info) ||
        info.RegionSize == 0) {
      return nullptr;
    }
    if (regions_.size() >= kMaxCachedRegions) {
      regions_.clear();
    }
    const auto base = reinterpret_cast<std::uint64_t>(info.BaseAddress);
    regions_.push_back(Region{ base, base + info.RegionSize, ReadableRegion(info) });
    return &regions_.back();
  }

  HANDLE process_;
  std::vector<Region> regions_;
};

class RecordingReader
{
public:
  // pages: null reads without checking the pages first (a crashed game).
  RecordingReader(HANDLE process, std::size_t budget, PageCheck* pages)
    : process_(process), budget_(budget), pages_(pages)
  {}

  bool operator()(std::uint64_t address, void* destination, std::size_t size) const
  {
    if (size == 0 || bytes_ + size > budget_ || (pages_ && !pages_->Readable(address, size))) {
      return false;
    }
    SIZE_T got = 0;
    if (!ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(address), destination, size, &got) || got != size) {
      return false;
    }
    reads_.push_back(ExtraDumpMemory{ address, static_cast<std::uint32_t>(size) });
    bytes_ += size;
    return true;
  }

  bool OverBudget() const { return bytes_ >= budget_; }
  std::size_t Mark() const { return reads_.size(); }
  // Forgets the reads made since `mark` (a value that was not an object).
  void Rewind(std::size_t mark) const
  {
    for (std::size_t i = mark; i < reads_.size(); ++i) {
      bytes_ -= reads_[i].size;
    }
    reads_.resize(mark);
  }
  std::vector<ExtraDumpMemory>& Reads() const { return reads_; }

private:
  HANDLE process_;
  std::size_t budget_;
  PageCheck* pages_;
  mutable std::vector<ExtraDumpMemory> reads_;
  mutable std::size_t bytes_ = 0;
};

std::vector<std::uint64_t> CollectRoots(HANDLE process, const CONTEXT& context, PageCheck* pages)
{
  const std::array<DWORD64, 15> registers = {
    context.Rax, context.Rbx, context.Rcx, context.Rdx, context.Rsi, context.Rdi, context.Rbp, context.R8,
    context.R9,  context.R10, context.R11, context.R12, context.R13, context.R14, context.R15,
  };
  std::vector<std::uint64_t> roots(registers.begin(), registers.end());

  // The stack itself is already in the dump; only its values are needed here.
  std::array<std::uint64_t, kCrashObjectStackBytes / 8> slots{};
  for (std::size_t offset = 0; offset < kCrashObjectStackBytes; offset += 0x100) {
    SIZE_T got = 0;
    if ((pages && !pages->Readable(context.Rsp + offset, 0x100)) ||
        !ReadProcessMemory(process, reinterpret_cast<LPCVOID>(context.Rsp + offset), &slots[offset / 8], 0x100, &got) ||
        got != 0x100) {
      break;  // the stack ends here
    }
    roots.insert(roots.end(), slots.begin() + static_cast<std::ptrdiff_t>(offset / 8),
      slots.begin() + static_cast<std::ptrdiff_t>((offset + 0x100) / 8));
  }
  return roots;
}

std::vector<ExtraDumpMemory> CollectObjectMemory(
  HANDLE process,
  const CONTEXT& context,
  std::size_t budget,
  std::chrono::steady_clock::time_point deadline,
  PageCheck* pages)
{
  if (!process) {
    return {};
  }
  RecordingReader reader(process, budget, pages);
  const bool timed = deadline != std::chrono::steady_clock::time_point::max();
  std::unordered_set<std::uint64_t> seen;
  std::array<std::uint8_t, kObjectWindowBytes> window{};
  for (const auto root : CollectRoots(process, context, pages)) {
    if (reader.OverBudget() || (timed && std::chrono::steady_clock::now() >= deadline)) {
      break;
    }
    if (!seen.insert(root).second) {
      continue;
    }
    const auto mark = reader.Mark();
    const auto object = skydiag::rtti::ReadObject(reader, root);
    if (!object) {
      reader.Rewind(mark);
      continue;
    }
    // Keep the start of the object as well; a shorter read near the end of
    // its allocation is better than none.
    if (!reader(object->complete_object, window.data(), window.size())) {
      reader(object->complete_object, window.data(), 0x40);
    }
  }
  return MergeExtraDumpMemory(std::move(reader.Reads()));
}

}  // namespace

std::vector<ExtraDumpMemory> MergeExtraDumpMemory(std::vector<ExtraDumpMemory> ranges)
{
  std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.base < b.base; });
  std::vector<ExtraDumpMemory> merged;
  for (const auto& range : ranges) {
    if (range.size == 0) {
      continue;
    }
    if (!merged.empty()) {
      auto& last = merged.back();
      const std::uint64_t lastEnd = last.base + last.size;
      if (range.base <= lastEnd) {
        const std::uint64_t end = std::max(lastEnd, range.base + range.size);
        if (end - last.base <= 0xFFFFFFFFull) {
          last.size = static_cast<std::uint32_t>(end - last.base);
          continue;
        }
      }
    }
    merged.push_back(range);
  }
  return merged;
}

std::vector<ExtraDumpMemory> CollectCrashObjectMemory(HANDLE process, const CONTEXT& context)
{
  // The game is stopped in its crash handler and will not run on.
  return CollectObjectMemory(
    process, context, kCrashObjectMemoryBudget, std::chrono::steady_clock::time_point::max(), /*pages=*/nullptr);
}

std::vector<ExtraDumpMemory> CollectThreadObjectMemory(
  HANDLE process,
  const CONTEXT& context,
  std::size_t budgetBytes,
  std::chrono::steady_clock::time_point deadline)
{
  PageCheck pages(process);
  return CollectObjectMemory(process, context, budgetBytes, deadline, &pages);
}

}  // namespace skydiag::helper::internal
