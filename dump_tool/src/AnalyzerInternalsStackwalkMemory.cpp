#include "AnalyzerInternalsStackwalkPriv.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace skydiag::dump_tool::internal::stackwalk_internal {
namespace {

using skydiag::dump_tool::minidump::ReadStreamSized;

static thread_local const MinidumpMemoryView* g_stackwalkMemView = nullptr;

BOOL CALLBACK ReadProcessMemoryFromMinidump64(HANDLE, DWORD64 baseAddr, PVOID buffer, DWORD size, LPDWORD bytesRead)
{
  if (bytesRead) {
    *bytesRead = 0;
  }
  if (!g_stackwalkMemView || !buffer || size == 0) {
    return FALSE;
  }
  std::size_t got = 0;
  const bool ok = g_stackwalkMemView->Read(static_cast<std::uint64_t>(baseAddr), buffer, static_cast<std::size_t>(size), got);
  if (bytesRead) {
    *bytesRead = static_cast<DWORD>(got);
  }
  return ok ? TRUE : FALSE;
}

}  // namespace

bool MinidumpMemoryView::Init(void* dumpBase, std::uint64_t dumpSize, const std::vector<minidump::ThreadRecord>* threads)
{
  ranges.clear();
  if (!dumpBase || dumpSize < sizeof(MINIDUMP_HEADER)) {
    return false;
  }

  const auto* base = static_cast<const std::uint8_t*>(dumpBase);

  // Prefer Memory64ListStream for FullMemory dumps.
  {
    void* ptr = nullptr;
    ULONG size = 0;
    if (ReadStreamSized(dumpBase, dumpSize, Memory64ListStream, &ptr, &size) && ptr && size >= sizeof(MINIDUMP_MEMORY64_LIST)) {
      const auto* list = static_cast<const MINIDUMP_MEMORY64_LIST*>(ptr);
      const std::uint64_t need =
        static_cast<std::uint64_t>(offsetof(MINIDUMP_MEMORY64_LIST, MemoryRanges)) +
        static_cast<std::uint64_t>(list->NumberOfMemoryRanges) * static_cast<std::uint64_t>(sizeof(MINIDUMP_MEMORY_DESCRIPTOR64));
      if (need <= static_cast<std::uint64_t>(size)) {
        std::uint64_t cursor = list->BaseRva;
        ranges.reserve(static_cast<std::size_t>(list->NumberOfMemoryRanges));
        for (ULONG i = 0; i < list->NumberOfMemoryRanges; i++) {
          const auto& mr = list->MemoryRanges[i];
          if (mr.DataSize == 0) {
            continue;
          }
          if (cursor > dumpSize || mr.DataSize > (dumpSize - cursor)) {
            ranges.clear();
            break;
          }
          MinidumpMemoryRange r{};
          r.start = mr.StartOfMemoryRange;
          r.end = mr.StartOfMemoryRange + mr.DataSize;
          r.bytes = base + cursor;
          ranges.push_back(r);
          cursor += mr.DataSize;
        }
        if (!ranges.empty()) {
          std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
          return true;
        }
      }
    }
  }

  // Fallback: MemoryListStream (typically contains thread stacks).
  {
    void* ptr = nullptr;
    ULONG size = 0;
    if (!ReadStreamSized(dumpBase, dumpSize, MemoryListStream, &ptr, &size) || !ptr || size < sizeof(MINIDUMP_MEMORY_LIST)) {
      // Some dumps omit MemoryListStream but still include per-thread stack memory in ThreadListStream.
      if (!threads || threads->empty()) {
        return false;
      }
      ranges.reserve(threads->size());
      for (const auto& tr : *threads) {
        const std::uint64_t rva = static_cast<std::uint64_t>(tr.stack.Memory.Rva);
        const std::uint64_t sz = static_cast<std::uint64_t>(tr.stack.Memory.DataSize);
        if (rva == 0 || sz == 0) {
          continue;
        }
        if (rva > dumpSize || sz > (dumpSize - rva)) {
          continue;
        }
        MinidumpMemoryRange r{};
        r.start = tr.stack.StartOfMemoryRange;
        r.end = tr.stack.StartOfMemoryRange + sz;
        r.bytes = base + rva;
        ranges.push_back(r);
      }
      if (!ranges.empty()) {
        std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
      }
      return !ranges.empty();
    }

    const auto* list = static_cast<const MINIDUMP_MEMORY_LIST*>(ptr);
    const std::uint64_t need =
      static_cast<std::uint64_t>(offsetof(MINIDUMP_MEMORY_LIST, MemoryRanges)) +
      static_cast<std::uint64_t>(list->NumberOfMemoryRanges) * static_cast<std::uint64_t>(sizeof(MINIDUMP_MEMORY_DESCRIPTOR));
    if (need > static_cast<std::uint64_t>(size)) {
      return false;
    }

    ranges.reserve(static_cast<std::size_t>(list->NumberOfMemoryRanges));
    for (ULONG i = 0; i < list->NumberOfMemoryRanges; i++) {
      const auto& mr = list->MemoryRanges[i];
      const std::uint64_t rva = static_cast<std::uint64_t>(mr.Memory.Rva);
      const std::uint64_t sz = static_cast<std::uint64_t>(mr.Memory.DataSize);
      if (rva == 0 || sz == 0) {
        continue;
      }
      if (rva > dumpSize || sz > (dumpSize - rva)) {
        continue;
      }
      MinidumpMemoryRange r{};
      r.start = mr.StartOfMemoryRange;
      r.end = mr.StartOfMemoryRange + sz;
      r.bytes = base + rva;
      ranges.push_back(r);
    }

    if (!ranges.empty()) {
      std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    }
    return !ranges.empty();
  }
}

