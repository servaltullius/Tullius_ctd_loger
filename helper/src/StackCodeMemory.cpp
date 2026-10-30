#include "StackCodeMemory.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <limits>
#include <unordered_set>

#include "ModuleUnwindMemory.h"

namespace skydiag::helper::internal {
namespace {

// More sections than this is not a real image header.
constexpr std::uint16_t kMaxSections = 96;
constexpr std::size_t kMaxPdataBytes = std::size_t{ 16 } * 1024u * 1024u;
constexpr std::size_t kStackReadChunk = 0x10000;

bool ReadExact(HANDLE process, std::uint64_t address, void* destination, std::size_t size)
{
  SIZE_T got = 0;
  return size != 0 &&
    ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), destination, size, &got) && got == size;
}

std::uint32_t ElapsedUs(std::chrono::steady_clock::time_point start)
{
  return static_cast<std::uint32_t>(
    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}

std::uint64_t TotalBytes(const std::vector<ExtraDumpMemory>& ranges)
{
  std::uint64_t total = 0;
  for (const auto& range : ranges) {
    total += range.size;
  }
  return total;
}

// Committed and readable, never a guard page. Unlike IsReadableRegion, an
// executable page outside a module image counts: a RIP there is in a hook
// trampoline (ADR-0016). Used for the stacks too.
bool ReadableCodeRegion(const MEMORY_BASIC_INFORMATION& info)
{
  if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
    return false;
  }
  constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  return (info.Protect & kReadable) != 0;
}

// [address, end) cut to the readable regions it crosses.
void AddReadable(HANDLE process, std::uint64_t address, std::uint64_t end, std::vector<ExtraDumpMemory>& out)
{
  std::uint64_t at = address;
  while (at < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(at), &info, sizeof(info)) != sizeof(info) ||
        info.RegionSize == 0) {
      return;
    }
    const auto regionEnd = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    if (regionEnd <= at) {
      return;
    }
    const std::uint64_t pieceEnd = std::min(end, regionEnd);
    if (ReadableCodeRegion(info)) {
      out.push_back(ExtraDumpMemory{ at, static_cast<std::uint32_t>(pieceEnd - at) });
    }
    at = pieceEnd;
  }
}

