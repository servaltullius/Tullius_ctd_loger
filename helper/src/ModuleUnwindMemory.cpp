#include "ModuleUnwindMemory.h"

#include <Psapi.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <string>
#include <unordered_set>

namespace skydiag::helper::internal {
namespace {

constexpr std::uint8_t kUnwFlagEHandler = 0x1;
constexpr std::uint8_t kUnwFlagUHandler = 0x2;
constexpr std::uint8_t kUnwFlagChainInfo = 0x4;
constexpr std::size_t kMaxPdataBytes = std::size_t{ 16 } * 1024u * 1024u;
constexpr std::size_t kMaxUnwindSpanBytes = std::size_t{ 32 } * 1024u * 1024u;

bool ReadExact(HANDLE process, std::uint64_t address, void* destination, std::size_t size)
{
  SIZE_T got = 0;
  return size != 0 &&
    ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), destination, size, &got) && got == size;
}

std::wstring Lower(std::wstring text)
{
  std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return text;
}

}  // namespace

std::wstring WindowsDirectoryPrefix()
{
  wchar_t buffer[MAX_PATH]{};
  const UINT length = GetWindowsDirectoryW(buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  std::wstring prefix = Lower(std::wstring(buffer, length));
  if (prefix.back() != L'\\') {
    prefix.push_back(L'\\');
  }
  return prefix;
}

std::size_t AddModuleUnwindMemory(
  HANDLE process,
  std::uint64_t base,
  std::uint32_t imageSize,
  std::vector<ExtraDumpMemory>& out)
{
  IMAGE_DOS_HEADER dos{};
  if (!ReadExact(process, base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
      static_cast<std::uint32_t>(dos.e_lfanew) > 0x1000u) {
    return 0;
  }
  IMAGE_NT_HEADERS64 nt{};
  if (!ReadExact(process, base + static_cast<std::uint32_t>(dos.e_lfanew), &nt, sizeof(nt)) ||
      nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    return 0;
  }
  std::size_t added = 0;
  const auto add = [&](std::uint64_t address, std::uint32_t size) {
    out.push_back(ExtraDumpMemory{ address, size });
    added += size;
  };
  const std::uint32_t headerBytes = std::min<std::uint32_t>(nt.OptionalHeader.SizeOfHeaders, 0x1000u);
  add(base, headerBytes);

  const auto& exports = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (exports.VirtualAddress != 0 && exports.Size != 0 &&
      static_cast<std::uint64_t>(exports.VirtualAddress) + exports.Size <= imageSize) {
    // The export directory's name, address and ordinal tables lie inside it
    // for the linker's usual layout; the whole directory is what is added.
    add(base + exports.VirtualAddress, exports.Size);
  }

  const auto& pdata = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if (pdata.VirtualAddress == 0 || pdata.Size < sizeof(RUNTIME_FUNCTION) || pdata.Size > kMaxPdataBytes ||
      static_cast<std::uint64_t>(pdata.VirtualAddress) + pdata.Size > imageSize) {
    return added;
  }
  std::vector<RUNTIME_FUNCTION> functions(pdata.Size / sizeof(RUNTIME_FUNCTION));
  if (!ReadExact(process, base + pdata.VirtualAddress, functions.data(), functions.size() * sizeof(RUNTIME_FUNCTION))) {
    return added;
  }
  add(base + pdata.VirtualAddress, static_cast<std::uint32_t>(functions.size() * sizeof(RUNTIME_FUNCTION)));

  // Read the unwind data in one piece and work out each entry's size from it.
  std::uint32_t low = UINT32_MAX;
  std::uint32_t high = 0;
  for (const auto& function : functions) {
    if ((function.UnwindData & 1u) != 0u || function.UnwindData == 0) {
      continue;  // points at another .pdata entry, already included
    }
    low = std::min(low, static_cast<std::uint32_t>(function.UnwindData));
    high = std::max(high, static_cast<std::uint32_t>(function.UnwindData));
  }
  if (low > high) {
    return added;
  }
  const std::uint64_t spanEnd = std::min<std::uint64_t>(static_cast<std::uint64_t>(high) + 0x200u, imageSize);
  if (spanEnd <= low || spanEnd - low > kMaxUnwindSpanBytes) {
    return added;
  }
  std::vector<std::uint8_t> span(static_cast<std::size_t>(spanEnd - low));
  if (!ReadExact(process, base + low, span.data(), span.size())) {
    return added;
  }
  for (const auto& range : UnwindInfoRanges(base, functions, low, span)) {
    add(range.base, range.size);
  }
  return added;
}

std::vector<ExtraDumpMemory> UnwindInfoRanges(
  std::uint64_t imageBase,
  const std::vector<RUNTIME_FUNCTION>& functions,
  std::uint32_t spanRva,
  const std::vector<std::uint8_t>& span)
{
  std::vector<ExtraDumpMemory> ranges;
  std::unordered_set<std::uint32_t> seen;
  const auto sizeAt = [&](std::uint32_t rva, std::uint32_t* chained) -> std::uint32_t {
    *chained = 0;
    if (rva < spanRva || static_cast<std::uint64_t>(rva - spanRva) + 4u > span.size()) {
      return 0;
    }
    const std::uint8_t* info = span.data() + (rva - spanRva);
    const std::uint8_t flags = static_cast<std::uint8_t>(info[0] >> 3);
    const std::uint32_t codes = info[2];
    std::uint32_t size = 4u + 2u * ((codes + 1u) & ~1u);
    if ((flags & kUnwFlagChainInfo) != 0u) {
      if (static_cast<std::uint64_t>(rva - spanRva) + size + sizeof(RUNTIME_FUNCTION) <= span.size()) {
        RUNTIME_FUNCTION next{};
        std::memcpy(&next, info + size, sizeof(next));
        *chained = static_cast<std::uint32_t>(next.UnwindData);
      }
      size += sizeof(RUNTIME_FUNCTION);
    } else if ((flags & (kUnwFlagEHandler | kUnwFlagUHandler)) != 0u) {
      size += 4u;  // handler RVA; its language-specific data is not needed to unwind
    }
    return size;
  };
  for (const auto& function : functions) {
    auto rva = static_cast<std::uint32_t>(function.UnwindData);
    // Follow chains: a chained entry's own unwind info is needed too.
    for (int depth = 0; depth < 32 && rva != 0 && (rva & 1u) == 0u; ++depth) {
      if (!seen.insert(rva).second) {
        break;
      }
      std::uint32_t chained = 0;
      const std::uint32_t size = sizeAt(rva, &chained);
      if (size == 0) {
        break;
      }
      ranges.push_back(ExtraDumpMemory{ imageBase + rva, size });
      rva = chained;
    }
  }
  return MergeExtraDumpMemory(std::move(ranges));
}

std::vector<ExtraDumpMemory> CollectModuleUnwindMemory(HANDLE process)
{
  if (!process) {
    return {};
  }
  const std::wstring windowsPrefix = WindowsDirectoryPrefix();
  if (windowsPrefix.empty()) {
    return {};
  }
  DWORD needed = 0;
  std::vector<HMODULE> modules(1024);
  if (!EnumProcessModulesEx(process, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed,
        LIST_MODULES_64BIT)) {
    return {};
  }
  if (needed > modules.size() * sizeof(HMODULE)) {
    modules.resize(needed / sizeof(HMODULE));
    if (!EnumProcessModulesEx(process, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed,
          LIST_MODULES_64BIT)) {
      return {};
    }
  }
  modules.resize(std::min<std::size_t>(modules.size(), needed / sizeof(HMODULE)));

  std::vector<ExtraDumpMemory> ranges;
  std::size_t total = 0;
  for (const HMODULE module : modules) {
    wchar_t path[MAX_PATH]{};
    MODULEINFO info{};
    if (GetModuleFileNameExW(process, module, path, MAX_PATH) == 0 ||
        !GetModuleInformation(process, module, &info, sizeof(info))) {
      continue;
    }
    if (Lower(path).rfind(windowsPrefix, 0) != 0) {
      continue;  // game, SKSE and mod files do not change under a Windows update
    }
    std::vector<ExtraDumpMemory> moduleRanges;
    const std::size_t bytes = AddModuleUnwindMemory(
      process, reinterpret_cast<std::uint64_t>(info.lpBaseOfDll), info.SizeOfImage, moduleRanges);
    if (total + bytes > kModuleUnwindMemoryBudget) {
      continue;  // a smaller module later may still fit
    }
    total += bytes;
    ranges.insert(ranges.end(), moduleRanges.begin(), moduleRanges.end());
  }
  return MergeExtraDumpMemory(std::move(ranges));
}

}  // namespace skydiag::helper::internal
