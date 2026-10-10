#include "CrashObjectMemory.h"

#include <algorithm>
#include <array>
#include <unordered_set>

#include "SkyrimDiagRtti.h"

namespace skydiag::helper::internal {
namespace {

// Bytes kept from the start of each object found, for fields read later.
constexpr std::uint32_t kObjectWindowBytes = 0x100;

class RecordingReader
{
public:
  explicit RecordingReader(HANDLE process) : process_(process) {}

  bool operator()(std::uint64_t address, void* destination, std::size_t size) const
  {
    SIZE_T got = 0;
    if (size == 0 || !ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(address), destination, size, &got) ||
        got != size) {
      return false;
    }
    reads_.push_back(ExtraDumpMemory{ address, static_cast<std::uint32_t>(size) });
    bytes_ += size;
    return true;
  }

  bool OverBudget() const { return bytes_ >= kCrashObjectMemoryBudget; }
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
  mutable std::vector<ExtraDumpMemory> reads_;
  mutable std::size_t bytes_ = 0;
};

std::vector<std::uint64_t> CollectRoots(HANDLE process, const CONTEXT& context)
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
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(context.Rsp + offset), &slots[offset / 8], 0x100, &got) ||
        got != 0x100) {
      break;  // the stack ends here
    }
    roots.insert(roots.end(), slots.begin() + static_cast<std::ptrdiff_t>(offset / 8),
      slots.begin() + static_cast<std::ptrdiff_t>((offset + 0x100) / 8));
  }
  return roots;
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
  if (!process) {
    return {};
  }
  RecordingReader reader(process);
  std::unordered_set<std::uint64_t> seen;
  std::array<std::uint8_t, kObjectWindowBytes> window{};
  for (const auto root : CollectRoots(process, context)) {
    if (reader.OverBudget()) {
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

}  // namespace skydiag::helper::internal