// The executable sections and .pdata of the module at `base`, from its image
// in the process.
bool ReadCodeImage(HANDLE process, std::uint64_t base, std::uint32_t size, CodeImage& image)
{
  IMAGE_DOS_HEADER dos{};
  if (!ReadExact(process, base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
      static_cast<std::uint32_t>(dos.e_lfanew) > 0x1000u) {
    return false;
  }
  const std::uint64_t ntAddress = base + static_cast<std::uint32_t>(dos.e_lfanew);
  IMAGE_NT_HEADERS64 nt{};
  if (!ReadExact(process, ntAddress, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.FileHeader.NumberOfSections == 0 ||
      nt.FileHeader.NumberOfSections > kMaxSections) {
    return false;
  }
  std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
  const std::uint64_t sectionsAddress =
    ntAddress + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
  if (!ReadExact(process, sectionsAddress, sections.data(), sections.size() * sizeof(IMAGE_SECTION_HEADER))) {
    return false;
  }

  image = CodeImage{};
  image.base = base;
  image.size = size;
  for (const auto& section : sections) {
    if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0u || section.VirtualAddress >= size) {
      continue;
    }
    const std::uint32_t length = section.Misc.VirtualSize != 0 ? section.Misc.VirtualSize : section.SizeOfRawData;
    const std::uint64_t end = std::min<std::uint64_t>(std::uint64_t{ section.VirtualAddress } + length, size);
    image.executable.emplace_back(section.VirtualAddress, static_cast<std::uint32_t>(end));
  }

  const auto& pdata = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if (pdata.VirtualAddress != 0 && pdata.Size >= sizeof(RUNTIME_FUNCTION) && pdata.Size <= kMaxPdataBytes &&
      std::uint64_t{ pdata.VirtualAddress } + pdata.Size <= size) {
    std::vector<RUNTIME_FUNCTION> functions(pdata.Size / sizeof(RUNTIME_FUNCTION));
    if (ReadExact(process, base + pdata.VirtualAddress, functions.data(), functions.size() * sizeof(RUNTIME_FUNCTION))) {
      std::sort(functions.begin(), functions.end(), [](const RUNTIME_FUNCTION& a, const RUNTIME_FUNCTION& b) {
        return a.BeginAddress < b.BeginAddress;
      });
      image.functions = std::move(functions);
    }
  }
  return true;
}

// Calls onValue with each 8-byte value from RSP to the top of the stack (at
// most kMaxStackScanBytes). Like the other collectors it reads only
// committed, readable regions, never a guard page; an unreadable region is
// skipped whole and the scan goes on above it.
template <typename OnValue>
void ScanStack(
  HANDLE process,
  std::uint64_t rsp,
  std::uint64_t low,
  std::uint64_t high,
  std::chrono::steady_clock::time_point deadline,
  std::vector<std::uint8_t>& buffer,
  OnValue&& onValue)
{
  if (high <= low) {
    return;
  }
  std::uint64_t at = (rsp >= low && rsp < high) ? rsp : low;
  at = (at + 7u) & ~std::uint64_t{ 7 };
  const std::uint64_t end = std::min<std::uint64_t>(high, at + kMaxStackScanBytes);
  buffer.resize(kStackReadChunk);
  while (at < end && std::chrono::steady_clock::now() < deadline) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(at), &info, sizeof(info)) != sizeof(info) ||
        info.RegionSize == 0) {
      return;
    }
    const auto regionEnd = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    if (regionEnd <= at) {
      return;
    }
    if (!ReadableCodeRegion(info)) {
      at = regionEnd;
      continue;
    }
    const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>({ kStackReadChunk, end - at, regionEnd - at }));
    if (!ReadExact(process, at, buffer.data(), chunk)) {
      at += chunk;
      continue;
    }
    for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= chunk; offset += sizeof(std::uint64_t)) {
      std::uint64_t value = 0;
      std::memcpy(&value, buffer.data() + offset, sizeof(value));
      onValue(value);
    }
    at += chunk;
  }
}

}  // namespace

bool StackCodeRanges(const CodeImage& image, std::uint64_t value, bool isRip, std::vector<ExtraDumpMemory>& out)
{
  if (value < image.base || value - image.base >= image.size) {
    return false;
  }
  const auto rva = static_cast<std::uint32_t>(value - image.base);
  const bool executable = std::any_of(image.executable.begin(), image.executable.end(), [rva](const auto& section) {
    return rva >= section.first && rva < section.second;
  });
  if (!executable) {
    return false;
  }

  const std::uint64_t before = std::min<std::uint64_t>(isRip ? kRipWindowBytes : kReturnWindowBefore, rva);
  const std::uint64_t after = std::min<std::uint64_t>(isRip ? kRipWindowBytes : kReturnWindowAfter, image.size - rva);
  out.push_back(ExtraDumpMemory{ value - before, static_cast<std::uint32_t>(before + after) });

  const auto find = [&image](std::uint32_t at) -> const RUNTIME_FUNCTION* {
    auto it = std::upper_bound(image.functions.begin(), image.functions.end(), at,
      [](std::uint32_t address, const RUNTIME_FUNCTION& function) { return address < function.BeginAddress; });
    if (it == image.functions.begin()) {
      return nullptr;
    }
    --it;
    return at < it->EndAddress ? &*it : nullptr;
  };
  const RUNTIME_FUNCTION* function = (!isRip && rva != 0) ? find(rva - 1u) : nullptr;
  if (!function) {
    function = find(rva);
  }
  if (function && function->EndAddress > function->BeginAddress && function->EndAddress <= image.size &&
      function->EndAddress - function->BeginAddress <= kMaxStackCodeFunctionBytes) {
    out.push_back(ExtraDumpMemory{ image.base + function->BeginAddress, function->EndAddress - function->BeginAddress });
  }
  return true;
}

