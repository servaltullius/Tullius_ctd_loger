#include "AnalyzerInternalsStackwalkPriv.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace skydiag::dump_tool::internal::stackwalk_internal {
namespace {

// Export names further than this from an address are not trusted for it (as
// for DbgHelp's own export symbols, see FormatFrame).
constexpr std::uint32_t kMaxExportSize = 0x2000;
constexpr std::uint32_t kMaxExports = 1u << 16;

}  // namespace

bool IgnoreLocalImagesForTesting()
{
  static const bool ignore = [] {
    char value[8]{};
    std::size_t length = 0;
    return getenv_s(&length, value, sizeof(value), "SKYDIAG_STACKWALK_IGNORE_LOCAL_IMAGES") == 0 && length > 0 &&
      value[0] == '1';
  }();
  return ignore;
}

bool LocalFileMatchesModule(const minidump::ModuleInfo& module)
{
  if (IgnoreLocalImagesForTesting()) {
    return false;
  }
  // The path comes from the dump; DbgHelp then opens the file there too
  // (review M16).
  if (module.path.empty() || !minidump::IsLocalFixedDrivePath(module.path)) {
    return false;
  }
  if (module.time_date_stamp == 0) {
    return true;  // nothing to compare against
  }
  std::ifstream file(module.path, std::ios::binary);
  if (!file) {
    return false;
  }
  char header[0x1000]{};
  file.read(header, sizeof(header));
  const auto got = static_cast<std::size_t>(file.gcount());
  if (got < sizeof(IMAGE_DOS_HEADER)) {
    return false;
  }
  IMAGE_DOS_HEADER dos{};
  std::memcpy(&dos, header, sizeof(dos));
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
      static_cast<std::size_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > got) {
    return false;
  }
  IMAGE_NT_HEADERS64 nt{};
  std::memcpy(&nt, header + dos.e_lfanew, sizeof(nt));
  return nt.Signature == IMAGE_NT_SIGNATURE && nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
    nt.FileHeader.TimeDateStamp == module.time_date_stamp &&
    static_cast<std::uint64_t>(nt.OptionalHeader.SizeOfImage) == (module.end - module.base);
}

DumpImageData::DumpImageData(const MinidumpMemoryView& memory, const std::vector<minidump::ModuleInfo>& modules)
  : memory_(memory), modules_(&modules)
{
  memory_.image_fallback = nullptr;
  memory_.dump_images = nullptr;
}

bool DumpImageData::Read(std::uint64_t addr, void* dst, std::size_t n) const
{
  std::size_t got = 0;
  return memory_.Read(addr, dst, n, got) && got == n;
}

const minidump::ModuleInfo* DumpImageData::ModuleFor(std::uint64_t addr) const
{
  for (const auto& module : *modules_) {
    if (addr >= module.base && addr < module.end) {
      return &module;
    }
  }
  return nullptr;
}

const DumpImageData::Directories& DumpImageData::DirectoriesFor(const minidump::ModuleInfo& module) const
{
  const auto it = directories_.find(module.base);
  if (it != directories_.end()) {
    return it->second;
  }
  Directories dirs{};
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 nt{};
  const std::uint64_t imageSize = module.end - module.base;
  if (Read(module.base, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
      Read(module.base + static_cast<std::uint32_t>(dos.e_lfanew), &nt, sizeof(nt)) &&
      nt.Signature == IMAGE_NT_SIGNATURE && nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
      // The dump's own module record must describe the same build.
      (module.time_date_stamp == 0 || nt.FileHeader.TimeDateStamp == module.time_date_stamp) &&
      nt.OptionalHeader.SizeOfImage == imageSize) {
    const auto& exceptions = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const auto& exports = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (static_cast<std::uint64_t>(exceptions.VirtualAddress) + exceptions.Size <= imageSize) {
      dirs.exception_rva = exceptions.VirtualAddress;
      dirs.exception_size = exceptions.Size;
    }
    if (static_cast<std::uint64_t>(exports.VirtualAddress) + exports.Size <= imageSize) {
      dirs.export_rva = exports.VirtualAddress;
      dirs.export_size = exports.Size;
    }
    dirs.ok = true;
  }
  return directories_.emplace(module.base, dirs).first->second;
}

const RUNTIME_FUNCTION* DumpImageData::FindFunctionEntry(std::uint64_t addr) const
{
  const auto* module = ModuleFor(addr);
  if (!module) {
    return nullptr;
  }
  const auto& dirs = DirectoriesFor(*module);
  if (!dirs.ok || dirs.exception_size < sizeof(RUNTIME_FUNCTION)) {
    return nullptr;
  }
  const auto rva = static_cast<std::uint32_t>(addr - module->base);
  const std::uint64_t table = module->base + dirs.exception_rva;
  std::size_t lo = 0;
  std::size_t hi = dirs.exception_size / sizeof(RUNTIME_FUNCTION);
  RUNTIME_FUNCTION entry{};
  bool found = false;
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (!Read(table + mid * sizeof(RUNTIME_FUNCTION), &entry, sizeof(entry))) {
      return nullptr;  // the dump does not carry this module's .pdata
    }
    if (rva < entry.BeginAddress) {
      hi = mid;
    } else if (rva >= entry.EndAddress) {
      lo = mid + 1;
    } else {
      found = true;
      break;
    }
  }
  if (!found) {
    return nullptr;  // a leaf function
  }
  // Low bit set: the entry points at another .pdata entry with the unwind data.
  if ((entry.UnwindData & 1u) != 0u &&
      !Read(module->base + (entry.UnwindData & ~1u), &entry, sizeof(entry))) {
    return nullptr;
  }
  auto& slot = entries_[next_entry_++ % entries_.size()];
  slot = entry;
  return &slot;
}