bool MinidumpMemoryView::Read(std::uint64_t addr, void* dst, std::size_t n, std::size_t& outRead) const
{
  outRead = 0;
  if (!dst || n == 0 || ranges.empty()) {
    return false;
  }

  const auto it = std::upper_bound(ranges.begin(), ranges.end(), addr, [](std::uint64_t value, const MinidumpMemoryRange& r) {
    return value < r.start;
  });
  if (it == ranges.begin()) {
    return false;
  }
  const auto& r = *(it - 1);
  if (addr < r.start || addr >= r.end || !r.bytes) {
    return false;
  }
  const std::uint64_t avail = r.end - addr;
  const std::size_t copyN = static_cast<std::size_t>(std::min<std::uint64_t>(avail, static_cast<std::uint64_t>(n)));
  std::memcpy(dst, r.bytes + static_cast<std::size_t>(addr - r.start), copyN);
  outRead = copyN;
  return copyN > 0;
}

std::vector<std::uint64_t> StackWalkAddrsForContext(
  HANDLE process,
  const MinidumpMemoryView& mem,
  const CONTEXT& inCtx,
  std::size_t maxFrames)
{
  std::vector<std::uint64_t> pcs;
  if (!process || maxFrames == 0) {
    return pcs;
  }

  CONTEXT ctx = inCtx;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = ctx.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = ctx.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = ctx.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;

  g_stackwalkMemView = &mem;
  const HANDLE thread = GetCurrentThread();

  constexpr std::size_t kMaxHard = 128;
  const std::size_t limit = std::min(maxFrames, kMaxHard);
  for (std::size_t i = 0; i < limit; i++) {
    const DWORD64 pc = frame.AddrPC.Offset;
    if (pc == 0) {
      break;
    }
    pcs.push_back(static_cast<std::uint64_t>(pc));

    const BOOL ok = StackWalk64(
      IMAGE_FILE_MACHINE_AMD64,
      process,
      thread,
      &frame,
      &ctx,
      ReadProcessMemoryFromMinidump64,
      SymFunctionTableAccess64,
      SymGetModuleBase64,
      nullptr);
    if (!ok) {
      break;
    }
    if (frame.AddrPC.Offset == pc) {
      break;
    }
  }

  g_stackwalkMemView = nullptr;
  return pcs;
}

}  // namespace skydiag::dump_tool::internal::stackwalk_internal