StackCodeMemoryCollector::StackCodeMemoryCollector(
  HANDLE process,
  std::uint64_t gameImageBase,
  std::vector<DWORD> preferredThreads,
  std::uint64_t exceptionRip,
  std::chrono::milliseconds timeBudget)
  : process_(process),
    gameImageBase_(gameImageBase),
    preferredThreads_(std::move(preferredThreads)),
    exceptionRip_(exceptionRip),
    timeBudget_(timeBudget)
{}

void StackCodeMemoryCollector::OnModule(std::uint64_t base, std::uint32_t size, const wchar_t* path)
{
  if (collected_ || base == 0 || size == 0) {
    return;
  }
  modules_.push_back(Module{ base, size, path ? std::wstring(path) : std::wstring() });
}

void StackCodeMemoryCollector::OnThread(DWORD tid, const CONTEXT& context, std::uint64_t stackBase, std::uint64_t stackEnd)
{
  if (collected_) {
    return;
  }
  Thread thread{};
  thread.tid = tid;
  thread.rip = context.Rip;
  thread.rsp = context.Rsp;
  thread.stackLow = std::min(stackBase, stackEnd);
  thread.stackHigh = std::max(stackBase, stackEnd);
  const auto existing =
    std::find_if(threads_.begin(), threads_.end(), [tid](const Thread& other) { return other.tid == tid; });
  if (existing != threads_.end()) {
    *existing = thread;
  } else {
    threads_.push_back(thread);
  }
}