std::vector<DumpImageData::Export> DumpImageData::ReadExports(const minidump::ModuleInfo& module) const
{
  std::vector<Export> exports;
  const auto& dirs = DirectoriesFor(module);
  if (!dirs.ok || dirs.export_size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
    return exports;
  }
  IMAGE_EXPORT_DIRECTORY directory{};
  if (!Read(module.base + dirs.export_rva, &directory, sizeof(directory)) || directory.NumberOfNames > kMaxExports ||
      directory.NumberOfFunctions > kMaxExports) {
    return exports;
  }
  std::vector<DWORD> functions(directory.NumberOfFunctions);
  std::vector<DWORD> names(directory.NumberOfNames);
  std::vector<WORD> ordinals(directory.NumberOfNames);
  if ((!functions.empty() &&
       !Read(module.base + directory.AddressOfFunctions, functions.data(), functions.size() * sizeof(DWORD))) ||
      (!names.empty() && !Read(module.base + directory.AddressOfNames, names.data(), names.size() * sizeof(DWORD))) ||
      (!ordinals.empty() &&
       !Read(module.base + directory.AddressOfNameOrdinals, ordinals.data(), ordinals.size() * sizeof(WORD)))) {
    return exports;
  }
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (ordinals[i] >= functions.size()) {
      continue;
    }
    const DWORD rva = functions[ordinals[i]];
    if (rva == 0 || (rva >= dirs.export_rva && rva < dirs.export_rva + dirs.export_size)) {
      continue;  // forwarded to another DLL
    }
    std::string name;
    char piece[64]{};
    for (std::uint32_t offset = 0; offset < 512; offset += sizeof(piece)) {
      if (!Read(module.base + names[i] + offset, piece, sizeof(piece))) {
        name.clear();
        break;
      }
      const auto* end = static_cast<const char*>(std::memchr(piece, 0, sizeof(piece)));
      name.append(piece, end ? static_cast<std::size_t>(end - piece) : sizeof(piece));
      if (end) {
        break;
      }
    }
    if (!name.empty()) {
      exports.push_back(Export{ rva, 0, std::move(name) });
    }
  }
  std::sort(exports.begin(), exports.end(), [](const Export& a, const Export& b) { return a.rva < b.rva; });
  // Aliases share an RVA; keep the first name.
  exports.erase(std::unique(exports.begin(), exports.end(), [](const Export& a, const Export& b) { return a.rva == b.rva; }),
    exports.end());
  const auto imageSize = static_cast<std::uint32_t>(module.end - module.base);
  for (std::size_t i = 0; i < exports.size(); ++i) {
    const std::uint32_t next = i + 1 < exports.size() ? exports[i + 1].rva : imageSize;
    exports[i].size = std::min(next - exports[i].rva, kMaxExportSize);
  }
  return exports;
}

void AddDumpExportSymbols(const SymSession& session, const DumpImageData& images)
{
  if (!session.ok || !session.process) {
    return;
  }
  for (const auto* module : session.virtualModules) {
    for (const auto& item : images.ReadExports(*module)) {
      std::wstring name(item.name.begin(), item.name.end());
      SymAddSymbolW(session.process, module->base, name.c_str(), module->base + item.rva, item.size, 0);
    }
  }
}

}  // namespace skydiag::dump_tool::internal::stackwalk_internal
