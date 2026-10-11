#pragma once

#include "AnalyzerInternals.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace skydiag::dump_tool::internal::stackwalk_internal {

struct MinidumpMemoryRange
{
  std::uint64_t start = 0;
  std::uint64_t end = 0;
  const std::uint8_t* bytes = nullptr;  // points into mapped dump file
};

// Read-only image-layout views of local module files. Used only for memory the
// dump does not contain (typically unwind data of system DLLs), and only when
// the local file has the dump module's TimeDateStamp and SizeOfImage.
class LocalImageMemory
{
public:
  explicit LocalImageMemory(const std::vector<minidump::ModuleInfo>& modules);
  ~LocalImageMemory();
  LocalImageMemory(const LocalImageMemory&) = delete;
  LocalImageMemory& operator=(const LocalImageMemory&) = delete;

  bool Read(std::uint64_t addr, void* dst, std::size_t n, std::size_t& outRead) const;

  // x64 unwind entry (.pdata) covering addr, or nullptr when the module has no
  // verified local image or the address is in a leaf function. The pointer
  // stays valid for the lifetime of this object.
  const RUNTIME_FUNCTION* FindFunctionEntry(std::uint64_t addr) const;

  // True only when the module has a verified local image and addr is not in
  // one of its executable sections (for example a vtable in .rdata). False
  // when that cannot be checked.
  bool IsKnownNonCodeAddress(std::uint64_t addr) const;

private:
  struct Image
  {
    const minidump::ModuleInfo* module = nullptr;
    bool attempted = false;
    HMODULE handle = nullptr;
    const std::uint8_t* view = nullptr;
  };
  const Image* Resolve(std::uint64_t addr) const;

  mutable std::vector<Image> images_;
};

class DumpImageData;

// True when SKYDIAG_STACKWALK_IGNORE_LOCAL_IMAGES=1: local module files are
// treated as not matching, as after a Windows update. Tests use it to prove a
// dump carries what its stack walk needs (ADR-0011).
bool IgnoreLocalImagesForTesting();

// True when the local file at module.path is the same build as the dump's
// module (TimeDateStamp and SizeOfImage), or the dump does not say.
bool LocalFileMatchesModule(const minidump::ModuleInfo& module);

struct MinidumpMemoryView
{
  std::vector<MinidumpMemoryRange> ranges;
  const LocalImageMemory* image_fallback = nullptr;
  // Unwind entries from the dump's own .pdata, for modules whose local file
  // does not match (ADR-0011).
  const DumpImageData* dump_images = nullptr;

  bool Init(void* dumpBase, std::uint64_t dumpSize, const std::vector<minidump::ThreadRecord>* threads);

  bool Read(std::uint64_t addr, void* dst, std::size_t n, std::size_t& outRead) const;
};

// ADR-0011: PE data that the dump itself carries for a module (headers,
// exception directory, export directory). The helper adds it for Windows
// modules; crash dumps also carry it for modules with code segments.
class DumpImageData
{
public:
  struct Export
  {
    std::uint32_t rva = 0;
    std::uint32_t size = 0;  // up to the next export, capped
    std::string name;
  };

  DumpImageData(const MinidumpMemoryView& memory, const std::vector<minidump::ModuleInfo>& modules);

  // x64 unwind entry covering addr from the dump's .pdata, or nullptr. The
  // pointer stays valid for the next 15 calls.
  const RUNTIME_FUNCTION* FindFunctionEntry(std::uint64_t addr) const;

  // Named exports from the dump's export directory, sorted by RVA; forwarders
  // are left out.
  std::vector<Export> ReadExports(const minidump::ModuleInfo& module) const;

private:
  struct Directories
  {
    bool ok = false;
    std::uint32_t exception_rva = 0;
    std::uint32_t exception_size = 0;
    std::uint32_t export_rva = 0;
    std::uint32_t export_size = 0;
  };

  bool Read(std::uint64_t addr, void* dst, std::size_t n) const;
  const minidump::ModuleInfo* ModuleFor(std::uint64_t addr) const;
  const Directories& DirectoriesFor(const minidump::ModuleInfo& module) const;

  MinidumpMemoryView memory_;  // dump ranges only
  const std::vector<minidump::ModuleInfo>* modules_ = nullptr;
  mutable std::unordered_map<std::uint64_t, Directories> directories_;
  mutable std::array<RUNTIME_FUNCTION, 16> entries_{};
  mutable std::size_t next_entry_ = 0;
};

struct SymSession
{
  HANDLE process = nullptr;
  HMODULE ownedMsdiaModule = nullptr;
  bool ok = false;
  std::wstring searchPath;
  std::wstring cachePath;
  std::wstring dbghelpPath;
  std::wstring dbghelpVersion;
  std::wstring msdiaPath;
  bool msdiaAvailable = false;
  bool symbolCacheReady = false;
  bool runtimeDegraded = false;
  bool usedOnlineSymbolSource = false;
  std::vector<std::wstring> runtimeDiagnostics;
  // Modules loaded without their local file (it was missing or a different
  // build); AddDumpExportSymbols names them from the dump (ADR-0011).
  std::vector<const minidump::ModuleInfo*> virtualModules;
  std::unique_lock<std::mutex> dbghelp_lock;

  explicit SymSession(const std::vector<minidump::ModuleInfo>& modules, bool allowOnlineSymbols);
  ~SymSession();
};

// Adds the dump's export names for the session's virtual modules.
void AddDumpExportSymbols(const SymSession& session, const DumpImageData& images);

std::vector<std::uint64_t> StackWalkAddrsForContext(
  HANDLE process,
  const MinidumpMemoryView& mem,
  const CONTEXT& inCtx,
  std::size_t maxFrames);

}  // namespace skydiag::dump_tool::internal::stackwalk_internal