const std::vector<ExtraDumpMemory>& StackCodeMemoryCollector::Collect()
{
  if (collected_) {
    return ranges_;
  }
  collected_ = true;
  stats_.collected = true;
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + timeBudget_;

  std::sort(modules_.begin(), modules_.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
  modules_.erase(
    std::unique(modules_.begin(), modules_.end(), [](const Module& a, const Module& b) { return a.base == b.base; }),
    modules_.end());
  constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
  const auto moduleAt = [this](std::uint64_t address) -> std::size_t {
    auto it = std::upper_bound(modules_.begin(), modules_.end(), address,
      [](std::uint64_t value, const Module& module) { return value < module.base; });
    if (it == modules_.begin()) {
      return kNone;
    }
    --it;
    return address - it->base < it->size ? static_cast<std::size_t>(it - modules_.begin()) : kNone;
  };

  // Headers, exports and unwind data: the game's first, then each module's
  // as the threads below first point into it. The Windows modules come with
  // ADR-0011's collection already.
  const std::wstring windowsPrefix = WindowsDirectoryPrefix();
  std::vector<ExtraDumpMemory> moduleData;
  std::uint64_t moduleBytes = 0;
  const auto addModuleData = [&](std::size_t index) {
    const Module& module = modules_[index];
    if (!windowsPrefix.empty() && _wcsnicmp(module.path.c_str(), windowsPrefix.c_str(), windowsPrefix.size()) == 0) {
      return;
    }
    std::vector<ExtraDumpMemory> pieces;
    const std::size_t bytes = AddModuleUnwindMemory(process_, module.base, module.size, pieces);
    if (bytes == 0) {
      return;
    }
    if (moduleBytes + bytes > kStackModuleMemoryBudget) {
      stats_.budget_reached = true;  // a smaller module later may still fit
      return;
    }
    moduleBytes += bytes;
    ++stats_.modules;
    moduleData.insert(moduleData.end(), pieces.begin(), pieces.end());
  };
  std::size_t game = moduleAt(gameImageBase_);
  if (game != kNone && modules_[game].base != gameImageBase_) {
    game = kNone;
  }
  if (game != kNone) {
    addModuleData(game);
  }

  std::vector<CodeImage> images(modules_.size());
  std::vector<std::uint8_t> imageState(modules_.size(), 0);  // 0 unread, 1 read, 2 unreadable
  std::vector<std::size_t> pointedInto;
  std::vector<std::uint8_t> isPointedInto(modules_.size(), 0);
  std::unordered_set<std::uint64_t> seenRips;
  std::unordered_set<std::uint64_t> seenValues;
  std::unordered_set<std::uint64_t> keptFunctions;
  std::vector<ExtraDumpMemory> code;
  std::uint64_t codeBytes = 0;
  std::vector<ExtraDumpMemory> pieces;
  const auto keep = [&]() {
    for (const auto& piece : pieces) {
      if (codeBytes + piece.size > kStackCodeMemoryBudget) {
        stats_.budget_reached = true;
        return;
      }
      code.push_back(piece);
      codeBytes += piece.size;
    }
  };
  const auto examine = [&](std::uint64_t value, bool isRip) {
    if (!(isRip ? seenRips : seenValues).insert(value).second) {
      return;
    }
    pieces.clear();
    const std::size_t index = moduleAt(value);
    if (index != kNone && imageState[index] == 0) {
      imageState[index] = ReadCodeImage(process_, modules_[index].base, modules_[index].size, images[index]) ? 1 : 2;
    }
    if (index == kNone || imageState[index] != 1 || !StackCodeRanges(images[index], value, isRip, pieces)) {
      if (isRip) {
        // Not in module code (a hook trampoline, data run by mistake, an
        // image whose header cannot be read): kept where readable.
        pieces.clear();
        const std::uint64_t begin = value - std::min<std::uint64_t>(value, kRipWindowBytes);
        const std::uint64_t end =
          value > std::numeric_limits<std::uint64_t>::max() - kRipWindowBytes ? value : value + kRipWindowBytes;
        AddReadable(process_, begin, end, pieces);
        keep();
      }
      return;
    }
    ++stats_.values;
    if (isPointedInto[index] == 0) {
      isPointedInto[index] = 1;
      pointedInto.push_back(index);
    }
    if (index == game) {
      return;  // the game's code is written whole
    }
    // Many values share a function: keep (and count) it once.
    if (pieces.size() > 1u && !keptFunctions.insert(pieces[1].base).second) {
      pieces.pop_back();
    }
    keep();
  };

  std::vector<const Thread*> order;
  order.reserve(threads_.size());
  for (const DWORD tid : preferredThreads_) {
    const auto it = std::find_if(threads_.begin(), threads_.end(), [tid](const Thread& t) { return t.tid == tid; });
    if (it != threads_.end() && std::find(order.begin(), order.end(), &*it) == order.end()) {
      order.push_back(&*it);
    }
  }
  for (const auto& thread : threads_) {
    if (std::find(order.begin(), order.end(), &thread) == order.end()) {
      order.push_back(&thread);
    }
  }

  // The module data of each thread's modules goes in right after its scan,
  // so a deadline drops the threads that matter least, not every module.
  std::size_t modulesDone = 0;
  const auto addNewModuleData = [&]() {
    for (; modulesDone < pointedInto.size(); ++modulesDone) {
      if (pointedInto[modulesDone] != game) {
        addModuleData(pointedInto[modulesDone]);
      }
    }
  };
  if (exceptionRip_ != 0) {
    examine(exceptionRip_, true);
    addNewModuleData();
  }
  std::vector<std::uint8_t> stackBuffer;
  for (const Thread* thread : order) {
    if (std::chrono::steady_clock::now() >= deadline) {
      stats_.budget_reached = true;
      break;
    }
    ++stats_.threads;
    examine(thread->rip, true);
    ScanStack(process_, thread->rsp, thread->stackLow, thread->stackHigh, deadline, stackBuffer,
      [&](std::uint64_t value) { examine(value, false); });
    addNewModuleData();
  }

  code = MergeExtraDumpMemory(std::move(code));
  stats_.code_ranges = static_cast<std::uint32_t>(code.size());
  stats_.code_bytes = TotalBytes(code);
  stats_.module_bytes = moduleBytes;
  code.insert(code.end(), moduleData.begin(), moduleData.end());
  ranges_ = MergeExtraDumpMemory(std::move(code));
  stats_.elapsed_us = ElapsedUs(start);
  return ranges_;
}

}  // namespace skydiag::helper::internal
